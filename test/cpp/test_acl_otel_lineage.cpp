// spec 018: the OpenLineage rendering of every payload shape, and the delivery policy against a fake
// poster - no SDK, no network.
#include "acl_otel_lineage.hpp"
#include "acl_otel_test_util.hpp"

#include "yyjson.hpp"

using namespace duckdb;
using namespace duckdb::acl_otel;
using namespace duckdb_yyjson;
using acl_otel_test::Check;

namespace {

LineageRenderOptions Options(bool physical = true) {
	LineageRenderOptions options;
	options.producer = "https://github.com/hugr-lab/acl-otel/tree/test";
	options.physical = physical;
	return options;
}

//! the value at a JSON pointer, as text ('' when absent)
struct Json {
	yyjson_doc *doc;
	explicit Json(const string &text) : doc(yyjson_read(text.c_str(), text.size(), 0)) {
	}
	~Json() {
		yyjson_doc_free(doc);
	}
	yyjson_val *At(const char *pointer) const {
		return doc ? yyjson_doc_ptr_get(doc, pointer) : nullptr;
	}
	string Str(const char *pointer) const {
		auto value = At(pointer);
		return value && yyjson_is_str(value) ? string(yyjson_get_str(value)) : string();
	}
	size_t Len(const char *pointer) const {
		auto value = At(pointer);
		return value ? yyjson_arr_size(value) + yyjson_obj_size(value) : 0;
	}
	bool Bool(const char *pointer) const {
		auto value = At(pointer);
		return value && yyjson_is_true(value);
	}
};

acl::AuditLineageDataset Dataset(const string &ns, const string &name, bool physical) {
	acl::AuditLineageDataset dataset;
	dataset.ns = ns;
	dataset.name = name;
	dataset.dataset_type = "TABLE";
	dataset.physical = physical;
	return dataset;
}

acl::AuditLineageEdge Edge(int32_t target, const string &target_field, int32_t source, const string &source_field,
                           const string &type, const string &subtype, bool masking = false) {
	acl::AuditLineageEdge edge;
	edge.target = target;
	edge.target_field = target_field;
	edge.source = source;
	edge.source_field = source_field;
	edge.type = type;
	edge.subtype = subtype;
	edge.masking = masking;
	return edge;
}

//! a write under a principal: src -> sink, a field from an element path, a whole-target filter
shared_ptr<acl::AuditLineage> Run() {
	auto lineage = make_shared_ptr<acl::AuditLineage>();
	lineage->event_type = "RUN_COMPLETE";
	lineage->run_id = "0192aaaa-bbbb-7ccc-8ddd-eeeeffff0000";
	lineage->job_ns = "acl://prod/client/flight";
	lineage->job_name = "dbt.sink_model";
	lineage->parent = {"airflow", "daily.load", "01929e3a-0000-7000-8000-000000000001"};
	lineage->root_parent = {"airflow", "daily", "01929e3a-0000-7000-8000-0000000000ff"};
	lineage->datasets.push_back(Dataset("acl://prod/sales", "src", false));
	auto sink = Dataset("acl://prod/sales", "sink", false);
	acl::AuditLineageField a;
	a.name = "a";
	acl::AuditLineageField b;
	b.name = "b";
	sink.schema = {a, b};
	lineage->datasets.push_back(sink);
	lineage->inputs = {0};
	lineage->outputs = {1};
	lineage->edges.push_back(Edge(1, "a", 0, "id", "DIRECT", "IDENTITY"));
	lineage->edges.push_back(Edge(1, "b", 0, "items[].price", "DIRECT", "TRANSFORMATION"));
	lineage->edges.push_back(Edge(1, "b", 0, "ssn", "DIRECT", "TRANSFORMATION", true));
	lineage->edges.push_back(Edge(1, "", 0, "total", "INDIRECT", "FILTER"));
	// the same source field twice, two transformations: one input field listing both
	lineage->edges.push_back(Edge(1, "a", 0, "id", "INDIRECT", "CONDITIONAL"));
	// a source taken whole (no field): not spellable per field, not sent
	lineage->edges.push_back(Edge(1, "b", 0, "", "DIRECT", "TRANSFORMATION"));
	lineage->sql = "INSERT INTO sink SELECT id, ? FROM src";
	lineage->dialect = "duckdb";
	lineage->client = "flight";
	lineage->roles = {"w"};
	lineage->node_group = "east";
	return lineage;
}

void TestRunEvent() {
	std::printf("RunEvent\n");
	LineageEvent event;
	event.lineage = Run();
	event.ts_us = 1759920000123456;
	Json json(RenderOpenLineage(event, Options()));
	Check(json.doc != nullptr, "the rendering is JSON");
	Check(json.Str("/eventType") == "COMPLETE", "RUN_COMPLETE is a COMPLETE event");
	Check(json.Str("/eventTime") == "2025-10-08T10:40:00.123Z", "eventTime is ISO-8601 UTC, milliseconds");
	Check(json.Str("/schemaURL").find("#/$defs/RunEvent") != string::npos, "the RunEvent schema");
	Check(json.Str("/run/runId") == "0192aaaa-bbbb-7ccc-8ddd-eeeeffff0000", "the run id");
	Check(json.Str("/run/facets/parent/run/runId") == "01929e3a-0000-7000-8000-000000000001", "the parent run");
	Check(json.Str("/run/facets/parent/job/name") == "daily.load", "the parent job");
	Check(json.Str("/run/facets/parent/root/run/runId") == "01929e3a-0000-7000-8000-0000000000ff", "the root run");
	Check(json.Str("/run/facets/duckdb_acl/client") == "flight", "the duckdb_acl facet's client");
	Check(json.Str("/run/facets/duckdb_acl/roles/0") == "w", "the duckdb_acl facet's roles");
	Check(json.At("/run/facets/duckdb_acl/subject") == nullptr, "an empty field is not reported");
	Check(json.Str("/run/facets/duckdb_acl/_schemaURL").find("AclRunFacet") != string::npos,
	      "the custom facet's schema");
	Check(json.Str("/job/namespace") == "acl://prod/client/flight" && json.Str("/job/name") == "dbt.sink_model",
	      "the job");
	Check(json.Str("/job/facets/jobType/integration") == "DUCKDB_ACL", "the job type");
	Check(json.Str("/job/facets/sql/query") == "INSERT INTO sink SELECT id, ? FROM src", "the SQL facet");
	Check(json.Str("/inputs/0/name") == "src" && json.Str("/outputs/0/name") == "sink", "inputs and outputs");
	Check(json.Str("/outputs/0/facets/schema/fields/1/name") == "b", "the output's schema");
	Check(json.Str("/outputs/0/facets/columnLineage/fields/a/inputFields/0/field") == "id", "a <- id");
	Check(json.Str("/outputs/0/facets/columnLineage/fields/a/inputFields/0/transformations/0/subtype") == "IDENTITY",
	      "the transformation's subtype");
	Check(json.Len("/outputs/0/facets/columnLineage/fields/a/inputFields") == 1 &&
	          json.Str("/outputs/0/facets/columnLineage/fields/a/inputFields/0/transformations/1/subtype") ==
	              "CONDITIONAL",
	      "one input field per source field, its transformations listed on it");
	Check(json.Len("/outputs/0/facets/columnLineage/fields/b/inputFields") == 2,
	      "b has two inputs - the source taken whole is not one");
	Check(json.Str("/outputs/0/facets/columnLineage/fields/b/inputFields/0/field") == "items[].price",
	      "a field path travels as written");
	Check(json.Bool("/outputs/0/facets/columnLineage/fields/b/inputFields/1/transformations/0/masking"),
	      "masking is carried");
	Check(json.Str("/outputs/0/facets/columnLineage/dataset/0/transformations/0/subtype") == "FILTER",
	      "an edge to the whole target goes to `dataset`");
	Check(json.At("/inputs/0/facets/columnLineage") == nullptr, "an input carries no columnLineage");

	event.lineage = [] {
		auto failed = Run();
		failed->event_type = "RUN_FAIL";
		failed->parent = acl::AuditLineageRunRef();
		return failed;
	}();
	Json fail(RenderOpenLineage(event, Options()));
	Check(fail.Str("/eventType") == "FAIL", "RUN_FAIL is a FAIL event");
	Check(fail.Str("/run/facets/parent/run/runId") == "01929e3a-0000-7000-8000-0000000000ff",
	      "with only a root, the root is the parent");
	Check(fail.At("/run/facets/parent/root") == nullptr, "... and no separate root");
	Check(fail.Str("/outputs/0/name") == "sink" && fail.At("/outputs/0/facets/columnLineage") == nullptr,
	      "a failed write names its output, without lineage");

	// spec 020: a write whose transaction rolled back (duckdb-acl spec 112) is an ABORT, wrote nothing
	event.lineage = [] {
		auto aborted = Run();
		aborted->event_type = "RUN_ABORT";
		return aborted;
	}();
	Json abort(RenderOpenLineage(event, Options()));
	Check(abort.Str("/eventType") == "ABORT", "RUN_ABORT is an ABORT event");
	Check(abort.Str("/outputs/0/name") == "sink" && abort.At("/outputs/0/facets/columnLineage") == nullptr,
	      "a rolled-back write names its output, without lineage");

	// a parent run id that is not a UUID: ParentRunFacet needs one - left out, and said so
	event.lineage = [] {
		auto loose = Run();
		loose->parent.run_id = "0190-run";
		loose->root_parent = acl::AuditLineageRunRef();
		return loose;
	}();
	LineageRenderNotes notes;
	Json loose(RenderOpenLineage(event, Options(), &notes));
	Check(loose.At("/run/facets/parent") == nullptr && notes.parent_dropped, "a non-UUID parent is left out, noted");
	Check(LineageIsUuid("01929e3a-0000-7000-8000-000000000001") && !LineageIsUuid("0190-run"), "the UUID check");

	// a run whose every dataset is physical, with physical off: nothing to send
	event.lineage = [] {
		auto physical = Run();
		for (auto &dataset : physical->datasets) {
			dataset.physical = true;
		}
		return physical;
	}();
	Check(RenderOpenLineage(event, Options(false)).empty(), "every dataset dropped: no event");
}

void TestPhysicalDropped() {
	std::printf("physical datasets dropped\n");
	auto lineage = make_shared_ptr<acl::AuditLineage>();
	lineage->event_type = "DATASET";
	lineage->datasets.push_back(Dataset("acl://prod/source/pg", "public.orders", true));
	auto view = Dataset("acl://prod/sales", "orders", false);
	view.dataset_type = "VIEW";
	view.lifecycle = "CREATE";
	view.tags.push_back({"acl.role.clerk", "select", ""});
	view.tags.push_back({"acl.role.clerk", "masked", "ssn"});
	lineage->datasets.push_back(view);
	lineage->inputs = {0};
	lineage->outputs = {1};
	lineage->edges.push_back(Edge(1, "id", 0, "id", "DIRECT", "IDENTITY"));
	LineageEvent event;
	event.lineage = lineage;
	event.ts_us = 1;
	Json with(RenderOpenLineage(event, Options(true)));
	Check(with.Str("/schemaURL").find("#/$defs/DatasetEvent") != string::npos, "a definition is a DatasetEvent");
	Check(with.Str("/dataset/name") == "orders", "of the defined dataset");
	Check(with.Str("/dataset/facets/lifecycleStateChange/lifecycleStateChange") == "CREATE", "its lifecycle");
	Check(with.Str("/dataset/facets/datasetType/datasetType") == "VIEW", "its type");
	Check(with.Str("/dataset/facets/tags/tags/1/field") == "ssn", "the per-role tags, per field");
	Check(with.Str("/dataset/facets/tags/tags/0/source") == "DUCKDB_ACL", "the tags' source");
	Check(with.Str("/dataset/facets/columnLineage/fields/id/inputFields/0/namespace") == "acl://prod/source/pg",
	      "the edge to the physical source");
	Json without(RenderOpenLineage(event, Options(false)));
	Check(without.At("/dataset/facets/columnLineage") == nullptr, "physical off: the edges to it are dropped");
	Check(without.Str("/dataset/name") == "orders", "... the virtual dataset stays");
}

void TestNamespace() {
	std::printf("NAMESPACE\n");
	auto lineage = make_shared_ptr<acl::AuditLineage>();
	lineage->event_type = "NAMESPACE";
	auto source = Dataset("acl://prod/source/warehouse", "", true);
	source.dataset_type = "POSTGRES";
	source.lifecycle = "CREATE";
	lineage->datasets.push_back(source);
	lineage->outputs = {0};
	LineageEvent event;
	event.lineage = lineage;
	Json json(RenderOpenLineage(event, Options()));
	Check(json.Str("/dataset/namespace") == "acl://prod/source/warehouse", "the source's namespace");
	Check(json.Str("/dataset/name") == "warehouse", "named by its alias");
	Check(json.Str("/dataset/facets/datasource/uri") == "acl://prod/source/warehouse", "a datasource facet");
	Check(json.Str("/dataset/facets/datasetType/datasetType") == "SOURCE" &&
	          json.Str("/dataset/facets/datasetType/subType") == "POSTGRES",
	      "SOURCE of the source's type");
	auto plain = make_shared_ptr<acl::AuditLineage>(*lineage);
	plain->datasets[0].ns = "prod/source/warehouse";
	event.lineage = plain;
	Json no_uri(RenderOpenLineage(event, Options()));
	Check(no_uri.At("/dataset/facets/datasource/uri") == nullptr, "a namespace that is no URI is no datasource uri");
	event.lineage = lineage;
	Check(RenderOpenLineage(event, Options(false)).empty(), "physical off: no source is sent");
}

void TestNothing() {
	std::printf("nothing to send\n");
	LineageEvent event;
	Check(RenderOpenLineage(event, Options()).empty(), "no payload");
	auto unknown = make_shared_ptr<acl::AuditLineage>();
	unknown->event_type = "SOMETHING_NEW";
	event.lineage = unknown;
	Check(RenderOpenLineage(event, Options()).empty(), "a kind this build does not know is not guessed at");
	auto quote = Run();
	quote->job_name = "a \"quoted\"\njob";
	event.lineage = quote;
	Json json(RenderOpenLineage(event, Options()));
	Check(json.Str("/job/name") == "a \"quoted\"\njob", "text is escaped");
	Check(LineageUrl("http://m:5000/", "/api/v1/lineage") == "http://m:5000/api/v1/lineage", "one slash between");
	Check(LineageUrlHasUserinfo("https://u:p@m/x") && !LineageUrlHasUserinfo("https://m/x?a=b@c"), "userinfo seen");
	Check(LineageKeyMayGoTo("https://m/x") && LineageKeyMayGoTo("http://127.0.0.1:5000/x") &&
	          LineageKeyMayGoTo("http://localhost/x") && !LineageKeyMayGoTo("http://m.corp/x"),
	      "a key over https, or plain http to loopback only");
	// invalid UTF-8 is replaced, never sent raw
	auto bad = Run();
	bad->job_name = string("job\xff");
	event.lineage = bad;
	Json replaced(RenderOpenLineage(event, Options()));
	Check(replaced.Str("/job/name") == "job\xEF\xBF\xBD", "an invalid byte becomes U+FFFD");
}

struct FakeBackend {
	vector<int> answers; // per call, the last repeats
	int calls = 0;
	vector<int64_t> slept;
	vector<std::pair<string, string>> last_headers;
	string echo; // what a 4xx/5xx body says
	LineagePoster Poster() {
		return [this](const string &, const string &, const vector<std::pair<string, string>> &headers, int64_t) {
			last_headers = headers;
			LineagePostResult result;
			result.status = answers[std::min(size_t(calls), answers.size() - 1)];
			result.error = result.status >= 400 ? (echo.empty() ? "refused\nmore" : echo) : "";
			if (result.status == 0) {
				result.error = "connection refused";
			}
			calls++;
			return result;
		};
	}
	LineageRetryPolicy Policy() {
		LineageRetryPolicy policy;
		policy.sleep_ms = [this](int64_t ms) {
			slept.push_back(ms);
			return true;
		};
		return policy;
	}
	string Bearer() const {
		for (auto &header : last_headers) {
			if (header.first == "Authorization") {
				return header.second;
			}
		}
		return string();
	}
};

LineageKeyReader Key(const string &value, const string &error = string()) {
	return [value, error](bool) {
		LineageKey k;
		k.value = value;
		k.source = "secret";
		k.error = error;
		return k;
	};
}

void TestDelivery() {
	std::printf("delivery\n");
	LineageEvent event;
	event.lineage = Run();
	const string url = "https://m/api/v1/lineage";
	{
		FakeBackend backend {{200}};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), Key("k1"), nullptr, backend.Policy());
		string error;
		Check(exporter.Export({event, event}, error), "a batch always answers true");
		Check(exporter.stats->sent == 2 && backend.calls == 2, "2xx: sent, one POST each");
		Check(backend.Bearer() == "Bearer k1", "the key as a bearer");
		Check(exporter.KeySource() == "secret", "the status names the key's source");
	}
	{
		FakeBackend backend {{503, 0, 201}};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), nullptr, nullptr, backend.Policy());
		string error;
		exporter.Export({event}, error);
		Check(exporter.stats->sent == 1 && exporter.stats->retried == 2, "5xx, no answer, then 2xx: sent on the third");
		Check(backend.slept == vector<int64_t>({500, 1000}), "backoff 0.5 s then 1 s");
	}
	{
		FakeBackend backend {{500}};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), nullptr, nullptr, backend.Policy());
		string error;
		exporter.Export({event}, error);
		Check(exporter.stats->lost_retries == 1 && backend.calls == 4 &&
		          backend.slept == vector<int64_t>({500, 1000, 2000}),
		      "the first try and 3 retries, 0.5 / 1 / 2 s apart: then lost, counted");
		Check(exporter.LastError() == "HTTP 500: refused", "the last error's first line");
	}
	{
		FakeBackend backend {{400, 200}};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), nullptr, nullptr, backend.Policy());
		string error;
		exporter.Export({event, event}, error);
		Check(exporter.stats->rejected == 1 && exporter.stats->sent == 1 && backend.calls == 2,
		      "a 4xx is dropped at once, and the next event still goes");
	}
	{
		FakeBackend backend {{429, 200}};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), nullptr, nullptr, backend.Policy());
		string error;
		exporter.Export({event}, error);
		Check(exporter.stats->sent == 1 && exporter.stats->retried == 1, "429 is retried");
	}
	{
		FakeBackend backend {{200}};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), Key("", "no secret \"k\""), nullptr,
		                             backend.Policy());
		string error;
		exporter.Export({event}, error);
		Check(exporter.stats->lost_key == 1 && backend.calls == 0, "a configured key that cannot be read: not sent");
	}
	{
		FakeBackend backend {{200}};
		OpenLineageExporter exporter("http://lineage.corp/api", 1000, Options(), backend.Poster(), Key("k1"), nullptr,
		                             backend.Policy());
		string error;
		exporter.Export({event}, error);
		Check(exporter.stats->lost_key == 1 && backend.calls == 0 && exporter.LastError().find("https") != string::npos,
		      "a key over plain http to another host: not sent");
		OpenLineageExporter loopback("http://127.0.0.1:5000/api", 1000, Options(), backend.Poster(), Key("k1"), nullptr,
		                             backend.Policy());
		loopback.Export({event}, error);
		Check(loopback.stats->sent == 1, "plain http to loopback: sent");
	}
	{
		FakeBackend backend {{200}};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), Key("k1\r\nX-Evil: 1"), nullptr,
		                             backend.Policy());
		string error;
		exporter.Export({event}, error);
		Check(exporter.stats->lost_key == 1 && backend.calls == 0, "a key with a control character: not sent");
	}
	{
		FakeBackend backend {{401, 200}};
		int reads = 0;
		LineageKeyReader rotating = [&reads](bool refresh) {
			LineageKey k;
			k.source = "secret";
			k.value = refresh ? "k-new" : "k-old";
			reads++;
			return k;
		};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), rotating, nullptr, backend.Policy());
		string error;
		exporter.Export({event}, error);
		Check(exporter.stats->sent == 1 && reads == 2 && backend.Bearer() == "Bearer k-new",
		      "a 401: the key is read again, once, and the event goes with the new one");
	}
	{
		FakeBackend backend {{400}};
		backend.echo = "bad request: Authorization: Bearer secret-k echoed";
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), Key("secret-k"), nullptr,
		                             backend.Policy());
		string error;
		exporter.Export({event}, error);
		Check(exporter.LastError().find("secret-k") == string::npos && exporter.LastError().find("***") != string::npos,
		      "a backend that echoes the key: the status never shows it");
	}
	{
		FakeBackend backend {{500}};
		LineageRetryPolicy stopping;
		OpenLineageExporter *self = nullptr;
		stopping.sleep_ms = [&self](int64_t) {
			self->Cancel(); // the lane stops while the exporter waits to retry
			return false;
		};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), nullptr, nullptr, stopping);
		self = &exporter;
		string error;
		exporter.Export({event, event}, error);
		Check(exporter.stats->lost_stopped == 2 && backend.calls == 1,
		      "a stop cuts the retries short, and the rest of the batch: counted");
		exporter.Cancel();
		exporter.Export({event}, error);
		Check(backend.calls == 1, "cancelled: nothing more is POSTed");
	}
	{
		FakeBackend backend {{200}};
		OpenLineageExporter exporter(url, 1000, Options(), backend.Poster(), nullptr, nullptr, backend.Policy());
		LineageEvent nothing;
		string error;
		exporter.Export({nothing}, error);
		Check(exporter.stats->empty == 1 && backend.calls == 0, "nothing to send: no POST");
	}
}

} // namespace

int main() {
	TestRunEvent();
	TestPhysicalDropped();
	TestNamespace();
	TestNothing();
	TestDelivery();
	std::printf("PASS test_acl_otel_lineage\n");
	return 0;
}
