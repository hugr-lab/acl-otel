// The sink and its lanes (spec 001, R1.5 / R6.2 / R7 / R10.1): the base's audit thread hands us an
// event, we copy it onto a bounded queue and return; one worker thread per lane pops batches and
// exports. Nothing here can slow a decision: the base already decoupled delivery from the decision,
// and this decouples the network from delivery. The records (spec 002) and the spans (spec 008) are
// two lanes of the same shape, so a slow trace backend never holds a log record and each is
// counted apart.

#include "acl_otel.hpp"

#include "acl_otel_metrics.hpp"

#include <chrono>

namespace duckdb {
namespace acl_otel {

namespace {

int64_t NowMicros() {
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
	    .count();
}

} // namespace

template <class E>
EventQueueOf<E>::EventQueueOf(idx_t queue_size_p, idx_t batch_size_p, int64_t flush_interval_ms_p,
                              shared_ptr<ExporterOf<E>> exporter_p)
    // a batch larger than the queue would never be reached, and Push would stop waking the worker
    // at all: the queue would fill, drop, and drain only on the flush timer. So a batch is at most a
    // queue.
    : queue_size(queue_size_p == 0 ? 1 : queue_size_p),
      batch_size(MinValue<idx_t>(batch_size_p == 0 ? 1 : batch_size_p, queue_size_p == 0 ? 1 : queue_size_p)),
      flush_interval_ms(flush_interval_ms_p <= 0 ? 1 : flush_interval_ms_p), exporter(std::move(exporter_p)) {
	if (!exporter) {
		exporter = make_shared_ptr<NoneExporterOf<E>>();
	}
	worker = std::thread([this] { Run(); });
}

template <class E>
EventQueueOf<E>::~EventQueueOf() {
	Stop();
}

template <class E>
void EventQueueOf<E>::Push(const E &event) {
	{
		std::lock_guard<std::mutex> guard(lock);
		if (stopping || queue.size() >= queue_size) {
			stats.dropped_queue++;
			return;
		}
		queue.push_back(event);
		if (queue.size() < batch_size) {
			return; // the worker wakes on its own clock; a full batch wakes it now
		}
	}
	wake.notify_one();
}

template <class E>
bool EventQueueOf<E>::FlushNow() {
	std::unique_lock<std::mutex> guard(lock);
	if (stopping) {
		return false;
	}
	flush_requested = true;
	wake.notify_one();
	// bounded: the base calls this on its audit thread, and a transport that hangs must not hold it
	drained.wait_for(guard, std::chrono::seconds(2), [this] { return !flush_requested || stopping; });
	return !flush_requested && !stopping;
}

template <class E>
void EventQueueOf<E>::SetExporter(shared_ptr<ExporterOf<E>> exporter_p) {
	shared_ptr<ExporterOf<E>> previous;
	{
		std::lock_guard<std::mutex> guard(lock);
		previous = std::move(exporter);
		exporter = exporter_p ? std::move(exporter_p) : make_shared_ptr<NoneExporterOf<E>>();
	}
	// `previous` dies HERE, outside the lock: an SDK exporter's destructor shuts its client down
	// (bounded, but up to two seconds), and Push must not wait behind that on the audit thread
}

template <class E>
string EventQueueOf<E>::ExporterName() {
	std::lock_guard<std::mutex> guard(lock);
	return exporter->Describe();
}

template <class E>
string EventQueueOf<E>::LastError() {
	std::lock_guard<std::mutex> guard(lock);
	return stats.last_error;
}

template <class E>
idx_t EventQueueOf<E>::QueueFill() {
	std::lock_guard<std::mutex> guard(lock);
	return queue.size();
}

template <class E>
void EventQueueOf<E>::Stop() {
	{
		std::lock_guard<std::mutex> guard(lock);
		if (stopping) {
			return;
		}
		stopping = true;
	}
	wake.notify_all();
	drained.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
}

template <class E>
void EventQueueOf<E>::Run() {
	std::unique_lock<std::mutex> guard(lock);
	while (!stopping) {
		wake.wait_for(guard, std::chrono::milliseconds(flush_interval_ms),
		              [this] { return stopping || flush_requested || queue.size() >= batch_size; });
		while (!queue.empty() && !stopping) {
			vector<E> batch;
			while (!queue.empty() && batch.size() < batch_size) {
				batch.push_back(std::move(queue.front()));
				queue.pop_front();
			}
			guard.unlock();
			ExportBatch(batch);
			guard.lock();
		}
		if (flush_requested) {
			flush_requested = false;
			drained.notify_all();
		}
	}
	// what is still queued at the end is lost, and said so (R6.2)
	stats.dropped_queue += NumericCast<int64_t>(queue.size());
	queue.clear();
}

template <class E>
void EventQueueOf<E>::ExportBatch(vector<E> &batch) {
	string error;
	shared_ptr<ExporterOf<E>> transport;
	{
		std::lock_guard<std::mutex> guard(lock);
		transport = exporter; // a batch finishes on the transport it started with
	}
	bool ok = transport->Export(batch, error);
	stats.batches++;
	if (ok) {
		stats.exported += NumericCast<int64_t>(batch.size());
		stats.last_export_us = NowMicros();
		return;
	}
	if (!transport->Configured()) {
		stats.dropped_no_exporter += NumericCast<int64_t>(batch.size());
		return;
	}
	stats.export_errors += NumericCast<int64_t>(batch.size());
	stats.exported_batches_failed++;
	std::lock_guard<std::mutex> guard(lock);
	stats.last_error = error;
}

// the two event types the lanes carry: the base's (spec 001) and tresor's (spec 011)
template class EventQueueOf<acl::AuditEvent>;
template class EventQueueOf<tresor::TresorAuditEvent>;

OtelSink::OtelSink(idx_t queue_size, idx_t batch_size, int64_t flush_interval_ms, shared_ptr<Exporter> exporter)
    : records(queue_size, batch_size, flush_interval_ms, std::move(exporter)) {
}

OtelSink::~OtelSink() {
	Stop();
}

void OtelSink::SetMetrics(shared_ptr<OtelMetrics> metrics_p) {
	std::lock_guard<std::mutex> guard(lock);
	metrics = std::move(metrics_p);
}

void OtelSink::SetSampler(shared_ptr<Sampler> sampler_p) {
	std::lock_guard<std::mutex> guard(lock);
	sampler = std::move(sampler_p);
}

double OtelSink::SampleRatio(const vector<string> &roles) {
	shared_ptr<Sampler> ratio;
	{
		std::lock_guard<std::mutex> guard(lock);
		ratio = sampler;
	}
	return ratio ? ratio->RatioFor(roles) : 1.0;
}

void OtelSink::SetTraces(shared_ptr<EventQueue> lane, TraceMode mode, bool session_spans_p) {
	shared_ptr<EventQueue> previous;
	{
		std::lock_guard<std::mutex> guard(lock);
		previous = std::move(spans);
		spans = std::move(lane);
		trace_mode = spans ? mode : TraceMode::OFF;
		session_spans = session_spans_p;
	}
	// `previous` dies HERE, outside the lock, when this was its last reference: a lane's destructor
	// joins its worker, which may be mid-export, and OnEvent must not wait behind that
}

shared_ptr<EventQueue> OtelSink::Traces() {
	std::lock_guard<std::mutex> guard(lock);
	return spans;
}

TraceMode OtelSink::TracesMode() {
	std::lock_guard<std::mutex> guard(lock);
	return trace_mode;
}

bool OtelSink::SessionSpans() {
	std::lock_guard<std::mutex> guard(lock);
	return session_spans;
}

void OtelSink::OnEvent(const acl::AuditEvent &event) {
	stats.received++;
	if (event.kind == "profile") {
		profiles_received++; // spec 009
	}
	shared_ptr<OtelMetrics> observers;
	shared_ptr<Sampler> ratio;
	shared_ptr<EventQueue> lane;
	TraceMode mode;
	bool sessions;
	{
		std::lock_guard<std::mutex> guard(lock);
		observers = metrics;
		ratio = sampler;
		lane = spans;
		mode = trace_mode;
		sessions = session_spans;
	}
	// spec 003 first: an event that the queue then drops still shaped a histogram, and the base's
	// own counters counted it too - the two agree
	if (observers) {
		observers->Observe(event);
	}
	// spec 005: what a backend stores may be thinned; what the node counted above never is. A
	// record and its span are one decision, so the span lane is fed from here too (spec 008).
	if (ratio && !ratio->Keep(event)) {
		stats.sampled++;
		return;
	}
	records.Push(event);
	if (lane) {
		bool unsampled = false;
		if (SpanCandidate(event, mode, sessions, unsampled)) {
			lane->stats.received++;
			lane->Push(event);
		} else if (unsampled) {
			lane->stats.received++;
			lane->stats.unsampled++;
		}
	}
}

void OtelSink::Flush() {
	// both lanes, each within its own bound: the base calls this on its audit thread
	records.FlushNow();
	auto lane = Traces();
	if (lane) {
		lane->FlushNow();
	}
}

bool OtelSink::FlushNow() {
	return records.FlushNow();
}

void OtelSink::SetExporter(shared_ptr<Exporter> exporter) {
	records.SetExporter(std::move(exporter));
}

string OtelSink::ExporterName() {
	return records.ExporterName();
}

string OtelSink::LastError() {
	return records.LastError();
}

idx_t OtelSink::QueueFill() {
	return records.QueueFill();
}

void OtelSink::Stop() {
	records.Stop();
	auto lane = Traces();
	if (lane) {
		lane->Stop(); // outside our lock: Stop joins the lane's worker, which may be mid-export
	}
}

} // namespace acl_otel
} // namespace duckdb
