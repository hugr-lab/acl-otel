// spec 018: the OpenLineage transport - rendering, delivery, the key. See acl_otel_lineage.hpp.
//
// The rendering follows OpenLineage 2-0-2 and its facets one to one, because the producer
// (duckdb-acl spec 107, ext-common spec 014) already shaped the payload after them: a run's datasets
// with `schema`, `tags`, `lifecycleStateChange`, the outputs' `columnLineage`; a definition as a
// DatasetEvent. The one facet of our own is `acl` on the run: who did it, at the level the node's
// operator chose, and whether the edges are exact.

#include "acl_otel_lineage.hpp"

#include <chrono>
#include <ctime>
#include <map>
#include <thread>

namespace duckdb {
namespace acl_otel {

namespace {

const char *const SPEC = "https://openlineage.io/spec/2-0-2/OpenLineage.json";
const char *const FACETS = "https://openlineage.io/spec/facets/";
const char *const ACL_FACET_SCHEMA = "https://hugr-lab.github.io/acl-otel/schemas/AclRunFacet.json";

//! the length of the valid UTF-8 sequence at `i`, 0 when it is not one
idx_t Utf8Length(const string &text, idx_t i) {
	auto c = (unsigned char)text[i];
	idx_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
	if (length == 0 || i + length > text.size()) {
		return 0;
	}
	for (idx_t j = 1; j < length; j++) {
		if ((((unsigned char)text[i + j]) >> 6) != 0x2) {
			return 0;
		}
	}
	return length;
}

//! a JSON string: escaped, and an invalid UTF-8 byte replaced (U+FFFD) - a strict backend refuses
//! the whole event over one bad byte in a name
string Quote(const string &value) {
	string out = "\"";
	for (idx_t i = 0; i < value.size();) {
		auto length = Utf8Length(value, i);
		if (length == 0) {
			out += "\xEF\xBF\xBD";
			i++;
			continue;
		}
		if (length > 1) {
			out += value.substr(i, length);
			i += length;
			continue;
		}
		unsigned char c = (unsigned char)value[i++];
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
			if (c < 0x20) {
				char buffer[8];
				snprintf(buffer, sizeof(buffer), "\\u%04x", c);
				out += buffer;
			} else {
				out += char(c);
			}
		}
	}
	return out + "\"";
}

//! `"_producer": …, "_schemaURL": …` - the two keys every facet carries
string FacetHead(const LineageRenderOptions &options, const string &schema_url) {
	return "\"_producer\":" + Quote(options.producer) + ",\"_schemaURL\":" + Quote(schema_url);
}

string FacetUrl(const string &version, const string &name) {
	return string(FACETS) + version + "/" + name + ".json#/$defs/" + name;
}

string Fields(const vector<acl::AuditLineageField> &fields) {
	string out = "[";
	for (idx_t i = 0; i < fields.size(); i++) {
		auto &field = fields[i];
		out += string(i ? "," : "") + "{\"name\":" + Quote(field.name);
		if (!field.type.empty()) {
			out += ",\"type\":" + Quote(field.type);
		}
		if (!field.fields.empty()) {
			out += ",\"fields\":" + Fields(field.fields);
		}
		out += "}";
	}
	return out + "]";
}

//! OpenLineage's lifecycle states; a producer's value outside them is not sent
bool KnownLifecycle(const string &state) {
	for (auto known : {"ALTER", "CREATE", "DROP", "OVERWRITE", "RENAME", "TRUNCATE"}) {
		if (state == known) {
			return true;
		}
	}
	return false;
}

struct Rendering {
	const acl::AuditLineage &lineage;
	const LineageRenderOptions &options;
	vector<bool> kept; // per dataset: sent (a physical one may be dropped)

	Rendering(const acl::AuditLineage &lineage_p, const LineageRenderOptions &options_p)
	    : lineage(lineage_p), options(options_p) {
		for (auto &dataset : lineage.datasets) {
			kept.push_back(options.physical || !dataset.physical);
		}
	}

	bool Kept(int32_t index) const {
		return index >= 0 && idx_t(index) < kept.size() && kept[idx_t(index)];
	}

	//! the transformation of one edge, as ColumnLineageDatasetFacet 1-2-0 spells it
	string Transformation(const acl::AuditLineageEdge &edge) const {
		string out = "{\"type\":" + Quote(edge.type);
		if (!edge.subtype.empty()) {
			out += ",\"subtype\":" + Quote(edge.subtype);
		}
		if (edge.masking) {
			out += ",\"masking\":true";
		}
		return out + "}";
	}

	//! the input fields of one target (a field, or the whole of it): one per source field, every
	//! transformation it goes through listed on it, in the order the edges named them
	struct Inputs {
		vector<string> order;
		std::map<string, vector<string>> transformations;
		std::map<string, string> heads;
		void Add(const acl::AuditLineageDataset &source, const acl::AuditLineageEdge &edge, const string &spelled) {
			auto key = source.ns + '\x1f' + source.name + '\x1f' + edge.source_field;
			if (!heads.count(key)) {
				order.push_back(key);
				heads[key] = "{\"namespace\":" + Quote(source.ns) + ",\"name\":" + Quote(source.name) +
				             ",\"field\":" + Quote(edge.source_field);
			}
			auto &list = transformations[key];
			for (auto &known : list) {
				if (known == spelled) {
					return;
				}
			}
			list.push_back(spelled);
		}
		string Render() {
			string out = "[";
			for (idx_t i = 0; i < order.size(); i++) {
				out += string(i ? "," : "") + heads[order[i]] + ",\"transformations\":[";
				auto &list = transformations[order[i]];
				for (idx_t j = 0; j < list.size(); j++) {
					out += string(j ? "," : "") + list[j];
				}
				out += "]}";
			}
			return out + "]";
		}
	};

	//! the columnLineage facet of a target: its fields' inputs, and the edges to the whole of it.
	//! Edges from a dropped (physical) source go with it. An edge without a source field (a source
	//! taken whole by an opaque function) has no field the facet could name: it is not sent - the
	//! event says `approximate` already.
	string ColumnLineage(int32_t target) const {
		std::map<string, Inputs> by_field;
		vector<string> field_order;
		Inputs whole;
		for (auto &edge : lineage.edges) {
			if (edge.target != target || !Kept(edge.source) || edge.source_field.empty()) {
				continue;
			}
			auto &source = lineage.datasets[idx_t(edge.source)];
			if (edge.target_field.empty()) {
				whole.Add(source, edge, Transformation(edge));
				continue;
			}
			if (!by_field.count(edge.target_field)) {
				field_order.push_back(edge.target_field);
			}
			by_field[edge.target_field].Add(source, edge, Transformation(edge));
		}
		if (field_order.empty() && whole.order.empty()) {
			return string();
		}
		string out = "\"columnLineage\":{" + FacetHead(options, FacetUrl("1-2-0", "ColumnLineageDatasetFacet")) +
		             ",\"fields\":{";
		for (idx_t i = 0; i < field_order.size(); i++) {
			out += string(i ? "," : "") + Quote(field_order[i]) +
			       ":{\"inputFields\":" + by_field[field_order[i]].Render() + "}";
		}
		out += "}";
		if (!whole.order.empty()) {
			out += ",\"dataset\":" + whole.Render();
		}
		return out + "}";
	}

	//! a dataset with its facets: schema, datasetType, lifecycle, symlinks, tags - and, on a
	//! target, its columnLineage
	string Dataset(int32_t index, bool target) const {
		auto &dataset = lineage.datasets[idx_t(index)];
		vector<string> facets;
		if (!dataset.schema.empty()) {
			facets.push_back("\"schema\":{" + FacetHead(options, FacetUrl("1-1-1", "SchemaDatasetFacet")) +
			                 ",\"fields\":" + Fields(dataset.schema) + "}");
		}
		if (!dataset.dataset_type.empty()) {
			facets.push_back("\"datasetType\":{" + FacetHead(options, FacetUrl("1-0-0", "DatasetTypeDatasetFacet")) +
			                 ",\"datasetType\":" + Quote(dataset.dataset_type) + "}");
		}
		if (target && KnownLifecycle(dataset.lifecycle)) {
			facets.push_back("\"lifecycleStateChange\":{" +
			                 FacetHead(options, FacetUrl("1-0-1", "LifecycleStateChangeDatasetFacet")) +
			                 ",\"lifecycleStateChange\":" + Quote(dataset.lifecycle) + "}");
		}
		if (!dataset.symlinks.empty()) {
			string identifiers;
			for (idx_t i = 0; i < dataset.symlinks.size(); i++) {
				auto &link = dataset.symlinks[i];
				identifiers += string(i ? "," : "") + "{\"namespace\":" + Quote(link.ns) +
				               ",\"name\":" + Quote(link.name) + ",\"type\":" + Quote(link.type) + "}";
			}
			facets.push_back("\"symlinks\":{" + FacetHead(options, FacetUrl("1-0-1", "SymlinksDatasetFacet")) +
			                 ",\"identifiers\":[" + identifiers + "]}");
		}
		if (!dataset.tags.empty()) {
			string tags;
			for (idx_t i = 0; i < dataset.tags.size(); i++) {
				auto &tag = dataset.tags[i];
				tags += string(i ? "," : "") + "{\"key\":" + Quote(tag.key) + ",\"value\":" + Quote(tag.value) +
				        ",\"source\":\"DUCKDB_ACL\"";
				if (!tag.field.empty()) {
					tags += ",\"field\":" + Quote(tag.field);
				}
				tags += "}";
			}
			facets.push_back("\"tags\":{" + FacetHead(options, FacetUrl("1-0-0", "TagsDatasetFacet")) + ",\"tags\":[" +
			                 tags + "]}");
		}
		if (target) {
			auto column_lineage = ColumnLineage(index);
			if (!column_lineage.empty()) {
				facets.push_back(column_lineage);
			}
		}
		string out = "{\"namespace\":" + Quote(dataset.ns) + ",\"name\":" + Quote(dataset.name);
		out += ",\"facets\":{";
		for (idx_t i = 0; i < facets.size(); i++) {
			out += string(i ? "," : "") + facets[i];
		}
		return out + "}}";
	}

	string DatasetList(const vector<int32_t> &indices, bool targets) const {
		string out = "[";
		bool first = true;
		for (auto index : indices) {
			if (!Kept(index)) {
				continue;
			}
			out += string(first ? "" : ",") + Dataset(index, targets);
			first = false;
		}
		return out + "]";
	}

	static string RunRef(const acl::AuditLineageRunRef &ref) {
		return "\"run\":{\"runId\":" + Quote(ref.run_id) + "},\"job\":{\"namespace\":" + Quote(ref.ns) +
		       ",\"name\":" + Quote(ref.job) + "}";
	}

	//! the `duckdb_acl` run facet (a custom facet carries its producer's prefix): who, as far as the node's operator
	//! let the producer say, and how exact
	string AclFacet() const {
		vector<string> fields;
		auto add = [&](const char *key, const string &value) {
			if (!value.empty()) {
				fields.push_back(string("\"") + key + "\":" + Quote(value));
			}
		};
		add("client", lineage.client);
		add("issuer", lineage.issuer);
		if (!lineage.roles.empty()) {
			string roles = "\"roles\":[";
			for (idx_t i = 0; i < lineage.roles.size(); i++) {
				roles += string(i ? "," : "") + Quote(lineage.roles[i]);
			}
			fields.push_back(roles + "]");
		}
		add("subject", lineage.subject);
		add("node_group", lineage.node_group);
		fields.push_back(string("\"approximate\":") + (lineage.approximate ? "true" : "false"));
		fields.push_back(string("\"truncated\":") + (lineage.truncated ? "true" : "false"));
		string out = "\"duckdb_acl\":{" + FacetHead(options, ACL_FACET_SCHEMA);
		for (auto &field : fields) {
			out += "," + field;
		}
		return out + "}";
	}

	string Run(const string &time, LineageRenderNotes *notes) const {
		if (!lineage.datasets.empty()) {
			bool any = false;
			for (idx_t i = 0; i < kept.size(); i++) {
				any = any || kept[i];
			}
			if (!any) {
				return string(); // every dataset it named was dropped: a run of nothing says nothing
			}
		}
		vector<string> run_facets;
		// ParentRunFacet's run ids are UUIDs: one that is not is left out (a strict backend refuses
		// the whole event over it) and said so
		auto usable = [&](const acl::AuditLineageRunRef &ref) {
			if (ref.Empty()) {
				return false;
			}
			if (!LineageIsUuid(ref.run_id)) {
				if (notes) {
					notes->parent_dropped = true;
				}
				return false;
			}
			return true;
		};
		bool has_parent = usable(lineage.parent);
		bool has_root = usable(lineage.root_parent);
		auto parent = has_parent ? lineage.parent : lineage.root_parent;
		if (has_parent || has_root) {
			string facet =
			    "\"parent\":{" + FacetHead(options, FacetUrl("1-1-0", "ParentRunFacet")) + "," + RunRef(parent);
			if (has_root && has_parent) {
				facet += ",\"root\":{" + RunRef(lineage.root_parent) + "}";
			}
			run_facets.push_back(facet + "}");
		}
		run_facets.push_back(AclFacet());
		vector<string> job_facets;
		job_facets.push_back("\"jobType\":{" + FacetHead(options, FacetUrl("2-0-3", "JobTypeJobFacet")) +
		                     ",\"processingType\":\"BATCH\",\"integration\":\"DUCKDB_ACL\",\"jobType\":\"QUERY\"}");
		if (!lineage.sql.empty()) {
			job_facets.push_back("\"sql\":{" + FacetHead(options, FacetUrl("1-1-0", "SQLJobFacet")) +
			                     ",\"query\":" + Quote(lineage.sql) +
			                     (lineage.dialect.empty() ? string() : ",\"dialect\":" + Quote(lineage.dialect)) + "}");
		}
		// spec 020: a write whose transaction rolled back is OpenLineage's ABORT (duckdb-acl spec 112)
		auto type = lineage.event_type == "RUN_FAIL"    ? "FAIL"
		            : lineage.event_type == "RUN_ABORT" ? "ABORT"
		                                                : "COMPLETE";
		bool wrote = lineage.event_type == "RUN_COMPLETE";
		string out = "{\"eventType\":" + Quote(type);
		out += ",\"eventTime\":" + Quote(time) + ",\"producer\":" + Quote(options.producer);
		out += ",\"schemaURL\":" + Quote(string(SPEC) + "#/$defs/RunEvent");
		out += ",\"run\":{\"runId\":" + Quote(lineage.run_id) + ",\"facets\":{";
		for (idx_t i = 0; i < run_facets.size(); i++) {
			out += string(i ? "," : "") + run_facets[i];
		}
		out += "}},\"job\":{\"namespace\":" + Quote(lineage.job_ns) + ",\"name\":" + Quote(lineage.job_name);
		out += ",\"facets\":{";
		for (idx_t i = 0; i < job_facets.size(); i++) {
			out += string(i ? "," : "") + job_facets[i];
		}
		out += "}},\"inputs\":" + DatasetList(lineage.inputs, false);
		// a failed or rolled-back write wrote nothing: its outputs are named, without lineage or a
		// lifecycle change
		out += ",\"outputs\":" + DatasetList(lineage.outputs, wrote) + "}";
		return out;
	}

	string DatasetEvent(const string &time, const string &dataset) const {
		return "{\"eventTime\":" + Quote(time) + ",\"producer\":" + Quote(options.producer) +
		       ",\"schemaURL\":" + Quote(string(SPEC) + "#/$defs/DatasetEvent") + ",\"dataset\":" + dataset + "}";
	}

	//! a definition: the dataset it defines (the event's output) with its edges to its sources
	string Definition(const string &time) const {
		if (lineage.outputs.empty() || !Kept(lineage.outputs[0])) {
			return string();
		}
		return DatasetEvent(time, Dataset(lineage.outputs[0], true));
	}

	//! a source attached or detached: its namespace as a dataset, the alias its name, the source's
	//! type its datasetType, a datasource facet the catalog relates to the real address
	string Namespace(const string &time) const {
		if (lineage.datasets.empty() || !options.physical) {
			return string();
		}
		auto &source = lineage.datasets[0];
		auto slash = source.ns.rfind('/');
		auto alias = slash == string::npos ? source.ns : source.ns.substr(slash + 1);
		vector<string> facets;
		// `uri` is a URI by the facet's schema: the namespace when it is one (the default acl://... is)
		facets.push_back("\"datasource\":{" + FacetHead(options, FacetUrl("1-0-1", "DatasourceDatasetFacet")) +
		                 ",\"name\":" + Quote(alias) +
		                 (source.ns.find("://") == string::npos ? string() : ",\"uri\":" + Quote(source.ns)) + "}");
		facets.push_back("\"datasetType\":{" + FacetHead(options, FacetUrl("1-0-0", "DatasetTypeDatasetFacet")) +
		                 ",\"datasetType\":\"SOURCE\"" +
		                 (source.dataset_type.empty() ? string() : ",\"subType\":" + Quote(source.dataset_type)) + "}");
		if (KnownLifecycle(source.lifecycle)) {
			facets.push_back("\"lifecycleStateChange\":{" +
			                 FacetHead(options, FacetUrl("1-0-1", "LifecycleStateChangeDatasetFacet")) +
			                 ",\"lifecycleStateChange\":" + Quote(source.lifecycle) + "}");
		}
		string dataset = "{\"namespace\":" + Quote(source.ns) + ",\"name\":" + Quote(alias) + ",\"facets\":{";
		for (idx_t i = 0; i < facets.size(); i++) {
			dataset += string(i ? "," : "") + facets[i];
		}
		return DatasetEvent(time, dataset + "}}");
	}
};

} // namespace

string LineageEventTime(int64_t ts_us) {
	auto seconds = std::time_t(ts_us / 1000000);
	auto millis = (ts_us / 1000) % 1000;
	std::tm utc {};
#ifdef _WIN32
	gmtime_s(&utc, &seconds);
#else
	gmtime_r(&seconds, &utc);
#endif
	char buffer[40];
	snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
	         utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, int(millis < 0 ? 0 : millis));
	return buffer;
}

bool LineageIsUuid(const string &text) {
	if (text.size() != 36) {
		return false;
	}
	for (idx_t i = 0; i < text.size(); i++) {
		auto c = text[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (c != '-') {
				return false;
			}
		} else if (!isxdigit((unsigned char)c)) {
			return false;
		}
	}
	return true;
}

string RenderOpenLineage(const LineageEvent &event, const LineageRenderOptions &options, LineageRenderNotes *notes) {
	if (!event.lineage) {
		return string();
	}
	auto &lineage = *event.lineage;
	Rendering rendering(lineage, options);
	auto time = LineageEventTime(event.ts_us);
	if (lineage.event_type == "RUN_COMPLETE" || lineage.event_type == "RUN_FAIL" || lineage.event_type == "RUN_ABORT") {
		if (lineage.run_id.empty() || lineage.job_name.empty()) {
			return string();
		}
		return rendering.Run(time, notes);
	}
	if (lineage.event_type == "DATASET") {
		return rendering.Definition(time);
	}
	if (lineage.event_type == "NAMESPACE") {
		return rendering.Namespace(time);
	}
	return string(); // a kind this build does not know: not guessed at
}

bool LineageUrlHasUserinfo(const string &url) {
	auto scheme = url.find("://");
	auto start = scheme == string::npos ? 0 : scheme + 3;
	auto end = url.find_first_of("/?#", start);
	auto at = url.find('@', start);
	return at != string::npos && (end == string::npos || at < end);
}

bool LineageKeyMayGoTo(const string &url) {
	auto lower = StringUtil::Lower(url);
	if (StringUtil::StartsWith(lower, "https://")) {
		return true;
	}
	if (!StringUtil::StartsWith(lower, "http://")) {
		return false;
	}
	auto host_start = string("http://").size();
	auto host_end = lower.find_first_of(":/?#", host_start);
	if (lower.size() > host_start && lower[host_start] == '[') {
		host_end = lower.find(']', host_start);
		host_end = host_end == string::npos ? host_end : host_end + 1;
	}
	auto host = lower.substr(host_start, host_end == string::npos ? string::npos : host_end - host_start);
	return host == "localhost" || host == "[::1]" || StringUtil::StartsWith(host, "127.");
}

string LineageUrl(const string &base, const string &endpoint) {
	auto left = base;
	while (!left.empty() && left.back() == '/') {
		left.pop_back();
	}
	auto right = endpoint;
	while (!right.empty() && right.front() == '/') {
		right.erase(0, 1);
	}
	return right.empty() ? left : left + "/" + right;
}

OpenLineageExporter::OpenLineageExporter(string url_p, int64_t timeout_ms_p, LineageRenderOptions options_p,
                                         LineagePoster poster_p, LineageKeyReader key_p,
                                         shared_ptr<LineageDeliveryStats> stats_p, LineageRetryPolicy policy_p)
    : stats(stats_p ? std::move(stats_p) : make_shared_ptr<LineageDeliveryStats>()), url(std::move(url_p)),
      timeout_ms(timeout_ms_p), options(std::move(options_p)), poster(std::move(poster_p)), key(std::move(key_p)),
      policy(std::move(policy_p)) {
	if (!policy.sleep_ms) {
		// the backoff waits on the cancel flag: a stopping lane is not held by a sleep
		policy.sleep_ms = [this](int64_t ms) {
			std::unique_lock<std::mutex> guard(lock);
			return !cancel_wake.wait_for(guard, std::chrono::milliseconds(ms), [this]() { return cancelled.load(); });
		};
	}
	if (policy.tries < 1) {
		policy.tries = 1;
	}
	if (policy.backoff_ms.empty()) {
		policy.backoff_ms = {500};
	}
}

void OpenLineageExporter::Cancel() {
	{
		std::lock_guard<std::mutex> guard(lock);
		cancelled = true;
	}
	cancel_wake.notify_all();
}

string OpenLineageExporter::Describe() const {
	return "openlineage " + url; // never with userinfo: refused where it is set
}

string OpenLineageExporter::LastError() {
	std::lock_guard<std::mutex> guard(lock);
	return last_error;
}

string OpenLineageExporter::KeySource() {
	std::lock_guard<std::mutex> guard(lock);
	return key_source;
}

bool OpenLineageExporter::Export(const vector<LineageEvent> &batch, string &) {
	auto &stats = *this->stats;
	for (idx_t index = 0; index < batch.size(); index++) {
		if (cancelled) {
			stats.lost_stopped += NumericCast<int64_t>(batch.size() - index);
			break;
		}
		auto &event = batch[index];
		LineageRenderNotes notes;
		auto body = RenderOpenLineage(event, options, &notes);
		if (notes.parent_dropped) {
			stats.parent_dropped++;
		}
		if (body.empty()) {
			stats.empty++;
			continue;
		}
		vector<std::pair<string, string>> headers;
		string bearer;
		auto take_key = [&](bool refresh) {
			headers = {{"Content-Type", "application/json"}};
			bearer.clear();
			if (!key) {
				return true;
			}
			auto current = key(refresh);
			string refused = current.error;
			if (refused.empty() && !current.value.empty()) {
				for (unsigned char c : current.value) {
					if (c < 0x20 || c == 0x7F) {
						refused = "the key carries a control character: not sent";
						break;
					}
				}
				if (refused.empty() && !LineageKeyMayGoTo(url)) {
					refused = "a key is sent over https only (or plain http to a loopback host): not sent";
				}
			}
			std::lock_guard<std::mutex> guard(lock);
			key_source = current.source;
			if (!refused.empty()) {
				last_error = refused;
				return false;
			}
			if (!current.value.empty()) {
				bearer = current.value;
				headers.emplace_back("Authorization", "Bearer " + bearer);
			}
			return true;
		};
		if (!take_key(false)) {
			stats.lost_key++;
			continue;
		}
		bool key_refreshed = false;
		for (int attempt = 0;; attempt++) {
			auto answer = poster(url, body, headers, timeout_ms);
			if (answer.status >= 200 && answer.status < 300) {
				stats.sent++;
				break;
			}
			bool transient = answer.status == 0 || answer.status == 429 || answer.status >= 500;
			{
				// what a backend answered, minus anything it echoed of the key
				auto first_line = answer.error.substr(0, answer.error.find('\n'));
				if (!bearer.empty()) {
					first_line = StringUtil::Replace(first_line, bearer, "***");
				}
				std::lock_guard<std::mutex> guard(lock);
				last_error = answer.status ? "HTTP " + std::to_string(answer.status) +
				                                 (first_line.empty() ? string() : ": " + first_line)
				                           : first_line;
			}
			if ((answer.status == 401 || answer.status == 403) && key && !key_refreshed) {
				// a key rotated in the service while a copy was cached: read it again, once
				key_refreshed = true;
				if (!take_key(true)) {
					stats.lost_key++;
					break;
				}
				stats.retried++;
				continue;
			}
			if (!transient) {
				stats.rejected++; // the backend refuses this event's shape: another try changes nothing
				break;
			}
			if (attempt + 1 >= policy.tries) {
				stats.lost_retries++;
				break;
			}
			auto wait = idx_t(attempt) < policy.backoff_ms.size() ? policy.backoff_ms[idx_t(attempt)]
			                                                      : policy.backoff_ms.back();
			if (cancelled || !policy.sleep_ms(wait)) {
				stats.lost_stopped++;
				break;
			}
			stats.retried++;
		}
	}
	return true;
}

} // namespace acl_otel
} // namespace duckdb
