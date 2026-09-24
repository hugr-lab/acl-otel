// Spec 011: tresor's audit as OpenTelemetry. tresor (the secrets service's duckdb client) publishes its
// own events through duckdb-ext-common's `tresor_audit.hpp` (TRSA 1) - a login, a secret looked up, a
// grant - and this extension, already on the node for acl's audit, carries them out the same way: a
// log record per event, a span when the event names the caller's trace and measured its call, and
// tresor's counters in the metrics scrape. The lanes are spec 001's, of tresor's event type; the
// transports are spec 002's and 008's, under the instrumentation scope `tresor`.

#include "acl_otel_otlp.hpp"

#include "duckdb/common/exception.hpp"

#include "opentelemetry/logs/severity.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_flags.h"
#include "opentelemetry/trace/trace_id.h"

#include <chrono>

namespace duckdb {
namespace acl_otel {

namespace sdklogs = opentelemetry::sdk::logs;
namespace sdktrace = opentelemetry::sdk::trace;

namespace {

//! tresor numbers its events apart from acl's, so its span ids take a salt of their own: a decision
//! span and a tresor span of the same node and seq never collide
constexpr int64_t TRESOR_SPAN_SALT = 0x74726573; // "tres"

bool Failed(const tresor::TresorAuditEvent &event) {
	return event.outcome == "denied" || event.outcome == "error";
}

} // namespace

OtelSeverity SeverityOf(const tresor::TresorAuditEvent &event) {
	return Failed(event) ? OtelSeverity::WARN : OtelSeverity::INFO;
}

string BodyOf(const tresor::TresorAuditEvent &event) {
	auto body = "tresor " + event.kind + " " + event.outcome;
	if (!event.reason.empty()) {
		body += ": " + event.reason;
	}
	return body;
}

void FillTresorAttributes(const tresor::TresorAuditEvent &event,
                          const std::function<void(const char *, const opentelemetry::common::AttributeValue &)> &set) {
	auto text = [&](const char *key, const string &value) {
		if (!value.empty()) {
			set(key, opentelemetry::nostd::string_view(value));
		}
	};
	text("tresor.kind", event.kind);
	text("tresor.outcome", event.outcome);
	text("tresor.reason_code", event.reason_code);
	text("tresor.reason", event.reason);
	text("tresor.service", event.service);
	text("tresor.host", event.host);
	text("tresor.login", event.login);
	text("tresor.principal", event.principal);
	text("tresor.user", event.user);
	text("tresor.acl_session", event.acl_session);
	text("tresor.correlation_id", event.correlation_id);
	text("tresor.secret", event.secret);
	text("tresor.secret_type", event.secret_type);
	text("tresor.target", event.target);
	text("tresor.detail", event.detail);
	set("tresor.seq", event.seq);
	if (event.kind == "lookup" || event.kind == "refresh") {
		set("tresor.cached", event.cached);
	}
	if (!event.secret.empty()) {
		set("tresor.dynamic", event.dynamic);
	}
	if (event.duration_us >= 0) {
		set("tresor.duration_us", event.duration_us);
	}
}

void FillTresorRecord(sdklogs::Recordable &record, const tresor::TresorAuditEvent &event) {
	using opentelemetry::common::SystemTimestamp;
	record.SetTimestamp(SystemTimestamp(std::chrono::microseconds(event.ts_us)));
	record.SetObservedTimestamp(SystemTimestamp(std::chrono::system_clock::now()));
	record.SetSeverity(SeverityOf(event) == OtelSeverity::WARN ? opentelemetry::logs::Severity::kWarn
	                                                           : opentelemetry::logs::Severity::kInfo);
	auto body = BodyOf(event);
	record.SetBody(opentelemetry::common::AttributeValue(opentelemetry::nostd::string_view(body)));
	// the caller's trace context, or nothing - a malformed value sets neither (R1.2, as acl's records)
	uint8_t trace_id[16], span_id[8], flags = 0;
	if (!event.traceparent.empty() && ParseTraceparent(event.traceparent, trace_id, span_id, flags)) {
		record.SetTraceId(opentelemetry::trace::TraceId(opentelemetry::nostd::span<const uint8_t, 16>(trace_id, 16)));
		record.SetSpanId(opentelemetry::trace::SpanId(opentelemetry::nostd::span<const uint8_t, 8>(span_id, 8)));
		record.SetTraceFlags(opentelemetry::trace::TraceFlags(flags));
	}
	FillTresorAttributes(event, [&](const char *key, const opentelemetry::common::AttributeValue &value) {
		record.SetAttribute(key, value);
	});
}

bool TresorSpanCandidate(const tresor::TresorAuditEvent &event, bool &unsampled) {
	unsampled = false;
	if (event.duration_us < 0 || event.traceparent.empty()) {
		return false; // no call was made, or nobody traces the statement: a record, not a span
	}
	uint8_t trace_id[16], span_id[8], flags = 0;
	if (!ParseTraceparent(event.traceparent, trace_id, span_id, flags)) {
		return false;
	}
	if ((flags & 0x01) == 0) {
		unsampled = true; // the caller's decision, obeyed and counted
		return false;
	}
	return true;
}

void TresorSpanId(const string &node, const tresor::TresorAuditEvent &event, uint8_t out[8]) {
	SpanIdFor(node, event.seq, out, TRESOR_SPAN_SALT);
}

void FillTresorSpan(sdktrace::Recordable &out, const tresor::TresorAuditEvent &event, const string &node) {
	using opentelemetry::common::SystemTimestamp;
	using Bytes16 = opentelemetry::nostd::span<const uint8_t, 16>;
	using Bytes8 = opentelemetry::nostd::span<const uint8_t, 8>;
	uint8_t trace_id[16], parent_id[8], flags = 0;
	ParseTraceparent(event.traceparent, trace_id, parent_id, flags); // the lane's gate parsed it already
	uint8_t span_id[8];
	TresorSpanId(node, event, span_id);
	opentelemetry::trace::SpanContext context(opentelemetry::trace::TraceId(Bytes16(trace_id, 16)),
	                                          opentelemetry::trace::SpanId(Bytes8(span_id, 8)),
	                                          opentelemetry::trace::TraceFlags(flags), false);
	out.SetIdentity(context, opentelemetry::trace::SpanId(Bytes8(parent_id, 8)));
	out.SetTraceFlags(opentelemetry::trace::TraceFlags(flags));
	out.SetName("tresor." + event.kind);
	out.SetSpanKind(opentelemetry::trace::SpanKind::kInternal);
	// both ends are tresor's: the event ends at ts_us, and duration_us is what its service calls took
	auto duration = MaxValue<int64_t>(event.duration_us, 0);
	out.SetStartTime(SystemTimestamp(std::chrono::microseconds(event.ts_us - duration)));
	out.SetDuration(std::chrono::nanoseconds(duration * 1000));
	if (Failed(event)) {
		out.SetStatus(opentelemetry::trace::StatusCode::kError, event.reason_code);
	} else {
		out.SetStatus(opentelemetry::trace::StatusCode::kUnset, "");
	}
	FillTresorAttributes(event, [&](const char *key, const opentelemetry::common::AttributeValue &value) {
		out.SetAttribute(key, value);
	});
}

OtlpTresorExporter::OtlpTresorExporter(const OtlpConfig &config) : transport(config, "tresor") {
}

bool OtlpTresorExporter::Export(const vector<tresor::TresorAuditEvent> &batch, string &error) {
	return transport.ExportFilled(
	    batch.size(), [&](sdklogs::Recordable &record, idx_t i) { FillTresorRecord(record, batch[i]); }, error);
}

string OtlpTresorExporter::Describe() const {
	return transport.Describe();
}

OtlpTresorTraceExporter::OtlpTresorTraceExporter(const OtlpConfig &config)
    : transport(config, "tresor"), node(config.instance_id) {
}

bool OtlpTresorTraceExporter::Export(const vector<tresor::TresorAuditEvent> &batch, string &error) {
	for (auto &event : batch) {
		bool unsampled;
		if (!TresorSpanCandidate(event, unsampled)) {
			// unreachable through the sink, which gates the lane; said loudly rather than exported
			error = "the tresor span lane was handed a " + event.kind + " event that is not a span";
			return false;
		}
	}
	return transport.ExportFilled(
	    batch.size(), [&](sdktrace::Recordable &span, idx_t i) { FillTresorSpan(span, batch[i], node); }, error);
}

string OtlpTresorTraceExporter::Describe() const {
	return transport.Describe();
}

//===--------------------------------------------------------------------===//
// The sink tresor delivers to
//===--------------------------------------------------------------------===//

TresorOtelSink::TresorOtelSink(idx_t queue_size, idx_t batch_size, int64_t flush_interval_ms,
                               shared_ptr<TresorExporter> exporter)
    : records(queue_size, batch_size, flush_interval_ms, std::move(exporter)) {
}

TresorOtelSink::~TresorOtelSink() {
	Stop();
}

void TresorOtelSink::OnEvent(const tresor::TresorAuditEvent &event) {
	// on tresor's delivery thread: O(1), no I/O, no wait - a full queue drops and counts
	received++;
	records.Push(event);
	shared_ptr<TresorQueue> lane;
	{
		std::lock_guard<std::mutex> guard(lock);
		lane = spans;
	}
	if (!lane) {
		return;
	}
	bool unsampled = false;
	if (TresorSpanCandidate(event, unsampled)) {
		lane->Push(event);
	} else if (unsampled) {
		lane->stats.unsampled++;
	}
}

void TresorOtelSink::Flush() {
	// bounded (each lane's FlushNow waits at most two seconds); touches no object cache, as the
	// contract asks - tresor's last delivery may run inside the cache's teardown
	records.FlushNow();
	auto lane = Spans();
	if (lane) {
		lane->FlushNow();
	}
}

void TresorOtelSink::SetSpans(shared_ptr<TresorQueue> lane) {
	std::lock_guard<std::mutex> guard(lock);
	spans = std::move(lane);
}

shared_ptr<TresorQueue> TresorOtelSink::Spans() {
	std::lock_guard<std::mutex> guard(lock);
	return spans;
}

void TresorOtelSink::Stop() {
	records.Stop();
	auto lane = Spans();
	if (lane) {
		lane->Stop();
	}
}

} // namespace acl_otel
} // namespace duckdb
