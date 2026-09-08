// The sink and its worker (spec 001, R1.5 / R6.2 / R7 / R10.1): the base's audit thread hands us an
// event, we copy it onto a bounded queue and return; one worker thread pops batches and exports.
// Nothing here can slow a decision: the base already decoupled delivery from the decision, and this
// decouples the network from delivery.

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

OtelSink::OtelSink(idx_t queue_size_p, idx_t batch_size_p, int64_t flush_interval_ms_p, shared_ptr<Exporter> exporter_p)
    // a batch larger than the queue would never be reached, and OnEvent would stop waking the worker
    // at all: the queue would fill, drop, and drain only on the flush timer. So a batch is at most a
    // queue.
    : queue_size(queue_size_p == 0 ? 1 : queue_size_p),
      batch_size(MinValue<idx_t>(batch_size_p == 0 ? 1 : batch_size_p, queue_size_p == 0 ? 1 : queue_size_p)),
      flush_interval_ms(flush_interval_ms_p <= 0 ? 1 : flush_interval_ms_p), exporter(std::move(exporter_p)) {
	if (!exporter) {
		exporter = make_shared_ptr<NoneExporter>();
	}
	worker = std::thread([this] { Run(); });
}

OtelSink::~OtelSink() {
	Stop();
}

void OtelSink::SetMetrics(shared_ptr<OtelMetrics> metrics_p) {
	std::lock_guard<std::mutex> guard(lock);
	metrics = std::move(metrics_p);
}

void OtelSink::OnEvent(const acl::AuditEvent &event) {
	stats.received++;
	{
		// spec 003 first: an event that the queue then drops still shaped a histogram, and the base's
		// own counters counted it too - the two agree
		shared_ptr<OtelMetrics> observers;
		{
			std::lock_guard<std::mutex> guard(lock);
			observers = metrics;
		}
		if (observers) {
			observers->Observe(event);
		}
	}
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

void OtelSink::Flush() {
	FlushNow();
}

bool OtelSink::FlushNow() {
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

void OtelSink::SetExporter(shared_ptr<Exporter> exporter_p) {
	std::lock_guard<std::mutex> guard(lock);
	exporter = exporter_p ? std::move(exporter_p) : make_shared_ptr<NoneExporter>();
}

string OtelSink::ExporterName() {
	std::lock_guard<std::mutex> guard(lock);
	return exporter->Describe();
}

string OtelSink::LastError() {
	std::lock_guard<std::mutex> guard(lock);
	return stats.last_error;
}

idx_t OtelSink::QueueFill() {
	std::lock_guard<std::mutex> guard(lock);
	return queue.size();
}

void OtelSink::Stop() {
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

void OtelSink::Run() {
	std::unique_lock<std::mutex> guard(lock);
	while (!stopping) {
		wake.wait_for(guard, std::chrono::milliseconds(flush_interval_ms),
		              [this] { return stopping || flush_requested || queue.size() >= batch_size; });
		while (!queue.empty() && !stopping) {
			vector<acl::AuditEvent> batch;
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

void OtelSink::ExportBatch(vector<acl::AuditEvent> &batch) {
	string error;
	shared_ptr<Exporter> transport;
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

} // namespace acl_otel
} // namespace duckdb
