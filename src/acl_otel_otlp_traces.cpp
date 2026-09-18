// The OTLP span exporter (spec 008): a decision the base already finished, re-told as a span. The
// SDK's trace Recordable is writable in any order and needs no live Tracer, so a span is built after
// the fact from the event - `[ts_us - rewrite_us, ts_us]`, the caller's trace id, the caller's span
// as the parent - and handed straight to the SDK's OtlpHttpExporter / OtlpGrpcExporter over the
// same endpoint the records and the metrics use. No TracerProvider, no processor, no sampler of the
// SDK's: we are re-exporting somebody else's already-finished work, not instrumenting our own.

#include "acl_otel_otlp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include "opentelemetry/common/key_value_iterable_view.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_exporter_options.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_flags.h"
#include "opentelemetry/trace/trace_id.h"

#include <algorithm>
#include <chrono>

namespace duckdb {
namespace acl_otel {

namespace otlp = opentelemetry::exporter::otlp;
namespace sdktrace = opentelemetry::sdk::trace;

namespace {

using AttributeList = std::vector<std::pair<std::string, opentelemetry::common::AttributeValue>>;

//! spec 009: one span event's attributes; the string values must outlive the AddEvent (they are
//! the event's own fields, which do)
opentelemetry::common::KeyValueIterableView<AttributeList> View(const AttributeList &list) {
	return opentelemetry::common::KeyValueIterableView<AttributeList>(list);
}

void SourceAttributes(const acl::AuditSource &s, AttributeList &out) {
	out.emplace_back("acl.source", opentelemetry::nostd::string_view(s.source));
	out.emplace_back("acl.kind", opentelemetry::nostd::string_view(s.kind));
	out.emplace_back("acl.scans", s.scans);
	out.emplace_back("acl.rows", s.rows);
	out.emplace_back("acl.rows_scanned", s.rows_scanned);
	out.emplace_back("acl.timing_us", s.timing_us);
	out.emplace_back("acl.bytes", s.bytes);
	out.emplace_back("acl.filters", s.filters);
	out.emplace_back("acl.projections", s.projections);
	out.emplace_back("acl.dynamic_filters", s.dynamic_filters);
}

void OperatorAttributes(const acl::AuditPlanNode &n, AttributeList &out) {
	out.emplace_back("acl.operator.id", n.id);
	out.emplace_back("acl.operator.parent", n.parent);
	out.emplace_back("acl.operator.depth", n.depth);
	out.emplace_back("acl.operator.type", opentelemetry::nostd::string_view(n.type));
	out.emplace_back("acl.kind", opentelemetry::nostd::string_view(n.kind));
	out.emplace_back("acl.source", opentelemetry::nostd::string_view(n.source));
	out.emplace_back("acl.rows", n.rows);
	out.emplace_back("acl.rows_scanned", n.rows_scanned);
	out.emplace_back("acl.timing_us", n.timing_us);
	out.emplace_back("acl.bytes", n.bytes);
	out.emplace_back("acl.peak_memory_observed", n.peak_memory_observed);
	out.emplace_back("acl.filters", n.filters);
	out.emplace_back("acl.projections", n.projections);
	out.emplace_back("acl.dynamic_filters", n.dynamic_filters);
	// what the number is: thread time summed over the operator's work, not an interval
	out.emplace_back("acl.timing_kind", opentelemetry::nostd::string_view("cumulative_thread_time"));
}

opentelemetry::trace::SpanContext ContextOf(const uint8_t trace_id[16], const uint8_t span_id[8], uint8_t flags,
                                            bool remote) {
	using Bytes16 = opentelemetry::nostd::span<const uint8_t, 16>;
	using Bytes8 = opentelemetry::nostd::span<const uint8_t, 8>;
	return opentelemetry::trace::SpanContext(opentelemetry::trace::TraceId(Bytes16(trace_id, 16)),
	                                         opentelemetry::trace::SpanId(Bytes8(span_id, 8)),
	                                         opentelemetry::trace::TraceFlags(flags), remote);
}

} // namespace

SpanIdentity IdentityOf(const acl::AuditEvent &event) {
	SpanIdentity identity;
	uint8_t flags = 0;
	bool profile = event.kind == "profile";
	bool caller_traced = !event.traceparent.empty() &&
	                     ParseTraceparent(event.traceparent, identity.trace_id, identity.parent_span_id, flags);
	if (caller_traced) {
		// the caller's trace, the caller's span as our parent: a decision and its execution are
		// siblings under the request (the execution began after the decision ended)
		identity.has_parent = true;
		identity.flags = flags;
	} else if (profile && event.decision_seq >= 0) {
		// no caller's trace: the execution hangs under its decision, in the trace the decision
		// rooted for itself under `all` - both derived from what both sides know
		TraceIdFor(event.node, event.decision_seq, identity.trace_id);
		SpanIdFor(event.node, event.decision_seq, identity.parent_span_id);
		identity.has_parent = true;
		identity.flags = opentelemetry::trace::TraceFlags::kIsSampled;
	} else {
		// no parent is invented (a correlation id is not a trace id): a trace of our own, derived so
		// an execution can find it, sampled - it is the only reason a backend would keep it
		TraceIdFor(event.node, event.seq, identity.trace_id);
		identity.has_parent = false;
		identity.flags = opentelemetry::trace::TraceFlags::kIsSampled;
	}
	SpanIdFor(event.node, event.seq, identity.span_id);
	if (profile && event.decision_seq >= 0) {
		// the link to the decision span: in the caller's trace when the statement was traced (the
		// execution carries the decision's own trace context), else in the trace the decision rooted
		identity.has_link = true;
		if (caller_traced) {
			std::copy(identity.trace_id, identity.trace_id + 16, identity.link_trace_id);
		} else {
			TraceIdFor(event.node, event.decision_seq, identity.link_trace_id);
		}
		SpanIdFor(event.node, event.decision_seq, identity.link_span_id);
	}
	return identity;
}

string SpanNameOf(const acl::AuditEvent &event) {
	if (event.kind == "session") {
		return "acl session";
	}
	if (event.kind == "profile") {
		return "acl exec" + (event.statement.empty() ? string() : " " + StringUtil::Upper(event.statement));
	}
	if (!event.statement.empty()) {
		return "acl " + StringUtil::Upper(event.statement); // select / manage / native, as the base classes them
	}
	return "acl " + event.kind;
}

bool SpanIsError(const acl::AuditEvent &event) {
	if (event.kind == "profile") {
		return event.error; // a failed query is a failed span, whatever failed it
	}
	return !event.allowed && (event.reason_code == "source_error" || event.reason_code == "policy_error");
}

void OtlpTraceExporter::FillSpan(sdktrace::Recordable &out, const acl::AuditEvent &event, const SpanIdentity &identity,
                                 const vector<string> &claims, bool profile_plan) {
	using opentelemetry::common::SystemTimestamp;
	using Bytes8 = opentelemetry::nostd::span<const uint8_t, 8>;
	auto context = ContextOf(identity.trace_id, identity.span_id, identity.flags, identity.has_parent);
	opentelemetry::trace::SpanId parent;
	if (identity.has_parent) {
		parent = opentelemetry::trace::SpanId(Bytes8(identity.parent_span_id, 8));
	}
	out.SetIdentity(context, parent);
	out.SetTraceFlags(opentelemetry::trace::TraceFlags(identity.flags));
	auto name = SpanNameOf(event);
	out.SetName(name);
	// the door's RPC is the server span and it is not ours; this is work inside the node
	out.SetSpanKind(opentelemetry::trace::SpanKind::kInternal);
	// both ends are known: the base stamps ts_us when the decision is done and measured how long it
	// took. Nothing is derived, guessed or padded - the sink's gate never hands an unmeasured event
	// to this lane, so a negative duration here would be a bug, and the caller refuses the batch.
	auto duration = MaxValue<int64_t>(SpanDurationUs(event), 0);
	out.SetStartTime(SystemTimestamp(std::chrono::microseconds(event.ts_us - duration)));
	out.SetDuration(std::chrono::nanoseconds(duration * 1000));
	if (SpanIsError(event)) {
		// a failed execution names its error's class (spec 009), never its text
		out.SetStatus(opentelemetry::trace::StatusCode::kError, event.kind == "profile" ? event.detail : event.reason);
	} else {
		out.SetStatus(opentelemetry::trace::StatusCode::kUnset, "");
	}
	// the same bounded set the record carries (C5, R5.1), and nothing else
	FillAttributes(event, claims, [&](const char *key, const opentelemetry::common::AttributeValue &value) {
		out.SetAttribute(key, value);
	});
	if (event.kind != "profile") {
		return;
	}
	// spec 009: the link to the decision, and the sources - and the operators - as span events
	// stamped when the profile was collected, which is true; an event claims no interval
	if (identity.has_link) {
		AttributeList link {{"acl.link", opentelemetry::nostd::string_view("decision")}};
		out.AddLink(ContextOf(identity.link_trace_id, identity.link_span_id, identity.flags, false), View(link));
	}
	auto collected = SystemTimestamp(std::chrono::microseconds(event.ts_us));
	for (auto &source : event.sources) {
		AttributeList attributes;
		SourceAttributes(source, attributes);
		out.AddEvent("acl.source", collected, View(attributes));
	}
	if (profile_plan) {
		for (auto &node : event.plan) {
			AttributeList attributes;
			OperatorAttributes(node, attributes);
			out.AddEvent("acl.operator", collected, View(attributes));
		}
	}
}

void OtlpTraceExporter::FillOperatorSpan(sdktrace::Recordable &out, const acl::AuditEvent &event,
                                         const acl::AuditPlanNode &node, const SpanIdentity &parent) {
	using opentelemetry::common::SystemTimestamp;
	using Bytes8 = opentelemetry::nostd::span<const uint8_t, 8>;
	// the ids: the execution span's trace, this operator's own (salted by its id, so an event's
	// spans never collide), its parent the operator above it or the execution span at the root
	uint8_t span_id[8], parent_id[8];
	SpanIdFor(event.node, event.seq, span_id, node.id + 1);
	if (node.parent >= 0) {
		SpanIdFor(event.node, event.seq, parent_id, node.parent + 1);
	} else {
		std::copy(parent.span_id, parent.span_id + 8, parent_id);
	}
	out.SetIdentity(ContextOf(parent.trace_id, span_id, parent.flags, false),
	                opentelemetry::trace::SpanId(Bytes8(parent_id, 8)));
	out.SetTraceFlags(opentelemetry::trace::TraceFlags(parent.flags));
	auto name = "acl op " + node.type;
	out.SetName(name);
	out.SetSpanKind(opentelemetry::trace::SpanKind::kInternal);
	// cumulative thread time drawn as an interval ending where the profile was collected - and
	// labelled as what it is, because a scan on eight threads is eight seconds under a one-second
	// statement (the owner's condition for these spans at all)
	auto duration = MaxValue<int64_t>(node.timing_us, 0);
	out.SetStartTime(SystemTimestamp(std::chrono::microseconds(event.ts_us - duration)));
	out.SetDuration(std::chrono::nanoseconds(duration * 1000));
	out.SetStatus(opentelemetry::trace::StatusCode::kUnset, "");
	AttributeList attributes;
	OperatorAttributes(node, attributes);
	for (auto &attribute : attributes) {
		out.SetAttribute(attribute.first, attribute.second);
	}
	out.SetAttribute("acl.node", opentelemetry::nostd::string_view(event.node));
	out.SetAttribute("acl.seq", event.seq);
}

OtlpTraceExporter::OtlpTraceExporter(const OtlpConfig &config_p) : config(config_p) {
	if (!OtlpConfig::ValidProtocol(config.protocol)) {
		throw InvalidInputException("acl_otel_protocol accepts 'http/protobuf' or 'grpc', not '%s'", config.protocol);
	}
	auto protocol = config.ResolvedProtocol();
	resource = make_uniq<opentelemetry::sdk::resource::Resource>(ResourceOf(config));
	auto created = opentelemetry::sdk::instrumentationscope::InstrumentationScope::Create(
	    "acl_otel", config.acl_otel_version.empty() ? "dev" : config.acl_otel_version);
	scope = unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope>(created.release());
	auto timeout = std::chrono::seconds(config.timeout_s);
	if (protocol == "grpc") {
		otlp::OtlpGrpcExporterOptions options;
		if (!config.endpoint.empty()) {
			options.endpoint = config.endpoint;
		}
		if (config.timeout_s > 0) {
			options.timeout = timeout;
		}
		if (config.insecure) {
			options.use_ssl_credentials = false;
		} else if (!config.certificate.empty()) {
			options.use_ssl_credentials = true;
			options.ssl_credentials_cacert_path = config.certificate;
		}
		description = "otlp/grpc -> " + MaskUserinfo(options.endpoint);
		exporter = otlp::OtlpGrpcExporterFactory::Create(options);
	} else {
		otlp::OtlpHttpExporterOptions options;
		if (!config.endpoint.empty()) {
			options.url = SignalUrl(config.endpoint, "/v1/traces");
		}
		if (config.timeout_s > 0) {
			options.timeout = timeout;
		}
		if (!config.certificate.empty()) {
			options.ssl_ca_cert_path = config.certificate;
		}
		description = "otlp/http -> " + MaskUserinfo(options.url);
		exporter = otlp::OtlpHttpExporterFactory::Create(options);
	}
}

OtlpTraceExporter::~OtlpTraceExporter() {
	if (exporter) {
		exporter->Shutdown(std::chrono::seconds(2));
	}
}

bool OtlpTraceExporter::Export(const vector<acl::AuditEvent> &batch, string &error) {
	vector<std::unique_ptr<sdktrace::Recordable>> spans;
	spans.reserve(batch.size());
	for (auto &event : batch) {
		if (SpanDurationUs(event) < 0) {
			// unreachable through the sink (SpanCandidate gates the lane); said loudly rather than
			// exported as a zero-length span that would claim the decision was instant
			error = "the span lane was handed a " + event.kind + " event the base measured no duration for";
			return false;
		}
		auto identity = IdentityOf(event);
		auto span = exporter->MakeRecordable();
		span->SetResource(*resource);
		span->SetInstrumentationScope(*scope);
		FillSpan(*span, event, identity, config.claim_attributes, config.profile_plan);
		spans.push_back(std::move(span));
		if (event.kind == "profile" && config.profile_spans) {
			// spec 009, opt-in: the plan as child spans, nested as the plan says
			for (auto &node : event.plan) {
				auto child = exporter->MakeRecordable();
				child->SetResource(*resource);
				child->SetInstrumentationScope(*scope);
				FillOperatorSpan(*child, event, node, identity);
				spans.push_back(std::move(child));
			}
		}
	}
	TakeSdkError(); // what the SDK says about THIS export, not an earlier one (spec 002's handler)
	auto result =
	    exporter->Export(opentelemetry::nostd::span<std::unique_ptr<sdktrace::Recordable>>(spans.data(), spans.size()));
	// the HTTP exporter answers kSuccess whatever its client said (spec 002's finding, the same
	// client): an error the SDK logged during this export IS the failure
	auto why = TakeSdkError();
	switch (result) {
	case opentelemetry::sdk::common::ExportResult::kSuccess:
		if (why.empty()) {
			error.clear();
			return true;
		}
		break;
	case opentelemetry::sdk::common::ExportResult::kFailureFull:
		error = "the exporter's queue is full";
		return false;
	case opentelemetry::sdk::common::ExportResult::kFailureInvalidArgument:
		error = "the exporter refused the batch as invalid";
		return false;
	default:
		break;
	}
	error = "the export failed (" + description + ")" + (why.empty() ? "" : ": " + why);
	return false;
}

string OtlpTraceExporter::Describe() const {
	return description;
}

} // namespace acl_otel
} // namespace duckdb
