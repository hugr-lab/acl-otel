// The metrics transport (spec 003) against a fake OTLP receiver in-process, the way spec 002's test
// does for logs: HTTP/protobuf on the bundled httplib (`POST /v1/metrics`) and gRPC on the SDK's
// generated MetricsService. What it asserts is the mapping - the base's counters as cumulative
// monotonic sums and its gauges as gauges under their own names, our histograms with their bounds
// and counts, the `_sum` / `_count` pair of R2.5, the resource, and a start_ts that does not move
// between ticks. Built by CMake (it links the SDK); run via `make test-cpp`.

#include "acl_otel_metrics.hpp"
#include "acl_otel_otlp.hpp"
#include "acl_otel_test_util.hpp"

#include "httplib.hpp"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.grpc.pb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h"

#include <grpcpp/grpcpp.h>

#include <map>
#include <mutex>
#include <thread>

using namespace duckdb;
using namespace acl_otel_test;
namespace proto_metrics = opentelemetry::proto::collector::metrics::v1;
namespace proto_common = opentelemetry::proto::common::v1;

namespace {

//! one metric as a backend would index it
struct Received {
	string name;
	string unit;
	string kind; // sum | gauge | histogram
	bool monotonic = false;
	bool cumulative = false;
	uint64_t start_ns = 0;
	std::map<string, int64_t> by_labels;      // "k=v,k=v" -> value (sums and gauges)
	std::map<string, string> histogram_shape; // "k=v" -> "count/sum/bounds"
};

string LabelsOf(const google::protobuf::RepeatedPtrField<proto_common::KeyValue> &attributes) {
	std::map<string, string> ordered;
	for (auto &kv : attributes) {
		ordered[kv.key()] = kv.value().string_value();
	}
	string out;
	for (auto &entry : ordered) {
		out += (out.empty() ? "" : ",") + entry.first + "=" + entry.second;
	}
	return out;
}

struct Capture {
	std::mutex lock;
	std::map<string, string> resource;
	std::map<string, Received> metrics;
	int requests = 0;

	void Take(const proto_metrics::ExportMetricsServiceRequest &request) {
		std::lock_guard<std::mutex> guard(lock);
		requests++;
		for (auto &rm : request.resource_metrics()) {
			for (auto &kv : rm.resource().attributes()) {
				resource[kv.key()] = kv.value().string_value();
			}
			for (auto &sm : rm.scope_metrics()) {
				for (auto &metric : sm.metrics()) {
					Received received;
					received.name = metric.name();
					received.unit = metric.unit();
					if (metric.has_sum()) {
						received.kind = "sum";
						received.monotonic = metric.sum().is_monotonic();
						received.cumulative = metric.sum().aggregation_temporality() == 2; // CUMULATIVE
						for (auto &point : metric.sum().data_points()) {
							received.by_labels[LabelsOf(point.attributes())] = point.as_int();
							received.start_ns = point.start_time_unix_nano();
						}
					} else if (metric.has_gauge()) {
						received.kind = "gauge";
						for (auto &point : metric.gauge().data_points()) {
							received.by_labels[LabelsOf(point.attributes())] = point.as_int();
						}
					} else if (metric.has_histogram()) {
						received.kind = "histogram";
						received.cumulative = metric.histogram().aggregation_temporality() == 2;
						for (auto &point : metric.histogram().data_points()) {
							string shape = std::to_string(point.count()) + "/" + std::to_string(int64_t(point.sum())) +
							               "/" + std::to_string(point.explicit_bounds_size()) + "/" +
							               std::to_string(point.bucket_counts_size());
							received.histogram_shape[LabelsOf(point.attributes())] = shape;
							received.start_ns = point.start_time_unix_nano();
						}
					}
					metrics[received.name] = std::move(received);
				}
			}
		}
	}
	Received At(const string &name) {
		std::lock_guard<std::mutex> guard(lock);
		auto found = metrics.find(name);
		return found == metrics.end() ? Received {} : found->second;
	}
	bool Has(const string &name) {
		std::lock_guard<std::mutex> guard(lock);
		return metrics.find(name) != metrics.end();
	}
};

struct HttpReceiver {
	Capture capture;
	duckdb_httplib::Server server;
	int port = 0;
	std::thread thread;
	HttpReceiver() {
		server.Post("/v1/metrics", [this](const duckdb_httplib::Request &req, duckdb_httplib::Response &res) {
			proto_metrics::ExportMetricsServiceRequest request;
			if (!request.ParseFromString(req.body)) {
				res.status = 400;
				return;
			}
			capture.Take(request);
			proto_metrics::ExportMetricsServiceResponse response;
			res.set_content(response.SerializeAsString(), "application/x-protobuf");
		});
		port = server.bind_to_any_port("127.0.0.1");
		thread = std::thread([this] { server.listen_after_bind(); });
	}
	~HttpReceiver() {
		server.stop();
		thread.join();
	}
};

struct GrpcReceiver : proto_metrics::MetricsService::Service {
	Capture capture;
	std::unique_ptr<grpc::Server> server;
	int port = 0;
	GrpcReceiver() {
		grpc::ServerBuilder builder;
		builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
		builder.RegisterService(this);
		server = builder.BuildAndStart();
	}
	~GrpcReceiver() override {
		server->Shutdown();
	}
	grpc::Status Export(grpc::ServerContext *, const proto_metrics::ExportMetricsServiceRequest *request,
	                    proto_metrics::ExportMetricsServiceResponse *) override {
		capture.Take(*request);
		return grpc::Status::OK;
	}
};

acl_otel::OtlpConfig Config(const string &endpoint, const string &protocol) {
	acl_otel::OtlpConfig config;
	config.endpoint = endpoint;
	config.protocol = protocol;
	config.timeout_s = 5;
	config.insecure = true;
	config.service_name = "acl-test";
	config.instance_id = "node-a:42";
	config.duckdb_version = "2.0.0-test";
	config.acl_otel_version = "0.0.1-test";
	return config;
}

acl::AuditEvent Statement(bool allowed, int64_t rewrite_us, const string &role) {
	acl::AuditEvent event;
	event.kind = "statement";
	event.allowed = allowed;
	event.rewrite_us = rewrite_us;
	event.principal.roles = {role};
	return event;
}

//! the base's registry with a few numbers on it, as a node would have
void Seed(acl::AuditHooks &hooks) {
	hooks.Counters().Add("acl.decisions", {{"verdict", "allowed"}});
	hooks.Counters().Add("acl.decisions", {{"verdict", "allowed"}});
	hooks.Counters().Add("acl.decisions", {{"verdict", "denied"}});
	hooks.Gauges().Register("acl.sessions.live", {{"door", "flight"}}, "1", "sessions alive right now",
	                        [] { return int64_t(3); });
}

void CheckTick(Capture &capture, const string &transport) {
	Check(capture.resource["service.name"] == "acl-test" && capture.resource["service.instance.id"] == "node-a:42",
	      transport + ": the resource is the node's, the same as the logs'");
	auto decisions = capture.At("acl.decisions");
	Check(decisions.kind == "sum" && decisions.monotonic && decisions.cumulative,
	      transport + ": a base counter is a cumulative monotonic sum");
	Check(decisions.by_labels["verdict=allowed"] == 2 && decisions.by_labels["verdict=denied"] == 1,
	      transport + ": ...with the base's own numbers, per attribute tuple");
	auto sessions = capture.At("acl.sessions.live");
	Check(sessions.kind == "gauge" && sessions.by_labels["door=flight"] == 3 && sessions.unit == "1",
	      transport + ": a base gauge is a gauge, read at snapshot time, with its unit");
	auto rewrite = capture.At("acl.rewrite.duration");
	Check(rewrite.kind == "histogram" && rewrite.cumulative, transport + ": our histogram is a cumulative histogram");
	Check(rewrite.histogram_shape["kind=statement,verdict=allowed"] == "2/300/9/10",
	      transport + ": ...with the count, the sum, its nine bounds and ten buckets (" +
	          rewrite.histogram_shape["kind=statement,verdict=allowed"] + ")");
	Check(capture.At("acl.rewrite.duration_sum").by_labels["kind=statement,verdict=allowed"] == 300 &&
	          capture.At("acl.rewrite.duration_count").by_labels["kind=statement,verdict=allowed"] == 2,
	      transport + ": R2.5's _sum / _count pair rides beside it");
	Check(capture.At("acl.decisions.by_role").by_labels["role=analyst,verdict=allowed"] == 2,
	      transport + ": the opt-in series is exported as a counter");
	Check(!capture.Has("acl.session.duration"),
	      transport + ": an instrument nothing was recorded into is not a series at all");
}

} // namespace

int main() {
	std::printf("test_acl_otel_metrics_otlp\n");
	{
		HttpReceiver receiver;
		acl::AuditHooks hooks;
		Seed(hooks);
		acl_otel::OtelMetrics metrics(15, acl_otel::DefaultHistograms(),
		                              acl_otel::MakeOtlpMetricsExporter(Config(
		                                  "http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf")));
		metrics.SetSeries({"by_role"}, "", 100, {});
		metrics.Observe(Statement(true, 100, "analyst"));
		metrics.Observe(Statement(true, 200, "analyst"));
		Check(metrics.TickNow(hooks), "http: the tick was exported");
		CheckTick(receiver.capture, "http");
		auto first = receiver.capture.At("acl.decisions").start_ns;
		Check(metrics.TickNow(hooks), "http: a second tick");
		Check(receiver.capture.At("acl.decisions").start_ns == first && first > 0,
		      "http: start_ts does not move between ticks - the window is the run (cumulative)");
		Check(receiver.capture.requests == 2, "http: one request per tick");
	}
	{
		GrpcReceiver receiver;
		acl::AuditHooks hooks;
		Seed(hooks);
		acl_otel::OtelMetrics metrics(
		    15, acl_otel::DefaultHistograms(),
		    acl_otel::MakeOtlpMetricsExporter(Config("127.0.0.1:" + std::to_string(receiver.port), "grpc")));
		metrics.SetSeries({"by_role"}, "", 100, {});
		metrics.Observe(Statement(true, 100, "analyst"));
		metrics.Observe(Statement(true, 200, "analyst"));
		Check(metrics.TickNow(hooks), "grpc: the tick was exported");
		CheckTick(receiver.capture, "grpc");
	}
	{
		// spec 006 (R7.2): the extension's own numbers ride along, named apart from the node's
		HttpReceiver receiver;
		acl::AuditHooks hooks;
		Seed(hooks);
		acl_otel::OtelMetrics metrics(15, acl_otel::DefaultHistograms(),
		                              acl_otel::MakeOtlpMetricsExporter(Config(
		                                  "http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf")));
		metrics.SetSelfMetrics([]() {
			vector<acl_otel::MetricPoint> mine;
			mine.push_back(acl_otel::MetricPoint {"acl_otel.received", {}, 12, true, "1", "handed"});
			mine.push_back(acl_otel::MetricPoint {"acl_otel.dropped", {{"why", "queue"}}, 3, true, "1", "lost"});
			mine.push_back(acl_otel::MetricPoint {"acl_otel.healthy", {}, 0, false, "1", "0 while losing"});
			return mine;
		});
		Check(metrics.TickNow(hooks), "the tick with our own numbers was exported");
		Check(receiver.capture.At("acl_otel.received").by_labels[""] == 12 &&
		          receiver.capture.At("acl_otel.received").kind == "sum",
		      "acl_otel.received arrives as a counter");
		Check(receiver.capture.At("acl_otel.dropped").by_labels["why=queue"] == 3,
		      "acl_otel.dropped carries its reason");
		Check(receiver.capture.At("acl_otel.healthy").kind == "gauge" &&
		          receiver.capture.At("acl_otel.healthy").by_labels[""] == 0,
		      "acl_otel.healthy arrives as a gauge");
		Check(receiver.capture.Has("acl.decisions"), "...beside the base's own, which are not renamed");
	}
	{
		// the pair can be turned off (R2.5 is for a bridge that needs it, not for everybody)
		HttpReceiver receiver;
		acl::AuditHooks hooks;
		acl_otel::OtelMetrics metrics(15, acl_otel::DefaultHistograms(),
		                              acl_otel::MakeOtlpMetricsExporter(Config(
		                                  "http://127.0.0.1:" + std::to_string(receiver.port), "http/protobuf")));
		metrics.SetHistogramSums(false);
		metrics.Observe(Statement(true, 100, "analyst"));
		Check(metrics.TickNow(hooks), "the tick with the pair off was exported");
		Check(receiver.capture.Has("acl.rewrite.duration") && !receiver.capture.Has("acl.rewrite.duration_sum"),
		      "...the histogram is there and the pair is not");
	}
	{
		// a port nobody listens on: counted as an export error with the SDK's reason (spec 002's rule)
		acl::AuditHooks hooks;
		acl_otel::OtelMetrics metrics(15, acl_otel::DefaultHistograms(),
		                              acl_otel::MakeOtlpMetricsExporter(Config("http://127.0.0.1:1", "http/protobuf")));
		Check(!metrics.TickNow(hooks), "a tick nobody receives does not export");
		Check(metrics.stats.export_errors == 1 && metrics.LastError().find("the export failed") == 0,
		      "...and is an export error with a reason (" + metrics.LastError() + ")");
	}
	std::printf("PASS\n");
	return 0;
}
