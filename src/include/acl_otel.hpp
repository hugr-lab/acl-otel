//===----------------------------------------------------------------------===//
// acl_otel.hpp - the extension's own types (spec 001)
//
// Everything acl_otel is, minus the network: the level rules (R3), the sink that queues events off
// the base's audit thread (R1.5) - one lane for the records (spec 002) and one for the spans (spec
// 008) - the exporter seam a transport plugs into, the numbers the extension reports about itself
// (R7), and the per-instance state that ties them together and lives in duckdb's object cache
// beside the base's hook registry.
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
//! spec 007: one row of the central table as a rule. NULL, '' and '*' all mean "any"; an unknown
//! level throws, naming the row.
LevelRule RuleFromRow(const string &role, const string &subject, const string &issuer, const string &door,
                      const string &level, int64_t seq);
bool RuleMatches(const LevelRule &rule, const acl::Principal &principal, const string &door);

class OtelMetrics;     // spec 003, acl_otel_metrics.hpp
class MetricsExporter; // its transport seam
struct MetricPoint;    // one number as a transport receives it

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

//! Spec 006 (R7.3): is this node losing events, and for how long. A pure judgement over the drop
//! counters, the clock and a window - the tests hand it their own clock rather than sleeping.
class Health {
public:
	//! `drops` is every loss that is NOT a policy (queue + no_exporter + export errors; never
	//! sampling). True while the count has grown within `window_s` of `now_us`.
	bool Losing(int64_t drops, int64_t now_us, int64_t window_s);
	//! when the last loss was seen, 0 when there has been none
	int64_t LosingSince() const {
		return since_us;
	}

private:
	int64_t seen = -1; // -1 = never judged: the first judgement only takes a baseline
	int64_t since_us = 0;
};

//! The extension's own numbers (R7): every event received, exported, dropped (by why), failed.
struct Stats {
	std::atomic<int64_t> received {0};
	std::atomic<int64_t> exported_batches_failed {0};
	std::atomic<int64_t> exported {0};
	std::atomic<int64_t> dropped_queue {0};
	std::atomic<int64_t> dropped_no_exporter {0};
	std::atomic<int64_t> sampled {0}; // spec 005: allowed statements kept out by ratio, not by failure
	//! spec 008, the span lane only: events whose caller's traceparent says the trace is not recorded
	//! (flags 00) - the caller's decision, obeyed, and counted apart from a loss
	std::atomic<int64_t> unsampled {0};
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

//! spec 008: which decisions become spans. `off` allocates nothing; `linked` takes only an event
//! carrying a usable traceparent (the span always has a parent, and a node nobody traces costs
//! nothing); `all` takes every recorded decision, rooting a trace of its own when there is no parent.
enum class TraceMode : uint8_t { OFF, LINKED, ALL };
//! `off` / `linked` / `all` (case-insensitive, trimmed); false on anything else
bool ParseTraceMode(const string &text, TraceMode &out);
const char *TraceModeName(TraceMode mode);
//! The W3C traceparent, parsed: `00-<32 hex>-<16 hex>-<2 hex>`; false on anything else, an all-zero
//! id included (invalid by the specification). Shared by the record (R1.2) and the span (spec 008).
bool ParseTraceparent(const string &traceparent, uint8_t trace_id[16], uint8_t span_id[8], uint8_t &flags);
//! spec 008: is this event one a span can honestly be built from, under `mode`? Both ends must be
//! known - a decision's `rewrite_us`, or a session close's `duration_us` when `session_spans` is on;
//! nothing else ever becomes a span, because a zero-length span reads as "instant", a claim the base
//! never made. Under `linked` the caller's traceparent must be there and parse. `unsampled` is set
//! when the event would have qualified but the caller's flags say the trace is not recorded: not a
//! span, and the caller counts it as such. O(1): the traceparent is 55 characters.
bool SpanCandidate(const acl::AuditEvent &event, TraceMode mode, bool session_spans, bool &unsampled);
//! the span's length in microseconds - `rewrite_us` for a decision, `duration_us` for a session
//! close - or -1 when the base measured neither
int64_t SpanDurationUs(const acl::AuditEvent &event);

//! A bounded queue and the one worker that drains it into an Exporter, batch by batch (R1.5,
//! R10.1): Push copies the event and returns - no I/O, no wait; a full queue drops and counts.
//! FlushNow asks the worker to drain what is queued now and waits for that, bounded by a timeout, so
//! a stuck exporter cannot hold the caller. The sink runs one of these for the records (spec 002)
//! and, while traces are on, one for the spans (spec 008): the same semantics, counted apart.
class EventQueue {
public:
	EventQueue(idx_t queue_size, idx_t batch_size, int64_t flush_interval_ms, shared_ptr<Exporter> exporter);
	~EventQueue();

	//! O(1): onto the queue, or a counted drop. Never counts `received` - the caller does, so a lane
	//! can count what it was handed before its own policy thinned it.
	void Push(const acl::AuditEvent &event);
	//! true when what was queued at the call was exported (or failed and counted) within the bound,
	//! false when the transport is still on it or the queue is stopping
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
	//! ends the worker; after this Push drops and counts
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
	bool stopping = false;
	bool flush_requested = false;
	std::thread worker;
};

//! The sink (R1.5, R10.1): OnEvent hands the event to the metrics accumulators (spec 003), asks the
//! sampler (spec 005), copies what survives onto the records' queue and - while traces are on and
//! the event is one a span can be built from - onto the spans' (spec 008), and returns. Flush() (the
//! base calls it on a level change and at shutdown) drains both lanes, each bounded, so a stuck
//! exporter cannot hold the base's audit thread.
class OtelSink : public acl::AuditSink {
	EventQueue records; // first, before `stats` below refers to it

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
	//! spec 008: the span lane, or null while traces are off; `mode` and `session_spans` decide which
	//! events reach it. A record and its span are sampled together: the lane is fed after the sampler.
	void SetTraces(shared_ptr<EventQueue> lane, TraceMode mode, bool session_spans);
	shared_ptr<EventQueue> Traces();
	TraceMode TracesMode();
	bool SessionSpans();
	//! the records' lane: what acl_otel_flush() drains, whose numbers the status reports first
	EventQueue &Records() {
		return records;
	}
	//! Flush of the records that answers: true when what was queued at the call was exported (or
	//! failed and counted) within the bound, false when the transport is still on it or the sink
	//! is stopping
	bool FlushNow();

	void SetExporter(shared_ptr<Exporter> exporter);
	string ExporterName();
	string LastError();
	idx_t QueueFill();
	idx_t QueueSize() const {
		return records.QueueSize();
	}
	//! ends both workers; after this OnEvent drops and counts
	void Stop();
	//! the records' numbers (R7), the same object as Records().stats
	Stats &stats {records.stats};

private:
	std::mutex lock;
	shared_ptr<OtelMetrics> metrics; // under `lock`, spec 003
	shared_ptr<Sampler> sampler;     // under `lock`, spec 005; null = keep everything
	shared_ptr<EventQueue> spans;    // under `lock`, spec 008; null = traces off
	TraceMode trace_mode = TraceMode::OFF;
	bool session_spans = false;
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
	//! spec 008: drain the span lane now (acl_otel_traces_flush); false when traces are off or the
	//! bound ran out
	bool FlushTraces();
	//! spec 008: the span lane from `acl_otel_traces` / `acl_otel_session_spans` - as they will be
	//! after this SET, when `changed` names one - on the running sink: `off` flushes and tears the
	//! lane down, anything else builds it (with the transport of the records' settings) or retunes it
	void ReconfigureTraces(DatabaseInstance &db, const string &changed = string(), const Value &value = Value());
	//! spec 006 (R7.2/R7.3): 1 unless `acl_otel_strict` is on and this node is losing events or is
	//! not attached at all. Judged here, from the sink's own counters and the clock.
	bool Healthy(DatabaseInstance &db);
	//! the extension's own numbers, as metric points for the scrape (R7.2)
	vector<MetricPoint> SelfMetrics(DatabaseInstance &db);
	void SetRulesJson(const string &json); // parses, then hot-reloads the policy (R3.1)
	//! spec 007: read the central table now; returns how many rules are in force, or throws with
	//! what went wrong. Leaves the rules as they were when the read fails.
	idx_t RefreshRules(DatabaseInstance &db);
	//! read once and start the reader, when a table is named at all
	void StartRules(DatabaseInstance &db);
	//! start/stop the reader's own thread; the table setting decides whether it runs at all
	void StartRulesReader(DatabaseInstance &db);
	void StopRulesReader();
	//! `setting`, `table` or `none` - which of the two sources is in force
	string RulesSource(DatabaseInstance &db);
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
	//! spec 008: the span transport, from the same settings as the logs'
	shared_ptr<Exporter> BuildTraceExporter(DatabaseInstance &db, const string &changed, const Value &value);
	//! spec 008: ReconfigureTraces on a given sink, lock-free (Start calls it under the lock)
	void ApplyTraces(DatabaseInstance &db, const string &changed, const Value &value, OtelSink &target);
	//! spec 008: fold a stopped lane's losses into the running total strict health judges
	void RetireLane(EventQueue &lane);
	int64_t retired_span_losses = 0; // under `lock`
	vector<string> header_names;     // under `lock`
	string sampling_document = "1";  // under `lock`, spec 005: kept for a restart
	Health health;                   // under `lock`, spec 006
	// spec 007: the reader of the central table, and what it last saw
	std::thread rules_worker;
	std::condition_variable rules_wake;
	bool rules_stopping = false;
	int64_t rules_read_us = 0; // under `lock`
	string rules_error;        // under `lock`, '' when the last read worked
	idx_t rules_in_force = 0;  // under `lock`
	void ReadRules(DatabaseInstance &db);
	string attach_error; // under `lock`: why the last Start refused to attach, '' when it did
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
bool SettingBool(DatabaseInstance &db, const char *name, bool fallback);

} // namespace acl_otel
} // namespace duckdb
