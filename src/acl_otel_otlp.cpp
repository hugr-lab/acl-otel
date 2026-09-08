// The OTLP log exporter (spec 002): the mapping of an audit event onto an OpenTelemetry log record,
// and the SDK transport that carries a batch - HTTP/protobuf or gRPC - driven from the sink's
// worker. Credentials come from the environment only (R9.2); the status names header keys, never
// values.

#include "acl_otel_otlp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"

#include "opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_factory.h"
#include "opentelemetry/logs/severity.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/trace_flags.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_log_record_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_factory.h"
#include "opentelemetry/exporters/otlp/otlp_http_log_record_exporter_options.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/logs/exporter.h"
#include "opentelemetry/sdk/logs/recordable.h"
#include "opentelemetry/sdk/resource/resource.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#ifdef _WIN32
#include <process.h>
#define ACL_OTEL_PID _getpid()
#else
#include <unistd.h>
#define ACL_OTEL_PID getpid()
#endif

namespace duckdb {
namespace acl_otel {

namespace otlp = opentelemetry::exporter::otlp;
namespace sdklogs = opentelemetry::sdk::logs;

namespace {

//! The SDK reports what went wrong on its own internal log, to stderr by default - a database server
//! does not write there. This handler keeps the last error so the status can say why an export
//! failed (R6.3) and drops the rest; process-wide (the SDK's singleton), installed once.
class QuietLogHandler : public opentelemetry::sdk::common::internal_log::LogHandler {
public:
	void Handle(opentelemetry::sdk::common::internal_log::LogLevel level, const char *, int, const char *msg,
	            const opentelemetry::sdk::common::AttributeMap &) noexcept override {
		if (level != opentelemetry::sdk::common::internal_log::LogLevel::Error || !msg) {
			return;
		}
		std::lock_guard<std::mutex> guard(lock);
		if (first_error.empty()) { // the first line is the cause; what follows restates it
			first_error = msg;
		}
	}
	string Take() {
		std::lock_guard<std::mutex> guard(lock);
		string out = std::move(first_error);
		first_error.clear();
		return out;
	}

private:
	std::mutex lock;
	string first_error;
};

QuietLogHandler &SdkLog() {
	static QuietLogHandler *handler = [] {
		auto created = new QuietLogHandler(); // owned by the SDK's singleton from here on
		opentelemetry::sdk::common::internal_log::GlobalLogHandler::SetLogLevel(
		    opentelemetry::sdk::common::internal_log::LogLevel::Error);
		opentelemetry::sdk::common::internal_log::GlobalLogHandler::SetLogHandler(
		    opentelemetry::nostd::shared_ptr<opentelemetry::sdk::common::internal_log::LogHandler>(created));
		return created;
	}();
	return *handler;
}

} // namespace

string MaskUserinfo(const string &url) {
	auto scheme = url.find("://");
	auto start = scheme == string::npos ? 0 : scheme + 3;
	auto path = url.find('/', start);
	auto at = url.rfind('@', path == string::npos ? string::npos : path);
	if (at == string::npos || at < start) {
		return url;
	}
	return url.substr(0, start) + "***" + url.substr(at);
}

namespace {

string JsonQuote(const string &value) {
	string out = "\"";
	for (auto c : value) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buffer[8];
				std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
				out += buffer;
			} else {
				out += c;
			}
		}
	}
	return out + "\"";
}

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

//! the `k=v,k=v` form OTel uses for resource attributes
vector<std::pair<string, string>> ParsePairs(const string &text) {
	vector<std::pair<string, string>> pairs;
	for (auto &item : StringUtil::Split(text, ',')) {
		auto trimmed = item;
		StringUtil::Trim(trimmed);
		if (trimmed.empty()) {
			continue;
		}
		auto eq = trimmed.find('=');
		if (eq == string::npos) {
			continue;
		}
		auto key = trimmed.substr(0, eq);
		auto value = trimmed.substr(eq + 1);
		StringUtil::Trim(key);
		StringUtil::Trim(value);
		if (!key.empty()) {
			pairs.emplace_back(key, value);
		}
	}
	return pairs;
}

} // namespace

bool OtlpConfig::ValidProtocol(const string &protocol) {
	return protocol.empty() || protocol == "http/protobuf" || protocol == "grpc";
}

string OtlpConfig::ResolvedProtocol() const {
	if (!protocol.empty()) {
		return protocol;
	}
	for (auto name : {"OTEL_EXPORTER_OTLP_LOGS_PROTOCOL", "OTEL_EXPORTER_OTLP_PROTOCOL"}) {
		const char *env = std::getenv(name);
		if (env && string(env) == "grpc") {
			return "grpc";
		}
		if (env && *env) {
			break; // http/protobuf, or http/json which is served as protobuf (what every collector takes)
		}
	}
	return "http/protobuf";
}

OtlpConfig OtlpConfig::From(DatabaseInstance &db) {
	OtlpConfig config;
	config.endpoint = SettingString(db, "acl_otel_endpoint", "");
	config.protocol = SettingString(db, "acl_otel_protocol", "");
	config.timeout_s = SettingInt64(db, "acl_otel_timeout", 0);
	Value insecure;
	config.insecure =
	    db.TryGetCurrentSetting("acl_otel_insecure", insecure) && !insecure.IsNull() && insecure.GetValue<bool>();
	config.certificate = SettingString(db, "acl_otel_certificate", "");
	config.service_name = SettingString(db, "acl_otel_service_name", "duckdb-acl");
	config.resource_attributes = SettingString(db, "acl_otel_resource_attributes", "");
	// the node id is the base's (spec 069) when acl is loaded; else the same shape, ours
	config.instance_id = SettingString(db, "acl_node_id", "");
	if (config.instance_id.empty()) {
		char host[256];
		host[0] = '\0';
#ifndef _WIN32
		gethostname(host, sizeof(host));
#endif
		config.instance_id = string(host[0] ? host : "node") + ":" + std::to_string(static_cast<int64_t>(ACL_OTEL_PID));
	}
	config.duckdb_version = DuckDB::LibraryVersion();
#ifdef EXT_VERSION_ACL_OTEL
	config.acl_otel_version = EXT_VERSION_ACL_OTEL;
#endif
	return config;
}

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

OtelSeverity SeverityOf(const acl::AuditEvent &event) {
	if (event.allowed) {
		return OtelSeverity::INFO;
	}
	// the source of the decision failed, not the principal: the policy catalog, the keys' location
	if (event.reason_code == "source_error" || event.reason_code == "policy_error" || event.kind == "policy" ||
	    event.kind == "keys") {
		return OtelSeverity::ERROR;
	}
	return OtelSeverity::WARN;
}

string BodyOf(const acl::AuditEvent &event) {
	string body = event.kind + (event.allowed ? " allowed" : " denied");
	if (!event.reason.empty()) {
		body += ": " + event.reason;
	}
	return body;
}

string RolesJson(const acl::AuditEvent &event) {
	string json = "[";
	for (idx_t i = 0; i < event.principal.roles.size(); i++) {
		json += (i ? "," : "") + JsonQuote(event.principal.roles[i]);
	}
	return json + "]";
}

string ObjectsJson(const acl::AuditEvent &event) {
	string json = "[";
	for (idx_t i = 0; i < event.objects.size(); i++) {
		json += string(i ? "," : "") + "{\"name\":" + JsonQuote(event.objects[i].name) +
		        ",\"capability\":" + JsonQuote(event.objects[i].capability) + "}";
	}
	return json + "]";
}

void OtlpExporter::Fill(sdklogs::Recordable &record, const acl::AuditEvent &event) {
	using opentelemetry::common::SystemTimestamp;
	record.SetTimestamp(SystemTimestamp(std::chrono::microseconds(event.ts_us)));
	record.SetObservedTimestamp(SystemTimestamp(std::chrono::system_clock::now()));
	switch (SeverityOf(event)) {
	case OtelSeverity::INFO:
		record.SetSeverity(opentelemetry::logs::Severity::kInfo);
		break;
	case OtelSeverity::WARN:
		record.SetSeverity(opentelemetry::logs::Severity::kWarn);
		break;
	default:
		record.SetSeverity(opentelemetry::logs::Severity::kError);
	}
	auto body = BodyOf(event);
	record.SetBody(opentelemetry::common::AttributeValue(opentelemetry::nostd::string_view(body)));
	// R1.2: the trace context, or nothing - a malformed value sets neither and is not exported
	uint8_t trace_id[16], span_id[8], flags = 0;
	if (!event.traceparent.empty() && ParseTraceparent(event.traceparent, trace_id, span_id, flags)) {
		record.SetTraceId(opentelemetry::trace::TraceId(opentelemetry::nostd::span<const uint8_t, 16>(trace_id, 16)));
		record.SetSpanId(opentelemetry::trace::SpanId(opentelemetry::nostd::span<const uint8_t, 8>(span_id, 8)));
		record.SetTraceFlags(opentelemetry::trace::TraceFlags(flags));
	}
	// R1.1: every field an `acl.` attribute, present only when the event carries it. The values are
	// copied by the recordable at SetAttribute (the OTLP recordable serialises into its proto), so
	// temporaries are fine here.
	auto text = [&](const char *key, const string &value) {
		if (!value.empty()) {
			record.SetAttribute(key, opentelemetry::common::AttributeValue(opentelemetry::nostd::string_view(value)));
		}
	};
	auto number = [&](const char *key, int64_t value) {
		if (value >= 0) {
			record.SetAttribute(key, opentelemetry::common::AttributeValue(value));
		}
	};
	text("acl.kind", event.kind);
	text("acl.statement", event.statement);
	text("acl.verdict", event.allowed ? "allowed" : "denied");
	text("acl.reason_code", event.reason_code);
	text("acl.reason", event.reason);
	text("acl.door", event.door);
	text("acl.session", event.session);
	text("acl.subject", event.principal.subject);
	text("acl.issuer", event.principal.issuer);
	if (!event.principal.roles.empty()) {
		text("acl.roles", RolesJson(event));
	}
	if (!event.objects.empty()) {
		text("acl.objects", ObjectsJson(event));
	}
	text("acl.correlation_id", event.correlation_id);
	number("acl.rewrite_us", event.rewrite_us);
	number("acl.rows", event.rows);
	number("acl.duration_us", event.duration_us);
	text("acl.detail", event.detail);
	text("acl.level", acl::AuditLevelName(event.level));
	number("acl.seq", event.seq);
	text("acl.node", event.node);
}

OtlpExporter::OtlpExporter(const OtlpConfig &config_p) : config(config_p) {
	SdkLog();
	if (!OtlpConfig::ValidProtocol(config.protocol)) {
		throw InvalidInputException("acl_otel_protocol accepts 'http/protobuf' or 'grpc', not '%s'", config.protocol);
	}
	auto protocol = config.ResolvedProtocol();
	// the resource (R1.3): the node, the versions, and what the operator adds
	opentelemetry::sdk::resource::ResourceAttributes attributes;
	attributes.SetAttribute("service.name", config.service_name);
	attributes.SetAttribute("service.instance.id", config.instance_id);
	attributes.SetAttribute("duckdb.version", config.duckdb_version);
	if (!config.acl_otel_version.empty()) {
		attributes.SetAttribute("acl_otel.version", config.acl_otel_version);
	}
	for (auto &pair : ParsePairs(config.resource_attributes)) {
		attributes.SetAttribute(pair.first, pair.second);
	}
	resource =
	    make_uniq<opentelemetry::sdk::resource::Resource>(opentelemetry::sdk::resource::Resource::Create(attributes));
	auto created = opentelemetry::sdk::instrumentationscope::InstrumentationScope::Create(
	    "acl_otel", config.acl_otel_version.empty() ? "dev" : config.acl_otel_version);
	scope = unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope>(created.release());
	// a timeout set here wins; unset, the SDK's own (OTEL_EXPORTER_OTLP_TIMEOUT, else 10 s) stands
	auto timeout = std::chrono::seconds(config.timeout_s);
	if (protocol == "grpc") {
		// the options' default constructor reads OTEL_EXPORTER_OTLP_* (endpoint, headers, timeout,
		// certificate); a setting that is set wins
		otlp::OtlpGrpcLogRecordExporterOptions options;
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
		for (auto &header : options.metadata) {
			header_names.push_back(header.first);
		}
		description = "otlp/grpc -> " + MaskUserinfo(options.endpoint);
		exporter = otlp::OtlpGrpcLogRecordExporterFactory::Create(options);
	} else {
		otlp::OtlpHttpLogRecordExporterOptions options;
		if (!config.endpoint.empty()) {
			auto url = config.endpoint;
			// the OTLP convention: a base URL gets the logs path, a URL that already names it is kept
			if (url.find("/v1/logs") == string::npos) {
				if (!url.empty() && url.back() == '/') {
					url.pop_back();
				}
				url += "/v1/logs";
			}
			options.url = url;
		}
		if (config.timeout_s > 0) {
			options.timeout = timeout;
		}
		if (!config.certificate.empty()) {
			options.ssl_ca_cert_path = config.certificate;
		}
		for (auto &header : options.http_headers) {
			header_names.push_back(header.first);
		}
		description = "otlp/http -> " + MaskUserinfo(options.url);
		exporter = otlp::OtlpHttpLogRecordExporterFactory::Create(options);
	}
}

OtlpExporter::~OtlpExporter() {
	if (exporter) {
		exporter->Shutdown(std::chrono::seconds(2));
	}
}

bool OtlpExporter::Export(const vector<acl::AuditEvent> &batch, string &error) {
	vector<std::unique_ptr<sdklogs::Recordable>> records;
	records.reserve(batch.size());
	for (auto &event : batch) {
		auto record = exporter->MakeRecordable();
		record->SetResource(*resource);
		record->SetInstrumentationScope(*scope);
		Fill(*record, event);
		records.push_back(std::move(record));
	}
	SdkLog().Take(); // what the SDK says about THIS export, not an earlier one
	auto result = exporter->Export(
	    opentelemetry::nostd::span<std::unique_ptr<sdklogs::Recordable>>(records.data(), records.size()));
	// The HTTP log exporter of the SDK (1.24) answers kSuccess whatever its client said - a refused
	// connection or a 4xx from the collector reaches only its internal log. So an error the SDK
	// logged during this export IS the failure (R6.2: a batch that did not arrive is counted), and
	// its words are the reason the status shows.
	auto why = SdkLog().Take();
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

string OtlpExporter::Describe() const {
	return description;
}

vector<string> OtlpExporter::HeaderNames() const {
	return header_names;
}

} // namespace acl_otel
} // namespace duckdb
