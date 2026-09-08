// The sink (spec 001, R1.5 / R6.2 / R7 / R10.1), on its own: OnEvent returns at once whatever the
// exporter does, batches reach the exporter in order and in the configured size, a full queue drops
// and counts, an exporter that fails is counted as export errors, none configured as "no exporter",
// Flush drains what is queued and never waits on a stuck transport longer than its bound, and Stop
// joins the worker. Build + run via `GEN=ninja make test-cpp`.

#include "acl_otel.hpp"
#include "acl_otel_test_util.hpp"

#include <chrono>
#include <thread>

using namespace duckdb;
using namespace acl_otel_test;

namespace {

struct Recording : acl_otel::Exporter {
	std::mutex lock;
	std::vector<std::vector<int64_t>> batches; // the seqs of each batch
	bool fail = false;
	int64_t stall_ms = 0;
	bool Export(const vector<acl::AuditEvent> &batch, string &error) override {
		if (stall_ms > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
		}
		std::lock_guard<std::mutex> guard(lock);
		std::vector<int64_t> seqs;
		for (auto &event : batch) {
			seqs.push_back(event.seq);
		}
		batches.push_back(std::move(seqs));
		if (fail) {
			error = "the collector said no";
			return false;
		}
		return true;
	}
	string Describe() const override {
		return "recording";
	}
	idx_t Count() {
		std::lock_guard<std::mutex> guard(lock);
		idx_t n = 0;
		for (auto &b : batches) {
			n += b.size();
		}
		return n;
	}
};

acl::AuditEvent MakeEvent(int64_t seq) {
	acl::AuditEvent event;
	event.seq = seq;
	event.kind = "statement";
	event.allowed = seq % 3 != 0;
	return event;
}

template <class F>
bool Within(int64_t ms, F done) {
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < until) {
		if (done()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return done();
}

} // namespace

int main() {
	std::printf("test_acl_otel_sink\n");
	{
		auto recording = make_shared_ptr<Recording>();
		acl_otel::OtelSink sink(100, 4, 50, recording);
		for (int64_t i = 1; i <= 10; i++) {
			sink.OnEvent(MakeEvent(i));
		}
		Check(Within(2000, [&] { return recording->Count() == 10; }), "ten events exported");
		Check(sink.stats.received == 10 && sink.stats.exported == 10, "received and exported counted");
		std::lock_guard<std::mutex> guard(recording->lock);
		Check(recording->batches.size() >= 3 && recording->batches[0].size() == 4 && recording->batches[0][0] == 1 &&
		          recording->batches[0][3] == 4,
		      "batches of the configured size, in seq order");
	}
	{
		// a stuck exporter never holds OnEvent, and Flush is bounded
		auto recording = make_shared_ptr<Recording>();
		recording->stall_ms = 5000;
		acl_otel::OtelSink sink(8, 2, 20, recording);
		auto started = std::chrono::steady_clock::now();
		for (int64_t i = 1; i <= 20; i++) {
			sink.OnEvent(MakeEvent(i));
		}
		auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
		Check(spent.count() < 500,
		      "twenty OnEvent calls behind a stalled exporter took " + std::to_string(spent.count()) + "ms");
		Check(sink.stats.dropped_queue > 0,
		      "the full queue dropped and counted (" + std::to_string(sink.stats.dropped_queue.load()) + ")");
		started = std::chrono::steady_clock::now();
		sink.Flush();
		spent = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
		Check(spent.count() < 3000,
		      "Flush behind a stalled exporter returned in " + std::to_string(spent.count()) + "ms");
		sink.Stop();
	}
	{
		// a failing exporter: counted as export errors, with the last error kept
		auto recording = make_shared_ptr<Recording>();
		recording->fail = true;
		acl_otel::OtelSink sink(100, 1, 20, recording);
		sink.OnEvent(MakeEvent(1));
		Check(Within(2000, [&] { return sink.stats.export_errors == 1; }), "a refused batch is an export error");
		Check(sink.stats.exported == 0, "nothing counted as exported");
	}
	{
		// no exporter configured: dropped and said so, never an "error"
		acl_otel::OtelSink sink(100, 1, 20, nullptr);
		sink.OnEvent(MakeEvent(1));
		Check(Within(2000, [&] { return sink.stats.dropped_no_exporter == 1; }), "no exporter: dropped as such");
		Check(sink.stats.export_errors == 0, "...and not as an export error");
		Check(sink.ExporterName().rfind("none", 0) == 0, "the status names the stand-in");
		// swapping the transport in: the next batch goes there
		auto recording = make_shared_ptr<Recording>();
		sink.SetExporter(recording);
		sink.OnEvent(MakeEvent(2));
		Check(Within(2000, [&] { return recording->Count() == 1; }), "the swapped-in exporter receives the next event");
	}
	{
		// Stop: what is still queued is counted as dropped, OnEvent after Stop drops too
		auto recording = make_shared_ptr<Recording>();
		recording->stall_ms = 300;
		acl_otel::OtelSink sink(100, 50, 60000, recording);
		for (int64_t i = 1; i <= 10; i++) {
			sink.OnEvent(MakeEvent(i));
		}
		sink.Stop();
		sink.OnEvent(MakeEvent(11));
		Check(sink.stats.received == 11 && sink.stats.dropped_queue + sink.stats.exported == 11,
		      "after Stop every event is accounted for (exported or dropped)");
	}
	std::printf("PASS\n");
	return 0;
}
