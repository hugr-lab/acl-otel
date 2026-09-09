// The OTLP transport (spec 002), against fake receivers in-process: HTTP/protobuf on the bundled
// httplib and gRPC on the SDK's generated LogsService - every attribute of R1.1 for each kind of
// event, the trace context of R1.2 (a malformed value sets neither id), the resource of R1.3, the
// headers from the environment (R9.2: names in the status, values on the wire), an unreachable
// endpoint counted as an export error. Built by CMake (the SDK links); run via `make test-cpp`.

#include "acl_otel_otlp.hpp"
#include "acl_otel_test_util.hpp"

#include "httplib.hpp"
#include "opentelemetry/proto/collector/logs/v1/logs_service.grpc.pb.h"
#include "opentelemetry/proto/collector/logs/v1/logs_service.pb.h"

#include <grpcpp/grpcpp.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

using namespace duckdb;
using namespace acl_otel_test;
namespace proto_logs = opentelemetry::proto::collector::logs::v1;

namespace {

//! One received log record, flattened the way a backend would index it
struct Received {
	int severity = 0;
	std::string body;
	std::map<std::string, std::string> attributes; // ints rendered as text
	std::string trace_id_hex, span_id_hex;
	uint64_t time_unix_nano = 0;
};

struct Capture {
	std::mutex lock;
	std::map<std::string, std::string> resource;
	std::string scope_name, scope_version;
	std::vector<Received> records;
	std::map<std::string, std::string> headers; // the request's, lower-cased keys
	int requests = 0;

	void Take(const proto_logs::ExportLogsServiceRequest &request) {
		std::lock_guard<std::mutex> guard(lock);
		requests++;
		for (auto &rl : request.resource_logs()) {
			for (auto &kv : rl.resource().attributes()) {
				resource[kv.key()] = kv.value().string_value();
			}
			for (auto &sl : rl.scope_logs()) {
				scope_name = sl.scope().name();
				scope_version = sl.scope().version();
				for (auto &lr : sl.log_records()) {
					Received r;
					r.severity = lr.severity_number();
					r.body = lr.body().string_value();
					r.time_unix_nano = lr.time_unix_nano();
					for (auto &kv : lr.attributes()) {
						auto &v = kv.value();
						r.attributes[kv.key()] = v.has_int_value() ? std::to_string(v.int_value()) : v.string_value();
					}
					static const char *hex = "0123456789abcdef";
					for (unsigned char c : lr.trace_id()) {
						r.trace_id_hex += hex[c >> 4];
						r.trace_id_hex += hex[c & 15];
					}
					for (unsigned char c : lr.span_id()) {
						r.span_id_hex += hex[c >> 4];
						r.span_id_hex += hex[c & 15];
					}
					records.push_back(std::move(r));
				}
			}
		}
	}
	Received At(size_t i) { // a copy: the checks index its maps freely
		std::lock_guard<std::mutex> guard(lock);
		return records.at(i);
	}
	size_t Count() {
		std::lock_guard<std::mutex> guard(lock);
		return records.size();
	}
};

//! the HTTP receiver: POST /v1/logs, protobuf in, an empty ExportLogsServiceResponse out
struct HttpReceiver {
	Capture capture;
	duckdb_httplib::Server server;
	int port = 0;
	std::thread thread;
	HttpReceiver() {
		server.Post("/v1/logs", [this](const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
			proto_logs::ExportLogsServiceRequest request;
			if (!request.ParseFromString(req.body)) {
				res.status = 400;
				return;
			}
			{
				std::lock_guard<std::mutex> guard(capture.lock);
				for (auto &h : req.headers) {
					std::string key = h.first;
					for (auto &c : key) {
						c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
					}
					capture.headers[key] = h.second;
				}
			}
			capture.Take(request);
			proto_logs::ExportLogsServiceResponse response;
			res.set_content(response.SerializeAsString(), "application/x-protobuf");
		});
		port = server.bind_to_any_port("127.0.0.1");
		thread = std::thread([this] { server.listen_after_bind(); });
	}
	~HttpReceiver() {
		server.stop();
		thread.join();
	}
};

//! the gRPC receiver: the SDK's generated service, the request captured
struct GrpcReceiver : proto_logs::LogsService::Service {
	Capture capture;
	std::unique_ptr<grpc::Server> server;
	int port = 0;
	GrpcReceiver() {
		grpc::ServerBuilder builder;
		builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
		builder.RegisterService(this);
		server = builder.BuildAndStart();
	}
	~GrpcReceiver() override {
		server->Shutdown();
	}
	grpc::Status Export(grpc::ServerContext *context, const proto_logs::ExportLogsServiceRequest *request,
	                    proto_logs::ExportLogsServiceResponse *) override {
		{
			std::lock_guard<std::mutex> guard(capture.lock);
			for (auto &h : context->client_metadata()) {
				capture.headers[std::string(h.first.data(), h.first.size())] =
				    std::string(h.second.data(), h.second.size());
			}
		}
		capture.Take(*request);
		return grpc::Status::OK;
	}
};

acl::AuditEvent Base(int64_t seq, const char *kind, bool allowed) {
	acl::AuditEvent e;
	e.ts_us = 1757000000000000 + seq; // 2025-09-04T..., microseconds
	e.seq = seq;
	e.node = "node-a:42";
	e.kind = kind;
	e.allowed = allowed;
	e.level = allowed ? acl::AuditLevel::DECISIONS : acl::AuditLevel::DENIED;
	return e;
}

vector<acl::AuditEvent> SampleBatch() {
	vector<acl::AuditEvent> batch;
	auto allowed = Base(1, "statement", true);
	allowed.statement = "select";
	allowed.door = "flight";
	allowed.session = "s-1";
	allowed.principal.subject = "u7";
	allowed.principal.issuer = "https://idp.corp/x";
	allowed.principal.roles = {"analyst", "viewer"};
	allowed.principal.claims["tenant"] = "acme";           // exported only when named (spec 005)
	allowed.principal.claims["secret"] = "not for export"; // never named, never exported
	allowed.principal.claims["long"] = string(400, 'x');   // named below, and truncated
	allowed.objects.push_back(acl::AuditObject {"c.orders", "select"});
	allowed.objects.push_back(acl::AuditObject {"c.items", "select"});
	allowed.correlation_id = "corr-1";
	allowed.traceparent = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
	allowed.rewrite_us = 250;
	batch.push_back(allowed);
	auto denied = Base(2, "statement", false);
	denied.statement = "update";
	denied.door = "quack";
	denied.reason_code = "capability";
	denied.reason = "acl_rewrite: update on \"c.orders\" is not allowed";
	denied.traceparent = "not-a-traceparent";
	batch.push_back(denied);
	auto keys = Base(3, "keys", false);
	keys.detail = "refresh_failed";
	keys.reason_code = "source_error";
	keys.reason = "the keys could not be read";
	batch.push_back(keys);
	auto closed = Base(4, "session", true);
	closed.detail = "idle";
	closed.door = "flight";
	closed.duration_us = 90000000;
	batch.push_back(closed);
	auto ingest = Base(5, "ingest", true);
	ingest.door = "flight";
	ingest.rows = 10000;
	batch.push_back(ingest);
	return batch;
}

void CheckMapping(Capture &capture, const std::string &transport) {
	Check(capture.Count() == 5, transport + ": five records received");
	Check(capture.resource["service.name"] == "acl-test" && capture.resource["service.instance.id"] == "node-a:42" &&
	          !capture.resource["duckdb.version"].empty() && capture.resource["deployment"] == "test",
	      transport + ": the resource carries service.name, service.instance.id, duckdb.version and the extras");
	Check(capture.scope_name == "acl_otel", transport + ": the scope is acl_otel");
	auto allowed = capture.At(0);
	Check(allowed.severity == 9 && allowed.body == "statement allowed", transport + ": allowed -> INFO, the body");
	Check(allowed.attributes["acl.kind"] == "statement" && allowed.attributes["acl.statement"] == "select" &&
	          allowed.attributes["acl.verdict"] == "allowed" && allowed.attributes["acl.door"] == "flight" &&
	          allowed.attributes["acl.session"] == "s-1" && allowed.attributes["acl.subject"] == "u7" &&
	          allowed.attributes["acl.issuer"] == "https://idp.corp/x" &&
	          allowed.attributes["acl.roles"] == "[\"analyst\",\"viewer\"]" &&
	          allowed.attributes["acl.objects"] == "[{\"name\":\"c.orders\",\"capability\":\"select\"},{\"name\":\"c."
	                                               "items\",\"capability\":\"select\"}]" &&
	          allowed.attributes["acl.correlation_id"] == "corr-1" && allowed.attributes["acl.rewrite_us"] == "250" &&
	          allowed.attributes["acl.level"] == "decisions" && allowed.attributes["acl.seq"] == "1" &&
	          allowed.attributes["acl.node"] == "node-a:42",
	      transport + ": every attribute of R1.1 on the allowed statement");
	Check(allowed.attributes.count("acl.reason_code") == 0 && allowed.attributes.count("acl.rows") == 0 &&
	          allowed.attributes.count("acl.duration_us") == 0,
	      transport + ": what the event does not carry is absent, not empty");
	Check(allowed.attributes.count("acl.claim.tenant") == 0 && allowed.attributes.count("tenant") == 0,
	      transport + ": a claim value is not exported when no setting names it (C5, spec 005)");
	Check(allowed.trace_id_hex == "0af7651916cd43dd8448eb211c80319c" && allowed.span_id_hex == "b7ad6b7169203331",
	      transport + ": the traceparent sets trace and span id (R1.2)");
	Check(allowed.time_unix_nano == 1757000000000001000ULL, transport + ": the timestamp is the event's ts_us");
	auto denied = capture.At(1);
	Check(denied.severity == 13 &&
	          denied.body == "statement denied: acl_rewrite: update on \"c.orders\" is not allowed",
	      transport + ": denied -> WARN, the reason in the body");
	Check(denied.attributes["acl.reason_code"] == "capability" &&
	          denied.attributes["acl.reason"].rfind("acl_rewrite:", 0) == 0,
	      transport + ": reason_code and reason as attributes");
	Check(denied.trace_id_hex.empty() && denied.span_id_hex.empty(),
	      transport + ": a malformed traceparent sets neither id");
	auto keys = capture.At(2);
	Check(keys.severity == 17 && keys.attributes["acl.detail"] == "refresh_failed",
	      transport + ": a failed keys read -> ERROR");
	Check(capture.At(3).attributes["acl.duration_us"] == "90000000" && capture.At(3).attributes["acl.detail"] == "idle",
	      transport + ": a session close carries its duration");
	Check(capture.At(4).attributes["acl.rows"] == "10000", transport + ": an ingest carries its rows");
}

acl_otel::OtlpConfig Config(const std::string &endpoint, const std::string &protocol) {
	acl_otel::OtlpConfig config;
	config.endpoint = endpoint;
	config.protocol = protocol;
	config.timeout_s = 5;
	config.insecure = true;
	config.service_name = "acl-test";
	config.instance_id = "node-a:42";
	config.resource_attributes = "deployment=test, empty=";
	config.duckdb_version = "2.0.0-test";
	config.acl_otel_version = "0.0.1-test";
	return config;
}

} // namespace

int main() {
	std::printf("test_acl_otel_otlp\n");
	{
		HttpReceiver receiver;
		acl_otel::OtlpExporter exporter(Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf"));
		Check(exporter.Describe() == "otlp/http -> http://127.0.0.1:" + std::to_string(receiver.port) + "/v1/logs",
		      "http: the description names the endpoint with the logs path");
		string error;
		bool exported = exporter.Export(SampleBatch(), error);
		Check(exported, "http: the batch is exported (" + error + ")");
		CheckMapping(receiver.capture, "http");
		Check(receiver.capture.headers.count("authorization") == 0, "http: no header nobody configured");
	}
	{
		// headers from the environment only (R9.2): on the wire, names in the status
		setenv("OTEL_EXPORTER_OTLP_HEADERS", "authorization=Bearer secret-token,x-tenant=acme", 1);
		HttpReceiver receiver;
		acl_otel::OtlpExporter exporter(Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf"));
		auto names = exporter.HeaderNames();
		bool named = false, leaked = false;
		for (auto &n : names) {
			named = named || n == "authorization";
			leaked = leaked || n.find("secret") != std::string::npos;
		}
		Check(named && !leaked, "http: the status names the header, never its value");
		string error;
		Check(exporter.Export(SampleBatch(), error), "http: exported with headers");
		Check(receiver.capture.headers["authorization"] == "Bearer secret-token" &&
		          receiver.capture.headers["x-tenant"] == "acme",
		      "http: the headers reached the receiver");
		unsetenv("OTEL_EXPORTER_OTLP_HEADERS");
	}
	{
		GrpcReceiver receiver;
		acl_otel::OtlpExporter exporter(Config("127.0.0.1:" + std::to_string(receiver.port), "grpc"));
		Check(exporter.Describe() == "otlp/grpc -> 127.0.0.1:" + std::to_string(receiver.port),
		      "grpc: the description names the endpoint");
		string error;
		bool exported = exporter.Export(SampleBatch(), error);
		Check(exported, "grpc: the batch is exported (" + error + ")");
		CheckMapping(receiver.capture, "grpc");
	}
	{
		acl_otel::OtlpExporter exporter(Config("http://127.0.0.1:1", "http/protobuf"));
		string error;
		bool exported = exporter.Export(SampleBatch(), error);
		Check(!exported && error.find("the export failed") == 0,
		      "http: a port nobody listens on fails with a reason (" + error + ")");
		Check(error.find(exporter.Describe()) != std::string::npos,
		      "http: the error names the transport that failed (" + error + ")");
	}
	{
		// the same over grpc: a channel to a port nobody listens on fails within the timeout
		auto config = Config("127.0.0.1:1", "grpc");
		config.timeout_s = 2;
		acl_otel::OtlpExporter exporter(config);
		string error;
		bool exported = exporter.Export(SampleBatch(), error);
		Check(!exported && error.find("the export failed") == 0,
		      "grpc: a port nobody listens on fails with a reason (" + error + ")");
	}
	{
		// a wide statement: the list attributes stay documents a backend accepts whole (8 KB), whole
		// elements only, and what did not fit is counted in the last element
		acl::AuditEvent wide = Base(9, "statement", true);
		wide.statement = "select";
		for (int i = 0; i < 500; i++) {
			wide.objects.push_back(
			    acl::AuditObject {"catalog.schema.a_table_with_a_long_name_" + std::to_string(i), "select"});
			wide.principal.roles.push_back("a_role_with_a_fairly_long_name_" + std::to_string(i));
		}
		auto objects = acl_otel::ObjectsJson(wide);
		auto roles = acl_otel::RolesJson(wide);
		Check(objects.size() <= 8192 && roles.size() <= 8192, "both lists stay within the 8 KB a backend keeps");
		Check(objects.find("{\"truncated\":") != std::string::npos && objects.back() == ']' &&
		          roles.find("{\"truncated\":") != std::string::npos,
		      "...and each says how many did not fit");
		Check(objects.find("_39\",") != std::string::npos &&
		          objects.find("a_table_with_a_long_name_499") == std::string::npos,
		      "whole elements only: an early one is there, the last is not");
		acl::AuditEvent narrow = Base(10, "statement", true);
		narrow.objects.push_back(acl::AuditObject {"c.orders", "select"});
		Check(acl_otel::ObjectsJson(narrow) == "[{\"name\":\"c.orders\",\"capability\":\"select\"}]" &&
		          acl_otel::RolesJson(narrow) == "[]",
		      "a list that fits is unchanged, and an empty one is []");
	}
	{
		// spec 005 (R5.1): the claims an operator named, and only those
		HttpReceiver receiver;
		auto config = Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf");
		config.claim_attributes = acl_otel::ParseClaimAttributes("tenant, long");
		acl_otel::OtlpExporter exporter(config);
		string error;
		Check(exporter.Export(SampleBatch(), error), "claims: the batch is exported (" + error + ")");
		auto allowed = receiver.capture.At(0);
		Check(allowed.attributes["acl.claim.tenant"] == "acme", "the claim on the list is an acl.claim.<name>");
		Check(allowed.attributes.count("acl.claim.secret") == 0 && allowed.attributes.count("secret") == 0,
		      "...and one the list does not name is nowhere on the record");
		auto truncated = allowed.attributes["acl.claim.long"];
		Check(truncated.size() == 256 && truncated.rfind("...") == 253,
		      "a claim longer than 256 bytes is cut with an ellipsis (" + std::to_string(truncated.size()) + ")");
		Check(acl_otel::ParseClaimAttributes("a,b, c ,").size() == 3, "the list is comma separated and trimmed");
		bool too_many = false;
		try {
			acl_otel::ParseClaimAttributes("a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q");
		} catch (std::exception &) {
			too_many = true;
		}
		Check(too_many, "more than sixteen names is refused");
	}
	{
		bool refused = false;
		try {
			acl_otel::OtlpExporter exporter(Config("http://127.0.0.1:1", "carrier-pigeon"));
		} catch (std::exception &) {
			refused = true;
		}
		Check(refused, "a protocol that is not one of the two is refused");
	}
	{
		// a protocol left unset is the environment's, else http/protobuf; a timeout left unset is the SDK's
		auto config = Config("127.0.0.1:1", "");
		config.timeout_s = 0;
		unsetenv("OTEL_EXPORTER_OTLP_PROTOCOL");
		unsetenv("OTEL_EXPORTER_OTLP_LOGS_PROTOCOL");
		Check(config.ResolvedProtocol() == "http/protobuf", "no protocol anywhere -> http/protobuf");
		setenv("OTEL_EXPORTER_OTLP_PROTOCOL", "grpc", 1);
		Check(config.ResolvedProtocol() == "grpc", "OTEL_EXPORTER_OTLP_PROTOCOL=grpc is honoured when unset here");
		acl_otel::OtlpExporter exporter(config);
		Check(exporter.Describe() == "otlp/grpc -> 127.0.0.1:1", "...and the transport built is grpc");
		config.protocol = "http/protobuf";
		Check(config.ResolvedProtocol() == "http/protobuf", "a setting that is set wins over its variable");
		unsetenv("OTEL_EXPORTER_OTLP_PROTOCOL");
		acl_otel::OtlpExporter masked(Config("https://svc:token@collector.example:4318", "http/protobuf"));
		Check(masked.Describe() == "otlp/http -> https://***@collector.example:4318/v1/logs",
		      "a credential in the URL is masked in the description (R9.2)");
	}
	std::printf("PASS\n");
	return 0;
}
