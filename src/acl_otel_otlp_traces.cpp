// The OTLP span exporter (spec 008): a decision the base already finished, re-told as a span. The
// SDK's trace Recordable is writable in any order and needs no live Tracer, so a span is built after
// the fact from the event - `[ts_us - rewrite_us, ts_us]`, the caller's trace id, the caller's span
// as the parent - and handed straight to the SDK's OtlpHttpExporter / OtlpGrpcExporter over the
// same endpoint the records and the metrics use. No TracerProvider, no processor, no sampler of the
// SDK's: we are re-exporting somebody else's already-finished work, not instrumenting our own.

#include "acl_otel_otlp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

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

#include <chrono>

namespace duckdb {
namespace acl_otel {

namespace otlp = opentelemetry::exporter::otlp;
namespace sdktrace = opentelemetry::sdk::trace;

namespace {

//! 8 or 16 fresh bytes, never all zero (an all-zero id is invalid by the specification)
void FreshId(std::mt19937_64 &random, uint8_t *out, idx_t size) {
	for (;;) {
		bool zero = true;
		for (idx_t i = 0; i < size; i += 8) {
			auto word = random();
			for (idx_t j = 0; j < 8 && i + j < size; j++) {
				out[i + j] = static_cast<uint8_t>(word >> (8 * j));
				zero = zero && out[i + j] == 0;
			}
		}
		if (!zero) {
			return;
		}
	}
}

} // namespace

SpanIdentity IdentityOf(const acl::AuditEvent &event, std::mt19937_64 &random) {
	SpanIdentity identity;
	uint8_t flags = 0;
	if (!event.traceparent.empty() &&
	    ParseTraceparent(event.traceparent, identity.trace_id, identity.parent_span_id, flags)) {
		identity.has_parent = true;
		identity.flags = flags;
	} else {
		// no parent is invented (a correlation id is not a trace id): a trace of our own, sampled -
		// it is the only reason a backend would keep it
		FreshId(random, identity.trace_id, 16);
		identity.has_parent = false;
		identity.flags = opentelemetry::trace::TraceFlags::kIsSampled;
	}
	FreshId(random, identity.span_id, 8);
	return identity;
}

string SpanNameOf(const acl::AuditEvent &event) {
	if (event.kind == "session") {
		return "acl session";
	}
	if (!event.statement.empty()) {
		return "acl " + StringUtil::Upper(event.statement); // select / manage / native, as the base classes them
	}
	return "acl " + event.kind;
}

bool SpanIsError(const acl::AuditEvent &event) {
	return !event.allowed && (event.reason_code == "source_error" || event.reason_code == "policy_error");
}

void OtlpTraceExporter::FillSpan(sdktrace::Recordable &out, const acl::AuditEvent &event, const SpanIdentity &identity,
                                 const vector<string> &claims) {
	using opentelemetry::common::SystemTimestamp;
	using Bytes16 = opentelemetry::nostd::span<const uint8_t, 16>;
	using Bytes8 = opentelemetry::nostd::span<const uint8_t, 8>;
	opentelemetry::trace::SpanContext context(opentelemetry::trace::TraceId(Bytes16(identity.trace_id, 16)),
	                                          opentelemetry::trace::SpanId(Bytes8(identity.span_id, 8)),
	                                          opentelemetry::trace::TraceFlags(identity.flags), identity.has_parent);
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
		out.SetStatus(opentelemetry::trace::StatusCode::kError, event.reason);
	} else {
		out.SetStatus(opentelemetry::trace::StatusCode::kUnset, "");
	}
	// the same bounded set the record carries (C5, R5.1), and nothing else
	FillAttributes(event, claims, [&](const char *key, const opentelemetry::common::AttributeValue &value) {
		out.SetAttribute(key, value);
	});
}

OtlpTraceExporter::OtlpTraceExporter(const OtlpConfig &config_p)
    : config(config_p), random(std::random_device {}() ^
                               static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())) {
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
		auto span = exporter->MakeRecordable();
		span->SetResource(*resource);
		span->SetInstrumentationScope(*scope);
		FillSpan(*span, event, IdentityOf(event, random), config.claim_attributes);
		spans.push_back(std::move(span));
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
