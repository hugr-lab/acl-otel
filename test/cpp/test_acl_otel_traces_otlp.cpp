// The span transport (spec 008) against fake OTLP receivers in-process, the way specs 002 and 003
// test theirs: HTTP/protobuf on the bundled httplib (`POST /v1/traces`) and gRPC on the SDK's
// generated TraceService. What it asserts: which events are span candidates and which never are, the
// identity (the caller's trace id, the caller's span as our parent, a fresh span id; a root of our own
// only under `all`), the caller's sampling flag obeyed, `[ts_us - rewrite_us, ts_us]`, the name, the
// kind, the status rule (a refusal by policy is kUnset), the attributes the record carries and no
// other, and - with ACL_EXT - the two-loadable round trip: a real statement through the base under
// `TRACE ... PARENT ...`, and the span the receiver sees hangs under that parent. Built by CMake (the
// SDK links); run via `make test-cpp`, and again by CI's beside-acl step with ACL_EXT set.

#include "acl_otel_otlp.hpp"
#include "acl_otel_test_util.hpp"

#include "duckdb.hpp"
#include "httplib.hpp"
#include "opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"

#include <grpcpp/grpcpp.h>

#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

using namespace duckdb;
using namespace acl_otel_test;
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

//! spec 009: a span event as received - its name and attributes
struct ReceivedEvent {
	string name;
	std::map<string, string> attributes;
};

//! one received span, flattened the way a backend would index it
struct ReceivedSpan {
	string trace_id, span_id, parent_span_id; // hex
	string name;
	int kind = 0;
	uint64_t start_ns = 0, end_ns = 0;
	int status_code = 0;
	string status_message;
	uint32_t flags = 0;
	std::map<string, string> attributes; // ints rendered as text
	vector<ReceivedEvent> events;        // spec 009
	vector<string> links;                // spec 009: the linked span ids, hex
	vector<string> link_traces;          // ...and their trace ids
};

struct Capture {
	std::mutex lock;
	std::map<string, string> resource;
	string scope_name;
	vector<ReceivedSpan> spans;
	int requests = 0;

	void Take(const proto_trace::ExportTraceServiceRequest &request) {
		std::lock_guard<std::mutex> guard(lock);
		requests++;
		for (auto &rs : request.resource_spans()) {
			for (auto &kv : rs.resource().attributes()) {
				resource[kv.key()] = kv.value().string_value();
			}
			for (auto &ss : rs.scope_spans()) {
				scope_name = ss.scope().name();
				for (auto &span : ss.spans()) {
					ReceivedSpan r;
					r.trace_id = Hex(span.trace_id());
					r.span_id = Hex(span.span_id());
					r.parent_span_id = Hex(span.parent_span_id());
					r.name = span.name();
					r.kind = span.kind();
					r.start_ns = span.start_time_unix_nano();
					r.end_ns = span.end_time_unix_nano();
					r.status_code = span.status().code();
					r.status_message = span.status().message();
					r.flags = span.flags();
					for (auto &kv : span.attributes()) {
						r.attributes[kv.key()] = Attr(kv.value());
					}
					for (auto &event : span.events()) {
						ReceivedEvent e;
						e.name = event.name();
						for (auto &kv : event.attributes()) {
							e.attributes[kv.key()] = Attr(kv.value());
						}
						r.events.push_back(std::move(e));
					}
					for (auto &link : span.links()) {
						r.links.push_back(Hex(link.span_id()));
						r.link_traces.push_back(Hex(link.trace_id()));
					}
					spans.push_back(std::move(r));
				}
			}
		}
	}
	ReceivedSpan At(size_t i) {
		std::lock_guard<std::mutex> guard(lock);
		return spans.at(i);
	}
	size_t Count() {
		std::lock_guard<std::mutex> guard(lock);
		return spans.size();
	}
};

struct HttpReceiver {
	Capture capture;
	duckdb_httplib::Server server;
	int port = 0;
	std::thread thread;
	HttpReceiver() {
		server.Post("/v1/traces", [this](const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
			proto_trace::ExportTraceServiceRequest request;
			if (!request.ParseFromString(req.body)) {
				res.status = 400;
				return;
			}
			capture.Take(request);
			proto_trace::ExportTraceServiceResponse response;
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

struct GrpcReceiver : proto_trace::TraceService::Service {
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
	grpc::Status Export(grpc::ServerContext *, const proto_trace::ExportTraceServiceRequest *request,
	                    proto_trace::ExportTraceServiceResponse *) override {
		capture.Take(*request);
		return grpc::Status::OK;
	}
};

const char *TRACEPARENT = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";
const char *TRACEPARENT_UNSAMPLED = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-00";

acl::AuditEvent Base(int64_t seq, const char *kind, bool allowed) {
	acl::AuditEvent e;
	e.ts_us = 1757000000000000 + seq * 1000; // 2025-09-04T..., microseconds
	e.seq = seq;
	e.node = "node-a:42";
	e.kind = kind;
	e.allowed = allowed;
	e.level = allowed ? acl::AuditLevel::DECISIONS : acl::AuditLevel::DENIED;
	return e;
}

//! an allowed statement the caller traced: the span everybody wants
acl::AuditEvent Traced() {
	auto e = Base(1, "statement", true);
	e.statement = "select";
	e.door = "flight";
	e.session = "s-1";
	e.principal.subject = "u7";
	e.principal.roles = {"analyst"};
	e.principal.claims["tenant"] = "acme";
	e.principal.claims["secret"] = "not for export";
	e.objects.push_back(acl::AuditObject {"c.orders", "select"});
	e.correlation_id = "corr-1";
	e.traceparent = TRACEPARENT;
	e.rewrite_us = 250;
	return e;
}

acl::AuditEvent Unsampled() {
	auto e = Traced();
	e.seq = 2;
	e.traceparent = TRACEPARENT_UNSAMPLED;
	return e;
}

acl::AuditEvent Orphan() { // no trace context at all
	auto e = Traced();
	e.seq = 3;
	e.traceparent.clear();
	return e;
}

acl::AuditEvent Refused() { // by policy: the system working
	auto e = Base(4, "statement", false);
	e.statement = "update";
	e.reason_code = "capability";
	e.reason = "acl_rewrite: update on \"c.orders\" is not allowed";
	e.traceparent = TRACEPARENT;
	e.rewrite_us = 80;
	return e;
}

acl::AuditEvent Broken() { // our side failed
	auto e = Base(5, "statement", false);
	e.statement = "select";
	e.reason_code = "source_error";
	e.reason = "the policy catalog could not be read";
	e.traceparent = TRACEPARENT;
	e.rewrite_us = 120;
	return e;
}

acl::AuditEvent Admin() { // a management statement, untraced
	auto e = Base(6, "admin", true);
	e.statement = "manage";
	e.rewrite_us = 40;
	return e;
}

acl::AuditEvent SessionClose() {
	auto e = Base(7, "session", true);
	e.detail = "idle";
	e.door = "flight";
	e.duration_us = 90000000;
	return e;
}

//! spec 009: the execution of Traced() - the profile the base emits when the statement ran
acl::AuditEvent Profile() {
	auto e = Base(9, "profile", true);
	e.statement = "select";
	e.door = "flight";
	e.session = "s-1";
	e.principal.subject = "u7";
	e.principal.roles = {"analyst"};
	e.objects.push_back(acl::AuditObject {"c.orders", "select"});
	e.correlation_id = "corr-1";
	e.traceparent = TRACEPARENT;
	e.decision_seq = 1;
	e.exec_us = 4000;
	e.cpu_us = 6000;
	e.rows_scanned = 1000;
	e.rows_out = 5;
	e.bytes_read = 0;
	e.peak_memory = 4096;
	e.memory_allocated = 8192;
	e.blocked_us = 0;
	acl::AuditSource pg;
	pg.source = "pg";
	pg.kind = "postgres_scan";
	pg.scans = 1;
	pg.rows = 5;
	pg.rows_scanned = 1000;
	pg.timing_us = 3500;
	pg.bytes = 200;
	pg.filters = 2;
	pg.projections = 3;
	pg.dynamic_filters = true;
	e.sources.push_back(pg);
	acl::AuditSource local;
	local.source = "memory";
	local.kind = "table_scan";
	local.scans = 1;
	local.rows = 7;
	local.rows_scanned = 7;
	local.timing_us = 40;
	local.bytes = 28;
	e.sources.push_back(local);
	auto node = [&](int64_t id, int64_t parent, int64_t depth, const char *type, const char *kind, const char *source,
	                int64_t rows, int64_t timing_us) {
		acl::AuditPlanNode n;
		n.id = id;
		n.parent = parent;
		n.depth = depth;
		n.type = type;
		n.kind = kind;
		n.source = source;
		n.rows = rows;
		n.timing_us = timing_us;
		e.plan.push_back(n);
	};
	node(0, -1, 0, "RESULT_COLLECTOR", "", "", 0, 2);
	node(1, 0, 1, "HASH_JOIN", "", "", 5, 100);
	node(2, 1, 2, "TABLE_SCAN", "postgres_scan", "pg", 5, 3500);
	return e;
}

acl::AuditEvent FailedProfile() { // the execution failed: the class, never the text
	auto e = Profile();
	e.seq = 10;
	e.error = true;
	e.allowed = false;
	e.detail = "Conversion";
	e.exec_us = 700;
	e.sources.clear();
	e.plan.clear();
	return e;
}

acl::AuditEvent OrphanProfile() { // no caller's trace: under its decision, in the decision's trace
	auto e = Profile();
	e.seq = 11;
	e.traceparent.clear();
	return e;
}

string HexOf(const uint8_t *bytes, size_t size) {
	return Hex(string(reinterpret_cast<const char *>(bytes), size));
}

acl::AuditEvent Ingest() { // one end only
	auto e = Base(8, "ingest", true);
	e.door = "flight";
	e.rows = 10000;
	e.traceparent = TRACEPARENT;
	return e;
}

acl_otel::OtlpConfig Config(const std::string &endpoint, const std::string &protocol) {
	acl_otel::OtlpConfig config;
	config.endpoint = endpoint;
	config.protocol = protocol;
	config.timeout_s = 5;
	config.insecure = true;
	config.service_name = "acl-test";
	config.instance_id = "node-a:42";
	config.resource_attributes = "deployment=test";
	config.duckdb_version = "2.0.0-test";
	config.acl_otel_version = "0.0.1-test";
	return config;
}

vector<acl::AuditEvent> SampleBatch() {
	return {Traced(), Refused(), Broken(), Admin(), SessionClose()};
}

void CheckMapping(Capture &capture, const std::string &transport) {
	Check(capture.Count() == 5, transport + ": five spans received");
	Check(capture.resource["service.name"] == "acl-test" && capture.resource["service.instance.id"] == "node-a:42" &&
	          capture.resource["deployment"] == "test",
	      transport + ": the resource is the one the records carry (R1.3)");
	Check(capture.scope_name == "acl_otel", transport + ": the scope is acl_otel");
	auto traced = capture.At(0);
	Check(traced.trace_id == "0af7651916cd43dd8448eb211c80319c" && traced.parent_span_id == "b7ad6b7169203331",
	      transport + ": the caller's trace id, and the caller's span as our parent");
	Check(traced.span_id.size() == 16 && traced.span_id != "0000000000000000" &&
	          traced.span_id != traced.parent_span_id,
	      transport + ": a span id of our own");
	Check(traced.name == "acl SELECT" && traced.kind == 1, transport + ": named by the statement class, kInternal");
	Check(traced.start_ns == (1757000000001000ULL - 250) * 1000 && traced.end_ns == 1757000000001000ULL * 1000,
	      transport + ": [ts_us - rewrite_us, ts_us] (" + std::to_string(traced.start_ns) + ".." +
	          std::to_string(traced.end_ns) + ")");
	Check(traced.status_code == 0, transport + ": an allowed decision is kUnset");
	Check((traced.flags & 1) == 1, transport + ": sampled, as the caller said");
	Check(traced.attributes["acl.verdict"] == "allowed" && traced.attributes["acl.door"] == "flight" &&
	          traced.attributes["acl.session"] == "s-1" && traced.attributes["acl.subject"] == "u7" &&
	          traced.attributes["acl.roles"] == "[\"analyst\"]" &&
	          traced.attributes["acl.objects"] == "[{\"name\":\"c.orders\",\"capability\":\"select\"}]" &&
	          traced.attributes["acl.correlation_id"] == "corr-1" && traced.attributes["acl.rewrite_us"] == "250" &&
	          traced.attributes["acl.node"] == "node-a:42" && traced.attributes["acl.statement"] == "select",
	      transport + ": the attributes the record carries, on the span");
	Check(traced.attributes.count("acl.claim.tenant") == 0 && traced.attributes.count("acl.claim.secret") == 0,
	      transport + ": no claim value unless named (R5.1)");
	auto refused = capture.At(1);
	Check(refused.status_code == 0 && refused.attributes["acl.reason_code"] == "capability" &&
	          refused.attributes["acl.verdict"] == "denied",
	      transport + ": a refusal by policy is kUnset - the system working");
	Check(refused.name == "acl UPDATE" && refused.end_ns - refused.start_ns == 80000,
	      transport + ": ...with its own class and cost");
	auto broken = capture.At(2);
	Check(broken.status_code == 2 && broken.status_message == "the policy catalog could not be read",
	      transport + ": a source_error refusal is kError, with the reason");
	auto admin = capture.At(3);
	Check(admin.name == "acl MANAGE" && admin.parent_span_id.empty() && admin.trace_id.size() == 32 &&
	          admin.trace_id != "0af7651916cd43dd8448eb211c80319c" && admin.trace_id != string(32, '0') &&
	          (admin.flags & 1) == 1,
	      transport + ": an untraced decision roots a sampled trace of its own, with no invented parent");
	auto session = capture.At(4);
	Check(session.name == "acl session" && session.end_ns - session.start_ns == 90000000000ULL &&
	          session.attributes["acl.detail"] == "idle" && session.attributes["acl.duration_us"] == "90000000",
	      transport + ": a session close spans its life, by how it ended");
}

string Scalar(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		return "ERROR: " + result->GetError();
	}
	return result->RowCount() ? result->GetValue(0, 0).ToString() : string("<no rows>");
}

void Exec(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	Check(!result->HasError(), sql + (result->HasError() ? ": " + result->GetError() : ""));
}

} // namespace

int main(int argc, char *argv[]) {
	std::printf("test_acl_otel_traces_otlp\n");
	{
		// the gate: which events a span can honestly be built from, under which mode
		using acl_otel::SpanCandidate;
		using acl_otel::TraceMode;
		bool unsampled = false;
		Check(!SpanCandidate(Traced(), TraceMode::OFF, true, unsampled), "off: nothing, whatever the event");
		Check(SpanCandidate(Traced(), TraceMode::LINKED, false, unsampled) && !unsampled,
		      "linked: a traced decision is a span");
		Check(!SpanCandidate(Unsampled(), TraceMode::LINKED, false, unsampled) && unsampled,
		      "linked: the caller's 00 flags say no, and it is counted as such");
		Check(!SpanCandidate(Unsampled(), TraceMode::ALL, false, unsampled) && unsampled,
		      "all: the caller's decision wins there too - never upgraded");
		Check(!SpanCandidate(Orphan(), TraceMode::LINKED, false, unsampled) && !unsampled,
		      "linked: no parent, no span, not counted as unsampled either");
		Check(SpanCandidate(Orphan(), TraceMode::ALL, false, unsampled), "all: an orphan roots its own trace");
		Check(SpanCandidate(Refused(), TraceMode::LINKED, false, unsampled), "a refusal is a span like any decision");
		Check(SpanCandidate(Admin(), TraceMode::ALL, false, unsampled), "an admin decision too");
		Check(!SpanCandidate(SessionClose(), TraceMode::ALL, false, unsampled), "a session close waits for its switch");
		Check(SpanCandidate(SessionClose(), TraceMode::ALL, true, unsampled), "...and is a span with it");
		Check(!SpanCandidate(SessionClose(), TraceMode::LINKED, true, unsampled),
		      "...but not under linked, which needs a parent the session record does not carry");
		Check(!SpanCandidate(Ingest(), TraceMode::ALL, true, unsampled),
		      "an ingest has one end only and is never a span, traceparent or not");
		auto opened = SessionClose();
		opened.duration_us = -1;
		opened.detail = "opened";
		Check(!SpanCandidate(opened, TraceMode::ALL, true, unsampled), "a session open is one end only");
		// spec 009: an execution has both ends (exec_us), under the same rule as a decision
		Check(acl_otel::SpanDurationUs(Profile()) == 4000, "an execution's length is exec_us");
		Check(SpanCandidate(Profile(), TraceMode::LINKED, false, unsampled) && !unsampled,
		      "linked: a traced execution is a span");
		Check(!SpanCandidate(OrphanProfile(), TraceMode::LINKED, false, unsampled) && !unsampled,
		      "linked: an untraced one is not");
		Check(SpanCandidate(OrphanProfile(), TraceMode::ALL, false, unsampled), "all: it is");
		auto unlinked_flags = Profile();
		unlinked_flags.traceparent = TRACEPARENT_UNSAMPLED;
		Check(!SpanCandidate(unlinked_flags, TraceMode::ALL, false, unsampled) && unsampled,
		      "the caller's 00 flag is obeyed for an execution too");
		auto unmeasured = Profile();
		unmeasured.exec_us = -1;
		Check(!SpanCandidate(unmeasured, TraceMode::ALL, false, unsampled),
		      "an execution the base did not time is none");
		auto malformed = Traced();
		malformed.traceparent = "not-a-traceparent";
		Check(!SpanCandidate(malformed, TraceMode::LINKED, false, unsampled) &&
		          SpanCandidate(malformed, TraceMode::ALL, false, unsampled),
		      "a malformed traceparent is no parent: skipped under linked, a root under all");
		Check(acl_otel::SpanDurationUs(Traced()) == 250 && acl_otel::SpanDurationUs(SessionClose()) == 90000000 &&
		          acl_otel::SpanDurationUs(Ingest()) == -1,
		      "the duration is the decision's rewrite_us, the session's duration_us, nothing else");
	}
	{
		// the identity: the caller's ids when there are some, ours otherwise; our span id derived
		// from the node and the seq (spec 009: so an execution can link to its decision's)
		auto linked = acl_otel::IdentityOf(Traced());
		Check(linked.has_parent && linked.flags == 1 &&
		          Hex(string(reinterpret_cast<const char *>(linked.trace_id), 16)) ==
		              "0af7651916cd43dd8448eb211c80319c" &&
		          Hex(string(reinterpret_cast<const char *>(linked.parent_span_id), 8)) == "b7ad6b7169203331",
		      "a traced event: the caller's trace, the caller's span as the parent, the caller's flags");
		auto again = acl_otel::IdentityOf(Traced());
		uint8_t derived[8];
		acl_otel::SpanIdFor(Traced().node, Traced().seq, derived);
		Check(string(reinterpret_cast<const char *>(linked.span_id), 8) ==
		              string(reinterpret_cast<const char *>(again.span_id), 8) &&
		          string(reinterpret_cast<const char *>(linked.span_id), 8) ==
		              string(reinterpret_cast<const char *>(derived), 8) &&
		          string(reinterpret_cast<const char *>(linked.span_id), 8) != string(8, '\0'),
		      "our span id is SpanIdFor(node, seq): the same every time, never zero");
		auto other = Traced();
		other.seq = 99;
		Check(string(reinterpret_cast<const char *>(acl_otel::IdentityOf(other).span_id), 8) !=
		          string(reinterpret_cast<const char *>(linked.span_id), 8),
		      "...and another seq is another id");
		auto root = acl_otel::IdentityOf(Orphan());
		auto root_again = acl_otel::IdentityOf(Orphan());
		Check(!root.has_parent && root.flags == 1 &&
		          string(reinterpret_cast<const char *>(root.trace_id), 16) != string(16, '\0') &&
		          string(reinterpret_cast<const char *>(root.trace_id), 16) ==
		              string(reinterpret_cast<const char *>(root_again.trace_id), 16) &&
		          !linked.has_link && !root.has_link,
		      "an orphan: a sampled trace of its own, derived so its execution finds it, no parent invented");
		Check(acl_otel::SpanNameOf(Traced()) == "acl SELECT" && acl_otel::SpanNameOf(Admin()) == "acl MANAGE" &&
		          acl_otel::SpanNameOf(SessionClose()) == "acl session",
		      "the name is the class, upper-cased, never the SQL");
		Check(!acl_otel::SpanIsError(Traced()) && !acl_otel::SpanIsError(Refused()) && acl_otel::SpanIsError(Broken()),
		      "kError only when our side failed");
		// spec 009: the execution's identity - beside its decision under the caller's span, linked to
		// the decision's derived id; an orphan hangs under its decision in the decision's own trace
		auto execution = acl_otel::IdentityOf(Profile());
		uint8_t decision_id[8], decision_trace[16];
		acl_otel::SpanIdFor(Profile().node, 1, decision_id);
		acl_otel::TraceIdFor(Profile().node, 1, decision_trace);
		Check(execution.has_parent && HexOf(execution.parent_span_id, 8) == "b7ad6b7169203331" &&
		          HexOf(execution.trace_id, 16) == "0af7651916cd43dd8448eb211c80319c",
		      "a traced execution: the caller's span as the parent, a sibling of the decision");
		Check(execution.has_link && HexOf(execution.link_span_id, 8) == HexOf(decision_id, 8) &&
		          HexOf(execution.link_trace_id, 16) == HexOf(execution.trace_id, 16),
		      "...linked to SpanIdFor(node, decision_seq) in the caller's trace");
		Check(HexOf(execution.span_id, 8) == HexOf(acl_otel::IdentityOf(Traced()).span_id, 8) ? false : true,
		      "its own id is its own event's, not the decision's");
		auto orphan = acl_otel::IdentityOf(OrphanProfile());
		Check(orphan.has_parent && HexOf(orphan.parent_span_id, 8) == HexOf(decision_id, 8) &&
		          HexOf(orphan.trace_id, 16) == HexOf(decision_trace, 16) && orphan.has_link &&
		          HexOf(orphan.link_trace_id, 16) == HexOf(decision_trace, 16) && orphan.flags == 1,
		      "an untraced execution hangs under its decision, in the trace the decision rooted");
		auto unlinked = OrphanProfile();
		unlinked.decision_seq = -1;
		auto alone = acl_otel::IdentityOf(unlinked);
		Check(!alone.has_parent && !alone.has_link, "an unlinked orphan roots its own trace, links nothing");
		Check(acl_otel::SpanNameOf(Profile()) == "acl exec SELECT", "an execution is named acl exec <CLASS>");
		Check(acl_otel::SpanIsError(FailedProfile()) && !acl_otel::SpanIsError(Profile()),
		      "a failed execution is kError, a completed one kUnset");
	}
	{
		// spec 009: the execution span as a backend sees it - beside its decision, linked, with the
		// sources and the operators as events, the numbers as attributes, the class as the status
		HttpReceiver receiver;
		acl_otel::OtlpTraceExporter exporter(
		    Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf"));
		string error;
		Check(exporter.Export({Traced(), Profile(), FailedProfile()}, error), "profile: exported (" + error + ")");
		Check(receiver.capture.Count() == 3, "three spans: the decision, its execution, a failed one");
		auto decision = receiver.capture.At(0);
		auto execution = receiver.capture.At(1);
		Check(execution.name == "acl exec SELECT" && execution.kind == 1 && execution.trace_id == decision.trace_id &&
		          execution.parent_span_id == "b7ad6b7169203331",
		      "the execution: named by the class, under the caller's span beside the decision");
		Check(execution.links.size() == 1 && execution.links[0] == decision.span_id &&
		          execution.link_traces[0] == decision.trace_id,
		      "...linked to the decision span the receiver actually got (" +
		          (execution.links.empty() ? string("no link") : execution.links[0]) + " vs " + decision.span_id + ")");
		Check(execution.start_ns == (1757000000009000ULL - 4000) * 1000 &&
		          execution.end_ns == 1757000000009000ULL * 1000,
		      "[ts_us - exec_us, ts_us]");
		Check(execution.status_code == 0 && execution.attributes["acl.verdict"] == "ok" &&
		          execution.attributes["acl.decision_seq"] == "1" && execution.attributes["acl.exec.us"] == "4000" &&
		          execution.attributes["acl.exec.cpu_us"] == "6000" &&
		          execution.attributes["acl.exec.rows_out"] == "5" &&
		          execution.attributes["acl.exec.rows_scanned"] == "1000" &&
		          execution.attributes["acl.exec.peak_memory"] == "4096" &&
		          execution.attributes["acl.exec.truncated"] == "false" &&
		          execution.attributes["acl.exec.sources"].find("\"source\":\"pg\"") != string::npos &&
		          execution.attributes["acl.door"] == "flight" && execution.attributes["acl.statement"] == "select" &&
		          execution.attributes.count("acl.rewrite_us") == 0 && execution.attributes.count("acl.exec.plan") == 0,
		      "the execution's numbers as attributes, the rollup as a list, never the plan as one");
		Check(execution.events.size() == 5 && execution.events[0].name == "acl.source" &&
		          execution.events[0].attributes["acl.source"] == "pg" &&
		          execution.events[0].attributes["acl.kind"] == "postgres_scan" &&
		          execution.events[0].attributes["acl.filters"] == "2" &&
		          execution.events[0].attributes["acl.dynamic_filters"] == "true" &&
		          execution.events[0].attributes["acl.timing_us"] == "3500" &&
		          execution.events[1].name == "acl.source" && execution.events[1].attributes["acl.source"] == "memory",
		      "one acl.source event per source, with its numbers (" + std::to_string(execution.events.size()) + ")");
		Check(execution.events[2].name == "acl.operator" && execution.events[2].attributes["acl.operator.id"] == "0" &&
		          execution.events[2].attributes["acl.operator.type"] == "RESULT_COLLECTOR" &&
		          execution.events[4].attributes["acl.operator.parent"] == "1" &&
		          execution.events[4].attributes["acl.source"] == "pg" &&
		          execution.events[4].attributes["acl.timing_kind"] == "cumulative_thread_time",
		      "one acl.operator event per plan node, labelled for what its time is");
		auto failed = receiver.capture.At(2);
		Check(failed.status_code == 2 && failed.status_message == "Conversion" &&
		          failed.attributes["acl.verdict"] == "error" && failed.attributes["acl.detail"] == "Conversion" &&
		          failed.events.empty() && failed.end_ns - failed.start_ns == 700000,
		      "a failed execution is kError with the error's class, and nothing of the text");
	}
	{
		// spec 009: the plan is the volume knob - off, the sources still travel, the operators do not
		HttpReceiver receiver;
		auto config = Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf");
		config.profile_plan = false;
		acl_otel::OtlpTraceExporter exporter(config);
		string error;
		Check(exporter.Export({Profile()}, error), "plan off: exported (" + error + ")");
		auto execution = receiver.capture.At(0);
		Check(execution.events.size() == 2 && execution.events[1].name == "acl.source" &&
		          execution.attributes.count("acl.exec.sources") == 1,
		      "with the plan off the sources are still events and an attribute, the operators are gone");
	}
	{
		// spec 009, opt-in: the operators as child spans, nested exactly as the plan says
		HttpReceiver receiver;
		auto config = Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf");
		config.profile_spans = true;
		acl_otel::OtlpTraceExporter exporter(config);
		string error;
		Check(exporter.Export({Profile()}, error), "operator spans: exported (" + error + ")");
		Check(receiver.capture.Count() == 4,
		      "the execution span and one child per operator (" + std::to_string(receiver.capture.Count()) + ")");
		auto execution = receiver.capture.At(0);
		auto collector = receiver.capture.At(1);
		auto join = receiver.capture.At(2);
		auto scan = receiver.capture.At(3);
		Check(collector.name == "acl op RESULT_COLLECTOR" && collector.parent_span_id == execution.span_id &&
		          join.parent_span_id == collector.span_id && scan.parent_span_id == join.span_id &&
		          scan.trace_id == execution.trace_id && (scan.flags & 1) == 1,
		      "the root operator under the execution, each operator under its parent, one trace");
		Check(scan.end_ns - scan.start_ns == 3500000 && scan.end_ns == execution.end_ns &&
		          scan.attributes["acl.timing_kind"] == "cumulative_thread_time" &&
		          scan.attributes["acl.operator.type"] == "TABLE_SCAN" && scan.attributes["acl.source"] == "pg" &&
		          scan.attributes["acl.node"] == "node-a:42" && scan.status_code == 0,
		      "an operator span ends where the profile was collected, lasts its thread time, and says so");
		Check(collector.span_id != join.span_id && join.span_id != scan.span_id && scan.span_id != execution.span_id,
		      "four distinct ids from one event");
	}
	{
		HttpReceiver receiver;
		acl_otel::OtlpTraceExporter exporter(
		    Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf"));
		Check(exporter.Describe() == "otlp/http -> http://127.0.0.1:" + std::to_string(receiver.port) + "/v1/traces",
		      "http: the description names the endpoint with the traces path");
		string error;
		Check(exporter.Export(SampleBatch(), error), "http: the batch is exported (" + error + ")");
		CheckMapping(receiver.capture, "http");
	}
	{
		GrpcReceiver receiver;
		acl_otel::OtlpTraceExporter exporter(Config("127.0.0.1:" + std::to_string(receiver.port), "grpc"));
		Check(exporter.Describe() == "otlp/grpc -> 127.0.0.1:" + std::to_string(receiver.port),
		      "grpc: the description names the endpoint");
		string error;
		Check(exporter.Export(SampleBatch(), error), "grpc: the batch is exported (" + error + ")");
		CheckMapping(receiver.capture, "grpc");
	}
	{
		// spec 005 (R5.1) holds for a span exactly as for a record: the claims an operator named
		HttpReceiver receiver;
		auto config = Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf");
		config.claim_attributes = acl_otel::ParseClaimAttributes("tenant");
		acl_otel::OtlpTraceExporter exporter(config);
		string error;
		Check(exporter.Export({Traced()}, error), "claims: exported (" + error + ")");
		auto span = receiver.capture.At(0);
		Check(span.attributes["acl.claim.tenant"] == "acme" && span.attributes.count("acl.claim.secret") == 0,
		      "the named claim is on the span, the unnamed one is nowhere");
	}
	{
		// the sink's gate never hands an unmeasured event to the lane; if one arrived, the transport
		// refuses the batch loudly rather than export a zero-length span
		HttpReceiver receiver;
		acl_otel::OtlpTraceExporter exporter(
		    Config("http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf"));
		string error;
		Check(!exporter.Export({Traced(), Ingest()}, error) && error.find("no duration") != string::npos,
		      "an unmeasured event fails the batch with a reason (" + error + ")");
		Check(receiver.capture.Count() == 0, "...and nothing of it left");
	}
	{
		acl_otel::OtlpTraceExporter exporter(Config("http://127.0.0.1:1", "http/protobuf"));
		string error;
		Check(!exporter.Export({Traced()}, error) && error.find("the export failed") == 0 &&
		          error.find("/v1/traces") != string::npos,
		      "http: a port nobody listens on fails with a reason that names the transport (" + error + ")");
	}
	{
		// --- beside acl: the marker, the base's event and our span agree ---------------------------
		const char *acl_ext = std::getenv("ACL_EXT");
		if (!acl_ext || !*acl_ext) {
			std::printf(
			    "SKIP: ACL_EXT not set (the base's built extension) - the span round trip needs two loadables\n");
			std::printf("PASS\n");
			return 0;
		}
		string otel_ext = argc > 1 ? argv[1] : "build/release/extension/acl_otel/acl_otel.duckdb_extension";
		HttpReceiver receiver;
		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB db(nullptr, &config);
		Connection con(db);
		Exec(con, "LOAD '" + string(acl_ext) + "'");
		Exec(con, "LOAD '" + otel_ext + "'");
		Exec(con, "SET GLOBAL acl_otel_endpoint = 'http://127.0.0.1:" + std::to_string(receiver.port) + "'");
		Exec(con, "SET GLOBAL acl_otel_traces = 'linked'");
		Exec(con, "ATTACH ':memory:' AS store");
		Exec(con, "SELECT acl_use_db('store', 'acl', true)");
		Exec(con, "SET GLOBAL acl_allow_anonymous_admin = true");
		Exec(con, "SET GLOBAL acl_audit_level = 'all'");
		Exec(con, "ACL ADMIN CREATE VIRTUAL CATALOG c");
		Exec(con, "ACL ADMIN CREATE ROLE analyst");
		Exec(con, "ACL ADMIN GRANT CATALOG c TO ROLE analyst WITH (select) MAIN");
		Exec(con, "ATTACH ':memory:' AS phys");
		Exec(con, "CREATE TABLE phys.main.orders(id INTEGER)");
		Exec(con, "ACL ADMIN CREATE VIRTUAL TABLE c.orders AS phys.main.orders");
		Exec(con, string("ACL ROLE 'analyst' TRACE 'req-1' PARENT '") + TRACEPARENT + "' SELECT count(*) FROM orders");
		Exec(con, "ACL ROLE 'analyst' SELECT count(*) FROM orders"); // untraced: nothing under linked
		Exec(con, "SELECT acl_audit_flush()");
		Check(Scalar(con, "SELECT acl_otel_traces_flush()") == "true", "the span lane flushed");
		Check(receiver.capture.Count() == 1, "one span: the traced statement, not the untraced one (" +
		                                         std::to_string(receiver.capture.Count()) + ")");
		auto span = receiver.capture.At(0);
		Check(span.trace_id == "0af7651916cd43dd8448eb211c80319c" && span.parent_span_id == "b7ad6b7169203331",
		      "...and it hangs under the PARENT the statement was sent with");
		Check(span.name == "acl SELECT" && span.attributes["acl.verdict"] == "allowed" &&
		          span.attributes["acl.correlation_id"] == "req-1" && span.attributes.count("acl.rewrite_us") == 1 &&
		          span.end_ns > span.start_ns,
		      "named by the class the base gave it, TRACE as the correlation id, the base's own cost as its length");
		Check(span.end_ns - span.start_ns == std::stoull(span.attributes["acl.rewrite_us"]) * 1000,
		      "the length IS rewrite_us, not a guess");
		auto status = Scalar(con, "SELECT acl_otel_status()");
		Check(status.find("\"traces\":{\"mode\":\"linked\"") != string::npos &&
		          status.find("\"exported\":1,\"batches\":1,\"batches_failed\":0,\"dropped\":{\"queue\":0,\"no_"
		                      "exporter\":0,\"unsampled\":0}") != string::npos,
		      "the status counts it: " + status);
		// under `all` the untraced statement roots a trace of its own
		Exec(con, "SET GLOBAL acl_otel_traces = 'all'");
		Exec(con, "ACL ROLE 'analyst' SELECT count(*) FROM orders");
		Exec(con, "SELECT acl_audit_flush()");
		Check(Scalar(con, "SELECT acl_otel_traces_flush()") == "true", "flushed again");
		Check(receiver.capture.Count() == 2, "all: the untraced statement is a span now");
		auto root = receiver.capture.At(1);
		Check(root.parent_span_id.empty() && root.trace_id != span.trace_id && root.trace_id != string(32, '0'),
		      "...rooting a trace of its own, no parent invented");
		// spec 009: once the base profiles what it runs, a traced statement is two spans - the
		// decision and its execution - in the caller's trace, the execution linked to the decision
		Exec(con, "SET GLOBAL acl_otel_traces = 'linked'");
		Exec(con, "SET GLOBAL acl_profile_level = 'all'");
		Exec(con, string("ACL ROLE 'analyst' TRACE 'req-9' PARENT '") + TRACEPARENT + "' SELECT count(*) FROM orders");
		Exec(con, "SELECT acl_audit_flush()");
		Check(Scalar(con, "SELECT acl_otel_traces_flush()") == "true", "flushed with the profile");
		Check(receiver.capture.Count() == 4,
		      "two more spans: the decision and its execution (" + std::to_string(receiver.capture.Count()) + ")");
		auto decided = receiver.capture.At(2);
		auto executed = receiver.capture.At(3);
		Check(decided.name == "acl SELECT" && executed.name == "acl exec SELECT" &&
		          executed.trace_id == "0af7651916cd43dd8448eb211c80319c" &&
		          executed.parent_span_id == "b7ad6b7169203331" && decided.parent_span_id == executed.parent_span_id,
		      "the execution beside the decision, both under the PARENT the statement was sent with");
		Check(executed.links.size() == 1 && executed.links[0] == decided.span_id &&
		          executed.link_traces[0] == decided.trace_id,
		      "...and linked to the decision span the receiver got, by an id both sides derived");
		Check(executed.attributes["acl.correlation_id"] == "req-9" && executed.attributes["acl.verdict"] == "ok" &&
		          executed.attributes["acl.decision_seq"] == decided.attributes["acl.seq"] &&
		          executed.attributes["acl.exec.rows_out"] == "1" && executed.end_ns > executed.start_ns &&
		          executed.end_ns - executed.start_ns == std::stoull(executed.attributes["acl.exec.us"]) * 1000,
		      "the execution names its decision's seq, its rows, and lasts exactly exec_us");
		bool scanned_phys = false;
		for (auto &event : executed.events) {
			if (event.name == "acl.source" && event.attributes["acl.source"] == "phys") {
				scanned_phys = true;
			}
		}
		Check(scanned_phys, "the source event names the attached database the rewrite resolved the scan to (" +
		                        std::to_string(executed.events.size()) + " events)");
		Exec(con, "SET GLOBAL acl_profile_level = 'off'");
		Exec(con, "SET GLOBAL acl_otel_traces = 'off'");
		Check(Scalar(con, "SELECT acl_otel_traces_flush()") == "false", "off: nothing to flush");
	}
	std::printf("PASS\n");
	return 0;
}
