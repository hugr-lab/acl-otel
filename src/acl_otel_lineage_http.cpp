// spec 018: the OpenLineage transport's HTTP client - the SDK's (curl), the one the OTLP/HTTP
// exporters already link: no new dependency. Apart from the rendering (acl_otel_lineage.cpp) and the
// key (acl_otel_lineage_key.cpp), which the tests compile without the SDK.

#include "acl_otel_lineage.hpp"

#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/ext/http/client/http_client_factory.h"

#include <chrono>
#include <future>

namespace duckdb {
namespace acl_otel {

namespace http = opentelemetry::ext::http::client;

namespace {

//! The answer of one request, set once by whichever callback ends it.
class OneAnswer : public http::EventHandler {
public:
	void OnResponse(http::Response &response) noexcept override {
		LineagePostResult result;
		result.status = int(response.GetStatusCode());
		auto &body = response.GetBody();
		result.error = string(body.begin(), body.end()).substr(0, 512);
		Set(std::move(result));
	}
	void OnEvent(http::SessionState state, opentelemetry::nostd::string_view reason) noexcept override {
		switch (state) {
		case http::SessionState::Created:
		case http::SessionState::Connecting:
		case http::SessionState::Connected:
		case http::SessionState::Sending:
		case http::SessionState::Response:
		case http::SessionState::Destroyed:
			return;
		default:
			break;
		}
		LineagePostResult result;
		result.error = "no answer: " + string(reason.data(), reason.size());
		Set(std::move(result));
	}
	std::future<LineagePostResult> Future() {
		return promise.get_future();
	}

private:
	void Set(LineagePostResult result) {
		std::lock_guard<std::mutex> guard(lock);
		if (!done) {
			done = true;
			promise.set_value(std::move(result));
		}
	}
	std::mutex lock;
	bool done = false;
	std::promise<LineagePostResult> promise;
};

//! `scheme://host[:port]` and the path (+ query) the session's request carries - without its leading
//! slash: the curl session's base already ends in one, and `//api/...` is another path to a server
void SplitUrl(const string &url, string &origin, string &path) {
	auto scheme = url.find("://");
	auto start = scheme == string::npos ? 0 : scheme + 3;
	auto slash = url.find('/', start);
	origin = slash == string::npos ? url : url.substr(0, slash);
	path = slash == string::npos ? string() : url.substr(slash + 1);
}

} // namespace

LineagePoster MakeSdkLineagePoster(const string &certificate) {
	auto client = http::HttpClientFactory::Create();
	return
	    [client, certificate](const string &url, const string &body, const vector<std::pair<string, string>> &headers,
	                          int64_t timeout_ms) -> LineagePostResult {
		    string origin, path;
		    SplitUrl(url, origin, path);
		    auto session = client->CreateSession(origin);
		    auto request = session->CreateRequest();
		    request->SetMethod(http::Method::Post);
		    request->SetUri(path);
		    http::HttpSslOptions ssl(url, false, certificate, "", "", "", "", "", "", "", "", "");
		    request->SetSslOptions(ssl);
		    http::Body payload(body.begin(), body.end());
		    request->SetBody(payload);
		    for (auto &header : headers) {
			    request->AddHeader(header.first, header.second);
		    }
		    request->SetTimeoutMs(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 5000));
		    auto answer = std::make_shared<OneAnswer>();
		    auto future = answer->Future();
		    session->SendRequest(answer);
		    // the client times the request out itself; this bound only keeps a lost callback from
		    // holding the lane forever
		    auto bound = std::chrono::milliseconds((timeout_ms > 0 ? timeout_ms : 5000) + 5000);
		    if (future.wait_for(bound) != std::future_status::ready) {
			    session->CancelSession();
			    LineagePostResult lost;
			    lost.error = "no answer within the timeout";
			    return lost;
		    }
		    auto result = future.get();
		    session->FinishSession();
		    return result;
	    };
}

} // namespace acl_otel
} // namespace duckdb
