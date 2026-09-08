//===----------------------------------------------------------------------===//
// acl_otel_otlp.hpp - the OTLP log exporter (spec 002)
//
// One SDK LogRecordExporter (HTTP/protobuf or gRPC) driven from the sink's worker, batch by batch:
// one Recordable per event, filled from the event, one Export call. No SDK processor, no second
// queue, no second thread - the sink of spec 001 keeps its semantics. The mapping (R1.1-R1.3) is
// a free function so the tests judge it without a transport.
//===----------------------------------------------------------------------===//

#pragma once

#include "acl_otel.hpp"

// the SDK's namespace is an inline `v1`: a forward declaration of our own would make a second
// `opentelemetry::sdk` and every `sdk::` in the SDK's headers ambiguous - so the headers themselves
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/logs/exporter.h"
#include "opentelemetry/sdk/logs/recordable.h"
#include "opentelemetry/sdk/resource/resource.h"

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
	string duckdb_version;
	string acl_otel_version;
	//! from the settings (through the instance) and the base's acl_node_id when acl is loaded
	static OtlpConfig From(DatabaseInstance &db);
	//! `acl_otel_protocol` accepts the two protocols, or '' for the environment's
	static bool ValidProtocol(const string &protocol);
	//! the protocol this config stands for: the setting, else the environment, else http/protobuf
	string ResolvedProtocol() const;
};

//! `scheme://user:secret@host/...` -> `scheme://***@host/...`: a credential written into the
//! endpoint is the operator's choice against R9.2, and the status still never prints it
string MaskUserinfo(const string &url);

//! The W3C traceparent, parsed: `00-<32 hex>-<16 hex>-<2 hex>`; false on anything else
bool ParseTraceparent(const string &traceparent, uint8_t trace_id[16], uint8_t span_id[8], uint8_t &flags);

//! severity per R1.1: INFO allowed, WARN denied, ERROR when the source of the decision failed
enum class OtelSeverity : uint8_t { INFO, WARN, ERROR };
OtelSeverity SeverityOf(const acl::AuditEvent &event);
//! the body: `<kind> <allowed|denied>` + `: <reason>`
string BodyOf(const acl::AuditEvent &event);
//! `acl.roles` and `acl.objects` as JSON strings - flat attributes, never nested (R1.6)
string RolesJson(const acl::AuditEvent &event);
string ObjectsJson(const acl::AuditEvent &event);

class OtlpExporter : public Exporter {
public:
	//! throws InvalidInputException on a config the SDK refuses (a bad protocol, a bad endpoint)
	explicit OtlpExporter(const OtlpConfig &config);
	~OtlpExporter() override;
	bool Export(const vector<acl::AuditEvent> &batch, string &error) override;
	string Describe() const override;
	//! the header NAMES the transport carries (R9.2: never a value)
	vector<string> HeaderNames() const;
	//! fill one SDK record from one event - the mapping of R1.1-R1.2, shared with the tests
	static void Fill(opentelemetry::sdk::logs::Recordable &record, const acl::AuditEvent &event);

private:
	OtlpConfig config;
	string description;
	vector<string> header_names;
	std::unique_ptr<opentelemetry::sdk::logs::LogRecordExporter> exporter; // the SDK factory hands out std::
	unique_ptr<opentelemetry::sdk::resource::Resource> resource;
	unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope> scope;
};

} // namespace acl_otel
} // namespace duckdb
