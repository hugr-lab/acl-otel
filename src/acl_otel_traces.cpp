// What a span is built from, without the SDK (spec 008): the trace mode, the W3C traceparent
// parser the record (R1.2) and the span share, and the judgement - on the audit thread, O(1) - of
// whether an event is one a span can honestly be built from. The transport that turns the event
// into an SDK span is acl_otel_otlp_traces.cpp; this file is what the sink and its tests link
// without it.

#include "acl_otel.hpp"

#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace acl_otel {

namespace {

bool HexByte(const char *at, uint8_t &out) {
	uint8_t value = 0;
	for (int i = 0; i < 2; i++) {
		char c = at[i];
		uint8_t nibble;
		if (c >= '0' && c <= '9') {
			nibble = NumericCast<uint8_t>(c - '0');
		} else if (c >= 'a' && c <= 'f') {
			nibble = NumericCast<uint8_t>(c - 'a' + 10);
		} else if (c >= 'A' && c <= 'F') {
			nibble = NumericCast<uint8_t>(c - 'A' + 10);
		} else {
			return false;
		}
		value = NumericCast<uint8_t>((value << 4) | nibble);
	}
	out = value;
	return true;
}

bool HexBytes(const string &text, idx_t from, idx_t count, uint8_t *out) {
	for (idx_t i = 0; i < count; i++) {
		if (!HexByte(text.c_str() + from + 2 * i, out[i])) {
			return false;
		}
	}
	return true;
}

} // namespace

bool ParseTraceparent(const string &traceparent, uint8_t trace_id[16], uint8_t span_id[8], uint8_t &flags) {
	// 00-<32 hex>-<16 hex>-<2 hex> = 55 characters, version 00 only (the W3C recommendation for a
	// consumer that knows no other)
	if (traceparent.size() != 55 || traceparent[2] != '-' || traceparent[35] != '-' || traceparent[52] != '-') {
		return false;
	}
	if (traceparent[0] != '0' || traceparent[1] != '0') {
		return false;
	}
	if (!HexBytes(traceparent, 3, 16, trace_id) || !HexBytes(traceparent, 36, 8, span_id) ||
	    !HexBytes(traceparent, 53, 1, &flags)) {
		return false;
	}
	bool trace_zero = true, span_zero = true;
	for (int i = 0; i < 16; i++) {
		trace_zero = trace_zero && trace_id[i] == 0;
	}
	for (int i = 0; i < 8; i++) {
		span_zero = span_zero && span_id[i] == 0;
	}
	return !trace_zero && !span_zero; // an all-zero id is invalid by the specification
}

bool ParseTraceMode(const string &text, TraceMode &out) {
	auto lowered = StringUtil::Lower(text);
	StringUtil::Trim(lowered);
	if (lowered == "off") {
		out = TraceMode::OFF;
	} else if (lowered == "linked") {
		out = TraceMode::LINKED;
	} else if (lowered == "all") {
		out = TraceMode::ALL;
	} else {
		return false;
	}
	return true;
}

const char *TraceModeName(TraceMode mode) {
	switch (mode) {
	case TraceMode::LINKED:
		return "linked";
	case TraceMode::ALL:
		return "all";
	default:
		return "off";
	}
}

int64_t SpanDurationUs(const acl::AuditEvent &event) {
	if (event.kind == "statement" || event.kind == "admin") {
		return event.rewrite_us; // the decision's own cost, measured by the base; -1 when it was not
	}
	if (event.kind == "session") {
		return event.duration_us; // set on the close event only; the open carries -1
	}
	return -1; // an ingest, a door, a policy or a keys event has one end, and stays a log record
}

bool SpanCandidate(const acl::AuditEvent &event, TraceMode mode, bool session_spans, bool &unsampled) {
	unsampled = false;
	if (mode == TraceMode::OFF) {
		return false;
	}
	if (event.kind == "session" && !session_spans) {
		return false;
	}
	if (SpanDurationUs(event) < 0) {
		return false; // one end only: never a zero-length span pretending to be instant
	}
	uint8_t trace_id[16], span_id[8], flags = 0;
	if (!event.traceparent.empty() && ParseTraceparent(event.traceparent, trace_id, span_id, flags)) {
		// the caller's sampling decision wins: a trace its tracer is not recording gets no span of
		// ours, which would be an orphan in the backend and a surprise in the bill
		if ((flags & 0x01) == 0) {
			unsampled = true;
			return false;
		}
		return true;
	}
	// no usable parent: `linked` skips, `all` roots a trace of its own. A malformed traceparent is
	// no parent at all, as it is no trace context on the record (R1.2).
	return mode == TraceMode::ALL;
}

} // namespace acl_otel
} // namespace duckdb
