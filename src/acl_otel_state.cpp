// The per-instance state (spec 001, R8): where the sink and the policy live, how they attach to the
// base's registry and detach from it, and what acl_otel_status() reports.

#include "acl_otel.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"

#include <chrono>

namespace duckdb {
namespace acl_otel {

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
				out += StringUtil::Format("\\u%04x", static_cast<int>(static_cast<unsigned char>(c)));
			} else {
				out += c;
			}
		}
	}
	return out + "\"";
}

} // namespace

string SettingString(DatabaseInstance &db, const char *name, const string &fallback) {
	Value value;
	if (db.TryGetCurrentSetting(name, value) && !value.IsNull()) {
		return value.ToString();
	}
	return fallback;
}

int64_t SettingInt64(DatabaseInstance &db, const char *name, int64_t fallback) {
	Value value;
	if (db.TryGetCurrentSetting(name, value) && !value.IsNull()) {
		return value.GetValue<int64_t>();
	}
	return fallback;
}

shared_ptr<OtelState> OtelState::Of(DatabaseInstance &db) {
	return db.GetObjectCache().GetOrCreate<OtelState>(ObjectType());
}

OtelState::~OtelState() {
	Stop();
}

shared_ptr<OtelSink> OtelState::Sink() {
	std::lock_guard<std::mutex> guard(lock);
	return sink;
}

shared_ptr<OtelPolicy> OtelState::Policy() {
	std::lock_guard<std::mutex> guard(lock);
	return policy;
}

bool OtelState::Attached() {
	std::lock_guard<std::mutex> guard(lock);
	return attached;
}

bool OtelState::Start(DatabaseInstance &db) {
	std::lock_guard<std::mutex> guard(lock);
	if (attached) {
		return false;
	}
	// the registry: whoever comes first creates it, the base adopts it when it loads (C2)
	hooks = db.GetObjectCache().GetOrCreate<acl::AuditHooks>(acl::AuditHooks::ObjectType());
	auto queue = SettingInt64(db, "acl_otel_queue_size", 10000);
	auto batch = SettingInt64(db, "acl_otel_batch_size", 512);
	auto flush = SettingInt64(db, "acl_otel_flush_interval", 5);
	sink = make_shared_ptr<OtelSink>(NumericCast<idx_t>(MaxValue<int64_t>(queue, 1)),
	                                 NumericCast<idx_t>(MaxValue<int64_t>(batch, 1)),
	                                 MaxValue<int64_t>(flush, 1) * 1000, make_shared_ptr<NoneExporter>());
	if (!policy) {
		policy = make_shared_ptr<OtelPolicy>();
		policy->SetRules(ParseLevelRules(rules_json));
	}
	hooks->AddSink(sink);
	hooks->SetSessionPolicy(policy);
	attached = true;
	return true;
}

bool OtelState::Stop() {
	shared_ptr<OtelSink> ending;
	{
		std::lock_guard<std::mutex> guard(lock);
		if (!attached) {
			return false;
		}
		attached = false;
		if (hooks) {
			hooks->RemoveSink(sink);
			hooks->SetSessionPolicy(nullptr);
		}
		ending = std::move(sink);
	}
	// outside the lock: the worker may be mid-export, and Stop waits for it
	if (ending) {
		ending->Flush();
		ending->Stop();
	}
	return true;
}

void OtelState::SetRulesJson(const string &json) {
	auto rules = ParseLevelRules(json); // refused here, at the SET, never at a session open
	std::lock_guard<std::mutex> guard(lock);
	rules_json = json;
	if (!policy) {
		policy = make_shared_ptr<OtelPolicy>();
	}
	policy->SetRules(std::move(rules));
}

string OtelState::StatusJson(DatabaseInstance &db) {
	shared_ptr<OtelSink> current;
	shared_ptr<OtelPolicy> rules;
	bool is_attached;
	{
		std::lock_guard<std::mutex> guard(lock);
		current = sink;
		rules = policy;
		is_attached = attached;
	}
	string json = "{\"attached\":" + string(is_attached ? "true" : "false");
	json += ",\"acl_loaded\":" +
	        string(db.GetObjectCache().Get<acl::AuditHooks>(acl::AuditHooks::ObjectType()) ? "true" : "false");
	json += ",\"endpoint\":" + JsonQuote(SettingString(db, "acl_otel_endpoint", ""));
	json += ",\"exporter\":" + JsonQuote(current ? current->ExporterName() : "none (stopped)");
	json += ",\"rules\":" + std::to_string(rules ? rules->RuleCount() : 0);
	if (current) {
		auto &stats = current->stats;
		json += ",\"queue_fill\":" + std::to_string(current->QueueFill());
		json += ",\"queue_size\":" + std::to_string(current->QueueSize());
		json += ",\"received\":" + std::to_string(stats.received.load());
		json += ",\"exported\":" + std::to_string(stats.exported.load());
		json += ",\"batches\":" + std::to_string(stats.batches.load());
		json += ",\"dropped\":{\"queue\":" + std::to_string(stats.dropped_queue.load()) +
		        ",\"no_exporter\":" + std::to_string(stats.dropped_no_exporter.load()) + "}";
		json += ",\"export_errors\":" + std::to_string(stats.export_errors.load());
		auto last = stats.last_export_us.load();
		json += ",\"last_export_us\":" + (last > 0 ? std::to_string(last) : string("null"));
	}
	json += "}";
	return json;
}

} // namespace acl_otel
} // namespace duckdb
