// Spec 011: tresor's audit (duckdb-ext-common TRSA 1) through this extension, against a fake OTLP
// receiver in-process (HTTP/protobuf on the bundled httplib, `/v1/logs` and `/v1/traces`). What it
// asserts: the record per event (scope `tresor`, WARN for a refusal or a failure, `tresor.*`
// attributes, the caller's trace context, no duration for an event that made no call); the span only
// where the caller's sampled trace and a measured call meet (`tresor.<kind>`, a child of the caller's
// span, [ts_us - duration_us, ts_us], kError on denied/error, an id that never collides with a
// decision span's); and the state: attached beside the base, fed by tresor's registry the way tresor
// delivers, tresor's counters in the scrape, a registry of another contract version refused and said.
// Built by CMake (the SDK links); run via `make test-cpp`.

#include "acl_otel_otlp.hpp"
#include "acl_otel_metrics.hpp"
#include "acl_otel_test_util.hpp"

#include "duckdb.hpp"
#include "httplib.hpp"
#include "opentelemetry/proto/collector/logs/v1/logs_service.pb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"

#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

using namespace duckdb;
using namespace acl_otel_test;
namespace proto_logs = opentelemetry::proto::collector::logs::v1;
namespace proto_trace = opentelemetry::proto::collector::trace::v1;

namespace {

string Hex(const std::string &bytes) {
	static const char *hex = "0123456789abcdef";
	string out;
	for (unsigned char c : bytes) {
		out += hex[c >> 4];
		out += hex[c & 15];
	}
	return out;
}

string Attr(const opentelemetry::proto::common::v1::AnyValue &v) {
	if (v.has_int_value()) {
		return std::to_string(v.int_value());
	}
	if (v.has_bool_value()) {
		return v.bool_value() ? "true" : "false";
	}
	return v.string_value();
}

struct Record {
	string scope, body, trace_id, span_id;
	int severity = 0;
	std::map<string, string> attributes;
};

struct Span {
	string scope, name, trace_id, span_id, parent_span_id;
	uint64_t start_ns = 0, end_ns = 0;
	int status = 0;
	std::map<string, string> attributes;
};

struct Receiver {
	std::mutex lock;
	vector<Record> records;
	vector<Span> spans;
	duckdb_httplib::Server server;
	int port = 0;
	std::thread thread;

	Receiver() {
		server.Post("/v1/logs", [this](const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
			proto_logs::ExportLogsServiceRequest request;
			request.ParseFromString(req.body);
			std::lock_guard<std::mutex> guard(lock);
			for (auto &rl : request.resource_logs()) {
				for (auto &sl : rl.scope_logs()) {
					for (auto &log : sl.log_records()) {
						Record r;
						r.scope = sl.scope().name();
						r.body = log.body().string_value();
						r.severity = log.severity_number();
						r.trace_id = Hex(log.trace_id());
						r.span_id = Hex(log.span_id());
						for (auto &kv : log.attributes()) {
							r.attributes[kv.key()] = Attr(kv.value());
						}
						records.push_back(std::move(r));
					}
				}
			}
			res.set_content(proto_logs::ExportLogsServiceResponse().SerializeAsString(), "application/x-protobuf");
		});
		server.Post("/v1/traces", [this](const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
			proto_trace::ExportTraceServiceRequest request;
			request.ParseFromString(req.body);
			std::lock_guard<std::mutex> guard(lock);
			for (auto &rs : request.resource_spans()) {
				for (auto &ss : rs.scope_spans()) {
					for (auto &span : ss.spans()) {
						Span s;
						s.scope = ss.scope().name();
						s.name = span.name();
						s.trace_id = Hex(span.trace_id());
						s.span_id = Hex(span.span_id());
						s.parent_span_id = Hex(span.parent_span_id());
						s.start_ns = span.start_time_unix_nano();
						s.end_ns = span.end_time_unix_nano();
						s.status = span.status().code();
						for (auto &kv : span.attributes()) {
							s.attributes[kv.key()] = Attr(kv.value());
						}
						spans.push_back(std::move(s));
					}
				}
			}
			res.set_content(proto_trace::ExportTraceServiceResponse().SerializeAsString(), "application/x-protobuf");
		});
		port = server.bind_to_any_port("127.0.0.1");
		thread = std::thread([this] { server.listen_after_bind(); });
	}
	~Receiver() {
		server.stop();
		thread.join();
	}
	string Endpoint() const {
		return "http://127.0.0.1:" + std::to_string(port);
	}
	vector<Record> Records() {
		std::lock_guard<std::mutex> guard(lock);
		return records;
	}
	vector<Span> Spans() {
		std::lock_guard<std::mutex> guard(lock);
		return spans;
	}
	void Clear() {
		std::lock_guard<std::mutex> guard(lock);
		records.clear();
		spans.clear();
	}
};

const char *TRACEPARENT = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
const char *TRACEPARENT_UNSAMPLED = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00";

tresor::TresorAuditEvent Event(int64_t seq, const char *kind, const char *outcome) {
	tresor::TresorAuditEvent e;
	e.seq = seq;
	e.ts_us = 1790000000000000 + seq * 1000;
	e.kind = kind;
	e.outcome = outcome;
	e.service = "corp";
	e.host = "secrets.corp:443/base";
	e.principal = "client:acl-node";
	return e;
}

//! a lookup under a session's statement, that called the service: a record and a span
tresor::TresorAuditEvent Lookup() {
	auto e = Event(1, "lookup", "ok");
	e.secret = "lake";
	e.secret_type = "s3";
	e.user = "subject:https://idp|alice";
	e.acl_session = "2A7E529B9F78";
	e.correlation_id = "req-42";
	e.traceparent = TRACEPARENT;
	e.duration_us = 3500;
	return e;
}

//! a write the service refused: WARN, and a span in error
tresor::TresorAuditEvent RefusedWrite() {
	auto e = Event(2, "write", "denied");
	e.reason_code = "no_verb";
	e.reason = "403 no_verb";
	e.secret = "lake";
	e.traceparent = TRACEPARENT;
	e.duration_us = 1200;
	return e;
}

//! a cached lookup: no call made, so a record only - never a zero-length span
tresor::TresorAuditEvent CachedLookup() {
	auto e = Event(3, "lookup", "ok");
	e.secret = "lake";
	e.cached = true;
	e.traceparent = TRACEPARENT;
	return e;
}

//! the node's own login: no statement, no trace
tresor::TresorAuditEvent Login() {
	auto e = Event(4, "login", "ok");
	e.login = "client_credentials";
	e.duration_us = 80000;
	return e;
}

tresor::TresorAuditEvent Unsampled() {
	auto e = Lookup();
	e.seq = 5;
	e.traceparent = TRACEPARENT_UNSAMPLED;
	return e;
}

tresor::TresorAuditEvent Grant() {
	auto e = Event(6, "grant", "ok");
	e.secret = "lake";
	e.target = "role:analysts";
	e.duration_us = 900;
	e.traceparent = TRACEPARENT;
	return e;
}

acl_otel::OtlpConfig Config(const string &endpoint) {
	acl_otel::OtlpConfig config;
	config.endpoint = endpoint;
	config.protocol = "http/protobuf";
	config.instance_id = "node-1";
	return config;
}

template <class F>
bool Eventually(F &&ready) {
	for (int i = 0; i < 100; i++) {
		if (ready()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return false;
}

} // namespace

int main() {
	std::printf("test_acl_otel_tresor_otlp\n");
	{
		// the gate: a span needs the caller's sampled trace AND a measured call
		bool unsampled = false;
		Check(acl_otel::TresorSpanCandidate(Lookup(), unsampled) && !unsampled, "a traced, measured lookup is a span");
		Check(!acl_otel::TresorSpanCandidate(CachedLookup(), unsampled) && !unsampled,
		      "a cached lookup made no call: no span, never a zero-length one");
		Check(!acl_otel::TresorSpanCandidate(Login(), unsampled) && !unsampled, "no traceparent: a record only");
		Check(!acl_otel::TresorSpanCandidate(Unsampled(), unsampled) && unsampled,
		      "the caller's 00 flags are obeyed, and said");
		auto broken = Lookup();
		broken.traceparent = "00-not-a-traceparent";
		Check(!acl_otel::TresorSpanCandidate(broken, unsampled) && !unsampled, "a malformed traceparent: none");
		// the id never collides with a decision span of the same node and seq
		uint8_t tresor_id[8], decision_id[8];
		acl_otel::TresorSpanId("node-1", Lookup(), tresor_id);
		acl_otel::SpanIdFor("node-1", Lookup().seq, decision_id);
		Check(std::memcmp(tresor_id, decision_id, 8) != 0, "a tresor span's id is not the decision span's");
	}

	Receiver receiver;
	{
		// the transports alone
		acl_otel::OtlpTresorExporter logs(Config(receiver.Endpoint()));
		string error;
		Check(logs.Export({Lookup(), RefusedWrite(), CachedLookup(), Login(), Grant()}, error),
		      "the records export: " + error);
		auto records = receiver.Records();
		Check(records.size() == 5, "five records, one per event");
		auto &lookup = records[0];
		Check(lookup.scope == "tresor", "under the scope `tresor`");
		Check(lookup.body == "tresor lookup ok" && lookup.severity == 9, "INFO, `tresor <kind> <outcome>`");
		Check(lookup.attributes["tresor.secret"] == "lake" && lookup.attributes["tresor.secret_type"] == "s3" &&
		          lookup.attributes["tresor.user"] == "subject:https://idp|alice" &&
		          lookup.attributes["tresor.acl_session"] == "2A7E529B9F78" &&
		          lookup.attributes["tresor.correlation_id"] == "req-42" &&
		          lookup.attributes["tresor.duration_us"] == "3500" && lookup.attributes["tresor.cached"] == "false",
		      "the event's fields as tresor.* attributes");
		Check(lookup.trace_id == "0af7651916cd43dd8448eb211c80319c" && lookup.span_id == "b7ad6b7169203331",
		      "the caller's trace context on the record");
		auto &refused = records[1];
		Check(refused.severity == 13 && refused.body == "tresor write denied: 403 no_verb" &&
		          refused.attributes["tresor.reason_code"] == "no_verb",
		      "a refusal is WARN, with tresor's words and its code");
		Check(records[2].attributes.count("tresor.duration_us") == 0 &&
		          records[2].attributes["tresor.cached"] == "true",
		      "no call, no duration");
		Check(records[3].trace_id.empty() && records[3].attributes["tresor.login"] == "client_credentials",
		      "the node's login: no trace context");
		Check(records[4].attributes["tresor.target"] == "role:analysts", "a grant names its target");

		receiver.Clear();
		acl_otel::OtlpTresorTraceExporter traces(Config(receiver.Endpoint()));
		Check(traces.Export({Lookup(), RefusedWrite()}, error), "the spans export: " + error);
		auto spans = receiver.Spans();
		Check(spans.size() == 2, "two spans");
		Check(spans[0].scope == "tresor" && spans[0].name == "tresor.lookup", "`tresor.<kind>` under `tresor`");
		Check(spans[0].trace_id == "0af7651916cd43dd8448eb211c80319c" && spans[0].parent_span_id == "b7ad6b7169203331",
		      "a child of the caller's span, in the caller's trace");
		Check(spans[0].end_ns - spans[0].start_ns == 3500 * 1000 && spans[0].end_ns == uint64_t(Lookup().ts_us) * 1000,
		      "[ts_us - duration_us, ts_us]");
		Check(spans[0].status == 0 && spans[1].status == 2, "kUnset for ok, kError for denied");
		Check(!traces.Export({CachedLookup()}, error), "a batch with an unmeasured event is refused, not faked");
	}

	{
		// the state: attached beside the base, fed the way tresor delivers
		receiver.Clear();
		setenv("OTEL_EXPORTER_OTLP_ENDPOINT", receiver.Endpoint().c_str(), 1);
		DuckDB db(nullptr);
		auto &instance = *db.instance;
		auto state = acl_otel::OtelState::Of(instance);
		Check(state->Start(instance), "the extension attaches");
		auto sink = state->TresorSink();
		Check(sink != nullptr && state->TresorError().empty(), "...and to tresor's registry, created if absent");
		state->ReconfigureTraces(instance, "acl_otel_traces", Value("linked"));
		string why;
		auto registry = tresor::TresorAuditHooks::Reach(instance.GetObjectCache(), why);
		Check(registry && registry->Sinks().size() == 1, "tresor finds our sink on its registry");
		for (auto &event : {Lookup(), RefusedWrite(), CachedLookup(), Login(), Unsampled(), Grant()}) {
			registry->GetCounters().Add(
			    "tresor.events",
			    {{"kind", event.kind}, {"outcome", event.outcome}, {"cached", event.cached ? "true" : "false"}});
			for (auto &one : registry->Sinks()) {
				one->OnEvent(event); // what tresor's delivery thread does
			}
		}
		// the operator's flushes drain tresor's lanes too: a status read right after counts them sent
		Check(state->Flush(), "acl_otel_flush drains the records, tresor's included");
		Check(sink->Records().stats.exported.load() == 6, "tresor's six records are counted sent at once");
		Check(state->FlushTraces(), "acl_otel_traces_flush drains the spans, tresor's included");
		Check(sink->Spans()->stats.exported.load() == 3, "and its three spans");
		Check(Eventually([&] { return receiver.Records().size() == 6; }), "six records");
		Check(Eventually([&] { return receiver.Spans().size() == 3; }),
		      "three spans: the lookup, the refused write, the grant - not the cached, the login, the unsampled");
		Check(sink->Spans()->stats.unsampled.load() == 1, "the unsampled one counted as such");
		bool counted = false;
		for (auto &point : state->SelfMetrics(instance)) {
			if (point.name == "tresor.events" && point.monotonic && point.value == 1) {
				for (auto &label : point.labels) {
					counted = counted || (label.first == "kind" && label.second == "grant");
				}
			}
		}
		Check(counted, "tresor's own counters in the scrape, by tresor's names and labels");
		auto status = state->StatusJson(instance);
		Check(
		    status.find(
		        "\"tresor\":{\"enabled\":true,\"attached\":true,\"error\":null,\"events\":6,\"records\":{\"sent\":6") !=
		        string::npos,
		    "the status says attached and what arrived: " + status.substr(status.find("\"tresor\"")));
		state->ReconfigureTresor(instance, false);
		Check(registry->Sinks().empty() && !state->TresorSink(), "turned off: off tresor's registry");
		state->Stop();
	}

	{
		// a registry of another contract version: refused, said, nothing of ours on it
		DuckDB db(nullptr);
		auto &instance = *db.instance;
		auto foreign =
		    instance.GetObjectCache().GetOrCreate<tresor::TresorAuditHooks>(tresor::TresorAuditHooks::ObjectType());
		foreign->contract_version = 99;
		auto state = acl_otel::OtelState::Of(instance);
		Check(state->Start(instance), "the base's side still attaches");
		Check(!state->TresorSink() && state->TresorError().find("contract version 99") != string::npos,
		      "tresor's is refused, with why: " + state->TresorError());
		Check(foreign->Sinks().empty(), "and nothing of ours went on it");
		Check(state->StatusJson(instance).find("\"attached\":false,\"error\":\"") != string::npos,
		      "the status says so");
		state->Stop();
	}
	std::printf("PASS\n");
	return 0;
}
