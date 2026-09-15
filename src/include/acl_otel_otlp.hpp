//===----------------------------------------------------------------------===//
// acl_otel_otlp.hpp - the OTLP log exporter (spec 002) and the OTLP span exporter (spec 008)
//
// One SDK exporter per signal (HTTP/protobuf or gRPC) driven from a lane's worker, batch by batch:
// one Recordable per event, filled from the event, one Export call. No SDK processor, no tracer, no
// second queue, no second thread - the sink of spec 001 keeps its semantics. The mapping (R1.1-R1.3,
// and the span's identity) is a free function so the tests judge it without a transport.
//===----------------------------------------------------------------------===//

#pragma once

#include "acl_otel.hpp"

// the SDK's namespace is an inline `v1`: a forward declaration of our own would make a second
// `opentelemetry::sdk` and every `sdk::` in the SDK's headers ambiguous - so the headers themselves
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/logs/exporter.h"
#include "opentelemetry/sdk/logs/recordable.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/recordable.h"

#include <functional>
#include <random>

namespace duckdb {
namespace acl_otel {

//! What a transport is built from: the settings and the environment, resolved once
//! A setting at its default means "the standard environment decides" (`OTEL_EXPORTER_OTLP_*`, read
//! by the SDK's option defaults or here), so a container configured the OpenTelemetry way needs
//! no SET; a setting that is set wins over its variable.
struct OtlpConfig {
	string endpoint; // '' = OTEL_EXPORTER_OTLP_ENDPOINT / _LOGS_ENDPOINT
	string protocol; // http/protobuf | grpc | '' = OTEL_EXPORTER_OTLP_PROTOCOL / _LOGS_PROTOCOL, else http/protobuf
	int64_t timeout_s = 0; // <= 0 = OTEL_EXPORTER_OTLP_TIMEOUT, else the SDK's 10 s
	bool insecure = false; // gRPC without TLS; false = the SDK decides (the scheme, OTEL_EXPORTER_OTLP_INSECURE)
	string certificate;    // a CA file, '' = OTEL_EXPORTER_OTLP_CERTIFICATE, else the system's
	string service_name = "duckdb-acl";
	string instance_id;         // the node id
	string resource_attributes; // k=v,k=v
	//! spec 005 (R5.1): the claim names whose values may be exported, as `acl.claim.<name>`. Empty
	//! - the default - exports none: a claim value leaves the node only when an operator names it.
	vector<string> claim_attributes;
	string duckdb_version;
	string acl_otel_version;
	//! from the settings (through the instance) and the base's acl_node_id when acl is loaded
	static OtlpConfig From(DatabaseInstance &db);
	//! `acl_otel_protocol` accepts the two protocols, or '' for the environment's
	static bool ValidProtocol(const string &protocol);
	//! the protocol this config stands for: the setting, else the environment, else http/protobuf
	string ResolvedProtocol() const;
};

//! What the SDK logged as an error since the last call, and clears it: the HTTP exporters answer
//! success whatever their client said, so this is the failure signal both transports read.
string TakeSdkError();
//! the OTLP convention: a base URL gets the signal's path (`/v1/logs`, `/v1/metrics`), a URL that
//! already names it is kept as written
string SignalUrl(const string &endpoint, const string &path);
//! the resource every signal of this node carries (R1.3)
opentelemetry::sdk::resource::Resource ResourceOf(const OtlpConfig &config);

//! `scheme://user:secret@host/...` -> `scheme://***@host/...`: a credential written into the
//! endpoint is the operator's choice against R9.2, and the status still never prints it
string MaskUserinfo(const string &url);

//! spec 005: `acl_otel_claim_attributes` - at most 16 names, refused at the SET beyond that
vector<string> ParseClaimAttributes(const string &names);
//! a claim value as a record may carry it: at most 256 bytes, with an ellipsis when it was longer
string ClaimValue(const string &value);

//! severity per R1.1: INFO allowed, WARN denied, ERROR when the source of the decision failed
enum class OtelSeverity : uint8_t { INFO, WARN, ERROR };
OtelSeverity SeverityOf(const acl::AuditEvent &event);
//! the body: `<kind> <allowed|denied>` + `: <reason>`
string BodyOf(const acl::AuditEvent &event);
//! `acl.roles` and `acl.objects` as JSON strings - flat attributes, never nested (R1.6)
string RolesJson(const acl::AuditEvent &event);
string ObjectsJson(const acl::AuditEvent &event);
//! R1.1's attributes, shared by the record (spec 002) and the span (spec 008): every field an `acl.`
//! attribute, present only when the event carries it, and a claim value only when `claims` names
//! its claim (R5.1) - everything else in principal.claims is dropped here. `set` receives each.
void FillAttributes(const acl::AuditEvent &event, const vector<string> &claims,
                    const std::function<void(const char *, const opentelemetry::common::AttributeValue &)> &set);

class OtlpExporter : public Exporter {
public:
	//! throws InvalidInputException on a config the SDK refuses (a bad protocol, a bad endpoint)
	explicit OtlpExporter(const OtlpConfig &config);
	~OtlpExporter() override;
	bool Export(const vector<acl::AuditEvent> &batch, string &error) override;
	string Describe() const override;
	//! the header NAMES the transport carries (R9.2: never a value)
	vector<string> HeaderNames() const;
	//! fill one SDK record from one event - the mapping of R1.1-R1.2, shared with the tests.
	//! `claims` names the claims whose values may be exported (spec 005, R5.1); every other claim
	//! the event carries in memory is dropped here.
	static void Fill(opentelemetry::sdk::logs::Recordable &record, const acl::AuditEvent &event,
	                 const vector<string> &claims);

private:
	OtlpConfig config;
	string description;
	vector<string> header_names;
	std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> exporter; // the SDK factory hands out std::
	unique_ptr<opentelemetry::sdk::resource::Resource> resource;
	unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope> scope;
};

//! spec 008: who a span is. From the caller's traceparent when the event carries a usable one - its
//! trace id, its span id as our parent, its flags - else a trace of our own, rooted and sampled.
//! Our own span id is fresh either way: unique, not unguessable, so the ordinary generator.
struct SpanIdentity {
	uint8_t trace_id[16] = {};
	uint8_t span_id[8] = {};
	uint8_t parent_span_id[8] = {};
	bool has_parent = false;
	uint8_t flags = 0x01;
};
SpanIdentity IdentityOf(const acl::AuditEvent &event, std::mt19937_64 &random);
//! `acl SELECT` / `acl MANAGE` / `acl NATIVE` - the base's statement class, never the SQL text (C5);
//! `acl session` for a session close; a lifecycle kind that never becomes a span keeps its kind
string SpanNameOf(const acl::AuditEvent &event);
//! kError only when the reason code says our side failed (`source_error`, `policy_error`); a refusal
//! by policy is the system working and stays kUnset, like an allowed decision
bool SpanIsError(const acl::AuditEvent &event);

//! The OTLP span exporter (spec 008): the trace twin of OtlpExporter, built from the same config so
//! an operator configures one endpoint and gets the third signal, fed by the sink's span lane.
class OtlpTraceExporter : public Exporter {
public:
	//! throws InvalidInputException on a config the SDK refuses (a bad protocol, a bad endpoint)
	explicit OtlpTraceExporter(const OtlpConfig &config);
	~OtlpTraceExporter() override;
	//! one span per event; false with `error` set when an event with no measured duration reached
	//! the lane (the sink's gate never lets one through) or the transport failed
	bool Export(const vector<acl::AuditEvent> &batch, string &error) override;
	string Describe() const override;
	//! fill one SDK span from one event: `[ts_us - duration, ts_us]`, kInternal, the name, the
	//! status, the identity, and the same attributes the record carries. Shared with the tests.
	static void FillSpan(opentelemetry::sdk::trace::Recordable &span, const acl::AuditEvent &event,
	                     const SpanIdentity &identity, const vector<string> &claims);

private:
	OtlpConfig config;
	string description;
	std::mt19937_64 random; // Export runs on the lane's one worker; the tests call it from one thread
	std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter; // the SDK factory hands out std::
	unique_ptr<opentelemetry::sdk::resource::Resource> resource;
	unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope> scope;
};

} // namespace acl_otel
} // namespace duckdb
