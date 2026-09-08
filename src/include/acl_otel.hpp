//===----------------------------------------------------------------------===//
// acl_otel.hpp - the extension's own types (spec 001)
//
// Everything acl_otel is, minus the network: the level rules (R3), the sink that queues events off
// the base's audit thread (R1.5), the exporter seam a transport plugs into (spec 002), the numbers
// the extension reports about itself (R7), and the per-instance state that ties them together and
// lives in duckdb's object cache beside the base's hook registry.
//
// The base is reached through ONE header, acl_audit.hpp (header-only, spec 069): a loadable
// extension is dlopen'd RTLD_LOCAL, so nothing here ever calls a symbol of acl's - it calls what it
// compiled, on the registry object it got from the cache.
//===----------------------------------------------------------------------===//

#pragma once

#include "acl_audit.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/storage/object_cache.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace duckdb {
namespace acl_otel {

//! One level rule (R3): first match wins; an empty field, or `*`, matches anything.
struct LevelRule {
	string role;
	string subject;
	string issuer;
	string door;
	acl::AuditLevel level = acl::AuditLevel::DECISIONS;
};

//! `acl_otel_level_rules`: a JSON array of objects, each with any of role / subject / issuer / door
//! and a level. Throws InvalidInputException naming what is wrong; "" or "[]" is no rules.
vector<LevelRule> ParseLevelRules(const string &json);
bool RuleMatches(const LevelRule &rule, const acl::Principal &principal, const string &door);

class OtelMetrics;     // spec 003, acl_otel_metrics.hpp
class MetricsExporter; // its transport seam

//! Spec 005: which `allowed` statements are kept. A ratio ('0.1'), or a JSON object per role
//! ({"analyst": 0.05, "*": 0.5}) where `*` covers a role the object does not name; a principal with
//! several roles is judged by the HIGHEST ratio any of them names, because sampling is a cost
//! control and a role an operator kept whole should not be thinned by another the principal holds.
class Sampler {
public:
	//! Throws InvalidInputException when the document is neither a ratio in [0, 1] nor an object of
	//! role -> ratio. '' and '1' keep everything.
	explicit Sampler(const string &document);
	Sampler() = default;

	//! false = this event is sampled away. Only an `allowed` STATEMENT is eligible; a refusal, an
	//! admin decision, a session, a door, an ingest, a policy or a keys event always passes (R6.1).
	bool Keep(const acl::AuditEvent &event) const;
	//! the ratio this principal's roles earn, for the status
	double RatioFor(const vector<string> &roles) const;
	bool KeepsEverything() const {
		return everything;
	}

private:
	bool everything = true;
	double fallback = 1.0;            // the ratio with no role named, or `*`
	std::map<string, double> by_role; // the roles the document names
};

//! The extension's own numbers (R7): every event received, exported, dropped (by why), failed.
struct Stats {
	std::atomic<int64_t> received {0};
	std::atomic<int64_t> exported_batches_failed {0};
	std::atomic<int64_t> exported {0};
	std::atomic<int64_t> dropped_queue {0};
	std::atomic<int64_t> dropped_no_exporter {0};
	std::atomic<int64_t> sampled {0}; // spec 005: allowed statements kept out by ratio, not by failure
	std::atomic<int64_t> export_errors {0};
	std::atomic<int64_t> batches {0};
	std::atomic<int64_t> last_export_us {0}; // epoch microseconds of the last successful export
	string last_error;                       // guarded by the sink's lock
};

//! What carries a batch out. spec 002 plugs OTLP in here; the extension without an endpoint carries
//! a `NoneExporter` that drops and counts (R6.2: never silent).
struct Exporter {
	virtual ~Exporter() = default;
	//! false with `error` set on a failure; the batch is then counted as failed, never retried here
	virtual bool Export(const vector<acl::AuditEvent> &batch, string &error) = 0;
	virtual string Describe() const = 0;
	//! false for the exporter that stands in while nothing is configured: its drops are counted
	//! as "no exporter", not as export errors
	virtual bool Configured() const {
		return true;
	}
};

struct NoneExporter : Exporter {
	bool Export(const vector<acl::AuditEvent> &, string &) override {
		return false; // nothing is configured to receive them
	}
	string Describe() const override {
		return "none (acl_otel_endpoint is empty)";
	}
	bool Configured() const override {
		return false;
	}
};

//! The sink (R1.5, R10.1): OnEvent copies the event onto a bounded queue and returns - no I/O, no
//! wait; a full queue drops and counts. One worker thread pops batches and hands them to the
//! exporter; Flush() (the base calls it on a level change and at shutdown) asks the worker to drain
//! what is queued now and waits for that, bounded by a timeout, so a stuck exporter cannot hold the
//! base's audit thread.
class OtelSink : public acl::AuditSink {
public:
	OtelSink(idx_t queue_size, idx_t batch_size, int64_t flush_interval_ms, shared_ptr<Exporter> exporter);
	~OtelSink() override;

	void OnEvent(const acl::AuditEvent &event) override;
	void Flush() override;
	//! spec 003: the metrics accumulators see every event this sink receives, before the queue -
	//! O(bounds) and one short lock, so R10.1 holds. Null while metrics are off.
	void SetMetrics(shared_ptr<OtelMetrics> metrics);
	//! spec 005: the sampler, applied AFTER the metrics have seen the event - what thins is the
	//! record a backend stores, never the number the node counts
	void SetSampler(shared_ptr<Sampler> sampler);
	double SampleRatio(const vector<string> &roles);
	//! Flush that answers: true when what was queued at the call was exported (or failed and
	//! counted) within the bound, false when the transport is still on it or the sink is stopping
	bool FlushNow();

	//! swap the transport (a setting changed); the worker sees it on its next batch, and a batch in
	//! flight finishes on the transport it started with (the pointer is shared)
	void SetExporter(shared_ptr<Exporter> exporter);
	string ExporterName();
	string LastError();
	idx_t QueueFill();
	idx_t QueueSize() const {
		return queue_size;
	}
	Stats stats;
	//! ends the worker; after this OnEvent drops and counts
	void Stop();

private:
	void Run();
	void ExportBatch(vector<acl::AuditEvent> &batch);

	idx_t queue_size;
	idx_t batch_size;
	int64_t flush_interval_ms;
	std::mutex lock;
	std::condition_variable wake;
	std::condition_variable drained;
	std::deque<acl::AuditEvent> queue;
	shared_ptr<Exporter> exporter;
	shared_ptr<OtelMetrics> metrics; // under `lock`, spec 003
	shared_ptr<Sampler> sampler;     // under `lock`, spec 005; null = keep everything
	bool stopping = false;
	bool flush_requested = false;
	std::thread worker;
};

//! The session policy (R3): the rules as last set, first match wins; no rule = no opinion.
class OtelPolicy : public acl::SessionPolicy {
public:
	bool LevelFor(const acl::Principal &principal, const string &door, acl::AuditLevel &out) override;
	void SetRules(vector<LevelRule> rules_p);
	idx_t RuleCount();

private:
	std::mutex lock;
	vector<LevelRule> rules;
};

//! The per-instance state, in the object cache beside the base's registry (R8.3: two instances in
//! one process are two of these). Created at load; Start attaches the sink and the policy to the
//! hooks, Stop flushes and detaches them (idempotent both ways). The destructor is the shutdown seam:
//! the instance is going, the worker is joined, the sink is off the registry.
class OtelState : public ObjectCacheEntry {
public:
	static string ObjectType() {
		return "acl_otel_state";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx(); // never evicted: the registration must outlive cache pressure
	}
	~OtelState() override;

	//! reached from a setting's callback, a function or Load - one per DatabaseInstance
	static shared_ptr<OtelState> Of(DatabaseInstance &db);

	//! attach to the instance's registry - false when already attached, or when the registry there
	//! is stamped with another contract version (AuditHooks::Reach): then nothing of ours is
	//! registered and AttachError() says why (C2a of the contract)
	bool Start(DatabaseInstance &db);
	bool Stop();
	bool Attached();
	string AttachError();
	//! drain the queue now (acl_otel_flush): false when not attached or the bound ran out
	bool Flush();
	//! spec 003: export one metrics tick now (acl_otel_metrics_flush); false when metrics are off
	bool FlushMetrics();
	void SetRulesJson(const string &json); // parses, then hot-reloads the policy (R3.1)
	//! spec 005: the sampler from `acl_otel_sample_allowed`, parsed (and refused) at the SET
	void SetSampling(const string &document);
	//! spec 002: rebuild the transport from the settings - `changed` names the setting whose new
	//! value is `value` (a SET's callback runs before the value is stored) - and swap it into the
	//! sink; an endpoint from neither a setting nor the environment means the stand-in
	void Reconfigure(DatabaseInstance &db, const string &changed = string(), const Value &value = Value());
	string StatusJson(DatabaseInstance &db);

	shared_ptr<OtelSink> Sink();
	shared_ptr<OtelPolicy> Policy();
	//! the header names the transport carries (never a value), '' when there is no transport
	vector<string> HeaderNames();
	//! spec 003: the opt-in series and their cap, read from the settings onto a live scrape
	void ApplySeriesSettings(DatabaseInstance &db, OtelMetrics &target);
	//! ... after a SET, with the value being set (the callback runs before it is stored)
	void ReconfigureSeries(DatabaseInstance &db, const string &changed, const Value &value);
	void ReconfigureSeries(DatabaseInstance &db, const string &changed, const Value &value, OtelMetrics &target);
	//! R2.5's pair, on a running scrape
	void SetHistogramSums(bool on);

private:
	//! lock-free (Start calls it under the lock); `names` receives the transport's header names
	shared_ptr<Exporter> BuildExporter(DatabaseInstance &db, const string &changed, const Value &value,
	                                   vector<string> &names);
	//! spec 003: the metrics transport, from the same settings as the logs'
	shared_ptr<MetricsExporter> BuildMetricsExporter(DatabaseInstance &db, const string &changed, const Value &value);
	vector<string> header_names;    // under `lock`
	string sampling_document = "1"; // under `lock`, spec 005: kept for a restart
	string attach_error;            // under `lock`: why the last Start refused to attach, '' when it did
	std::mutex lock;
	shared_ptr<acl::AuditHooks> hooks; // held: the registry outlives our sink's removal
	shared_ptr<OtelSink> sink;
	shared_ptr<OtelMetrics> metrics; // spec 003, null while acl_otel_metrics is off
	shared_ptr<OtelPolicy> policy;
	bool attached = false;
	string rules_json;
};

//! The settings, read through the instance (GLOBAL only)
string SettingString(DatabaseInstance &db, const char *name, const string &fallback);
int64_t SettingInt64(DatabaseInstance &db, const char *name, int64_t fallback);

} // namespace acl_otel
} // namespace duckdb
