// acl_otel: OpenTelemetry for duckdb-acl's audit (spec 001) - the entry point.
//
// Loaded beside `acl` on the same instance, in either order (C2): Load creates the per-instance
// state, registers the settings and the operator's functions, and attaches the sink and the level
// policy to the base's hook registry (R8.1). Everything registered in SQL is `acl_otel_*`, which the
// base's function gate denies to a principal by prefix (C8) - the surface is the operator's by
// construction. Nothing here enforces, changes a decision, or can slow one.

#define DUCKDB_EXTENSION_MAIN

#include "acl_otel_extension.hpp"

#include "acl_otel.hpp"
#include "acl_otel_otlp.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

namespace {

using acl_otel::OtelState;

//! a setting the judgement reads through the instance: a session-scoped value would show in
//! current_setting() and change nothing, so it is refused outright (the base's own rule)
void RequireGlobal(const char *name, SetScope scope) {
	if (scope != SetScope::GLOBAL) {
		throw InvalidInputException("%s is global - use SET GLOBAL", name);
	}
}

void AclOtelVersionFunc(DataChunk &args, ExpressionState &, Vector &result) {
#ifdef EXT_VERSION_ACL_OTEL
	result.Reference(Value(EXT_VERSION_ACL_OTEL), count_t(args.size()));
#else
	result.Reference(Value(""), count_t(args.size()));
#endif
}

DatabaseInstance &InstanceOf(ExpressionState &state) {
	return *state.GetContext().db;
}

void AclOtelStatusFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = InstanceOf(state);
	result.Reference(Value(OtelState::Of(db)->StatusJson(db)), count_t(args.size()));
}

void AclOtelStartFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = InstanceOf(state);
	result.Reference(Value::BOOLEAN(OtelState::Of(db)->Start(db)), count_t(args.size()));
}

void AclOtelFlushFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = InstanceOf(state);
	result.Reference(Value::BOOLEAN(OtelState::Of(db)->Flush()), count_t(args.size()));
}

void AclOtelStopFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &db = InstanceOf(state);
	result.Reference(Value::BOOLEAN(OtelState::Of(db)->Stop()), count_t(args.size()));
}

//! the transport settings (spec 002): a SET's callback is a plain function pointer, so each setting
//! is one instance of the template below, named by its index into this table
enum class TransportSetting : uint8_t {
	ENDPOINT,
	PROTOCOL,
	TIMEOUT,
	INSECURE,
	CERTIFICATE,
	RESOURCE_ATTRIBUTES,
	SERVICE_NAME
};
const char *const TRANSPORT_SETTING_NAMES[] = {"acl_otel_endpoint",    "acl_otel_protocol",
                                               "acl_otel_timeout",     "acl_otel_insecure",
                                               "acl_otel_certificate", "acl_otel_resource_attributes",
                                               "acl_otel_service_name"};

template <TransportSetting SETTING>
void TransportSet(ClientContext &context, SetScope scope, Value &parameter) {
	const char *setting = TRANSPORT_SETTING_NAMES[static_cast<uint8_t>(SETTING)];
	RequireGlobal(setting, scope);
	if (SETTING == TransportSetting::PROTOCOL && !acl_otel::OtlpConfig::ValidProtocol(parameter.ToString())) {
		throw InvalidInputException("acl_otel_protocol accepts 'http/protobuf' or 'grpc', not '%s'",
		                            parameter.ToString());
	}
	OtelState::Of(*context.db)->Reconfigure(*context.db, setting, parameter);
}

void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	// R9.1: every setting GLOBAL, acl_otel_*, and never a principal's (the base's SET gate)
	// spec 002: the transport. A SET's callback runs before the value is stored, so the new value
	// is handed to the rebuild explicitly; a config the SDK refuses fails the SET
	config.AddExtensionOption("acl_otel_endpoint",
	                          "acl_otel: the OTLP endpoint - http(s)://host:4318 for http/protobuf (/v1/logs is "
	                          "appended), host:4317 for grpc; '' = the OTEL_EXPORTER_OTLP_ENDPOINT environment, and "
	                          "with neither nothing is exported and every drop is counted (spec 002)",
	                          LogicalType::VARCHAR, Value(""), TransportSet<TransportSetting::ENDPOINT>,
	                          SetScope::GLOBAL);
	config.AddExtensionOption(
	    "acl_otel_protocol", "acl_otel: http/protobuf or grpc; '' = OTEL_EXPORTER_OTLP_PROTOCOL, else http/protobuf",
	    LogicalType::VARCHAR, Value(""), TransportSet<TransportSetting::PROTOCOL>, SetScope::GLOBAL);
	config.AddExtensionOption("acl_otel_timeout",
	                          "acl_otel: seconds an export may take before it fails; 0 = OTEL_EXPORTER_OTLP_TIMEOUT, "
	                          "else the SDK's 10",
	                          LogicalType::BIGINT, Value::BIGINT(0), TransportSet<TransportSetting::TIMEOUT>,
	                          SetScope::GLOBAL);
	config.AddExtensionOption("acl_otel_insecure",
	                          "acl_otel: grpc without TLS; false = the SDK decides (the endpoint's scheme, "
	                          "OTEL_EXPORTER_OTLP_INSECURE)",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false), TransportSet<TransportSetting::INSECURE>,
	                          SetScope::GLOBAL);
	config.AddExtensionOption("acl_otel_certificate",
	                          "acl_otel: a CA certificate file for TLS; '' = OTEL_EXPORTER_OTLP_CERTIFICATE, else "
	                          "the system's",
	                          LogicalType::VARCHAR, Value(""), TransportSet<TransportSetting::CERTIFICATE>,
	                          SetScope::GLOBAL);
	config.AddExtensionOption("acl_otel_resource_attributes",
	                          "acl_otel: extra resource attributes on every export, k=v,k=v", LogicalType::VARCHAR,
	                          Value(""), TransportSet<TransportSetting::RESOURCE_ATTRIBUTES>, SetScope::GLOBAL);
	config.AddExtensionOption(
	    "acl_otel_level_rules",
	    "acl_otel: the audit level per role / subject / issuer / door, a JSON array of rules, first match "
	    "wins, applied when a session opens (R3)",
	    LogicalType::VARCHAR, Value(""),
	    [](ClientContext &context, SetScope scope, Value &parameter) {
		    RequireGlobal("acl_otel_level_rules", scope);
		    // parsed here: a malformed document is refused at the SET, and a good one is live at once
		    OtelState::Of(*context.db)->SetRulesJson(parameter.IsNull() ? string() : parameter.ToString());
	    },
	    SetScope::GLOBAL);
	config.AddExtensionOption(
	    "acl_otel_queue_size",
	    "acl_otel: events queued between the audit thread and the exporter; a full queue "
	    "drops and counts (takes effect at the next acl_otel_start)",
	    LogicalType::BIGINT, Value::BIGINT(10000),
	    [](ClientContext &, SetScope scope, Value &) { RequireGlobal("acl_otel_queue_size", scope); },
	    SetScope::GLOBAL);
	config.AddExtensionOption(
	    "acl_otel_batch_size", "acl_otel: events per export batch (at the next acl_otel_start)", LogicalType::BIGINT,
	    Value::BIGINT(512),
	    [](ClientContext &, SetScope scope, Value &) { RequireGlobal("acl_otel_batch_size", scope); },
	    SetScope::GLOBAL);
	config.AddExtensionOption(
	    "acl_otel_flush_interval",
	    "acl_otel: seconds between exports of a batch that did not fill (at the next "
	    "acl_otel_start)",
	    LogicalType::BIGINT, Value::BIGINT(5),
	    [](ClientContext &, SetScope scope, Value &) { RequireGlobal("acl_otel_flush_interval", scope); },
	    SetScope::GLOBAL);
	config.AddExtensionOption("acl_otel_service_name", "acl_otel: the OTel service.name on every export (spec 002)",
	                          LogicalType::VARCHAR, Value("duckdb-acl"), TransportSet<TransportSetting::SERVICE_NAME>,
	                          SetScope::GLOBAL);

	auto register_scalar = [&](const char *name, const LogicalType &returns, scalar_function_t fn) {
		ScalarFunction function(Identifier(name), {}, returns, std::move(fn));
		// volatile: each call reads the state as it is now; a folded call would answer the plan's moment
		function.SetVolatile();
		function.SetFallible();
		loader.RegisterFunction(function);
	};
	register_scalar("acl_otel_version", LogicalType::VARCHAR, AclOtelVersionFunc);
	register_scalar("acl_otel_status", LogicalType::VARCHAR, AclOtelStatusFunc);
	register_scalar("acl_otel_start", LogicalType::BOOLEAN, AclOtelStartFunc);
	register_scalar("acl_otel_stop", LogicalType::BOOLEAN, AclOtelStopFunc);
	// spec 002: export what is queued now and wait for it (bounded) - before a shutdown, or to see
	// in the status what the collector answered
	register_scalar("acl_otel_flush", LogicalType::BOOLEAN, AclOtelFlushFunc);

	// R8.1: attached at load, before or after acl - the registry is shared through the cache
	OtelState::Of(db)->Start(db);
}

} // namespace

void AclOtelExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string AclOtelExtension::Name() {
	return "acl_otel";
}

std::string AclOtelExtension::Version() const {
#ifdef EXT_VERSION_ACL_OTEL
	return EXT_VERSION_ACL_OTEL;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(acl_otel, loader) {
	duckdb::LoadInternal(loader);
}
}
