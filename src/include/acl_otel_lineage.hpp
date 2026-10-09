//===----------------------------------------------------------------------===//
// acl_otel_lineage.hpp - spec 018: the OpenLineage transport
//
// duckdb-acl (spec 107) hands lineage facts to the sinks that ask for them (acl_audit v3,
// `AuditEvent::lineage`). This is the lane that carries them out: the rendering of one fact as an
// OpenLineage 2-0-2 event (a RunEvent, or a DatasetEvent for a definition or a source), the HTTP
// delivery with its retry policy, and where the API key comes from. Nothing here runs on a query's
// path: the sink copies a pointer, a worker renders and POSTs.
//===----------------------------------------------------------------------===//

#pragma once

#include "acl_otel.hpp"

#include <functional>

namespace duckdb {

class ExtensionLoader;

namespace acl_otel {

struct LineageRenderOptions {
	string producer;      // the `producer` / `_producer` URI: acl-otel at its version
	bool physical = true; // acl_otel_lineage_physical: false drops physical datasets and their edges
};

//! The OpenLineage JSON of one fact: a RunEvent for RUN_COMPLETE / RUN_FAIL / RUN_ABORT, a DatasetEvent for
//! DATASET (the defined dataset) and for NAMESPACE (a source, with a `datasource` facet). Empty when
//! there is nothing to send (no payload, an unknown kind, every dataset dropped).
//! `notes`, when given, says what the rendering left out.
struct LineageRenderNotes {
	bool parent_dropped = false; // a parent / root run id that is not a UUID (ParentRunFacet needs one)
};
string RenderOpenLineage(const LineageEvent &event, const LineageRenderOptions &options,
                         LineageRenderNotes *notes = nullptr);
//! 8-4-4-4-12 hex: what OpenLineage's run ids are
bool LineageIsUuid(const string &text);

//! The ISO-8601 UTC time of an epoch-microsecond timestamp, millisecond precision.
string LineageEventTime(int64_t ts_us);

//! What one POST answered: `status` the HTTP code, or 0 when there was no answer (a connection
//! error, a timeout - `error` says which).
struct LineagePostResult {
	int status = 0;
	string error;
};
//! The HTTP seam: the SDK's curl client in the extension, a fake in the tests.
using LineagePoster = std::function<LineagePostResult(
    const string &url, const string &body, const vector<std::pair<string, string>> &headers, int64_t timeout_ms)>;
//! The API key, read at use: '' = send none. `source` names where it came from (env / secret /
//! none) - never the value; `error` set = a key was configured but could not be read (the event is
//! then not sent: a backend that needs the key would refuse it anyway).
struct LineageKey {
	string value;
	string source;
	string error;
};
//! `refresh` = a cached key was just refused (401 / 403): read the source again now.
using LineageKeyReader = std::function<LineageKey(bool refresh)>;

//! Whether a URL carries userinfo (`user:pass@`): refused - a credential would sit in a setting.
bool LineageUrlHasUserinfo(const string &url);
//! Whether a bearer may go to this URL: https, or plain http to a loopback host only.
bool LineageKeyMayGoTo(const string &url);

//! The numbers the lane's transport keeps (spec 018): the queue's own stats count what it handed
//! here; these say what became of each event.
struct LineageDeliveryStats {
	std::atomic<int64_t> sent {0};
	std::atomic<int64_t> rejected {0};       // a 4xx: dropped at once, the backend refused its shape
	std::atomic<int64_t> retried {0};        // attempts after the first
	std::atomic<int64_t> lost_retries {0};   // a 5xx / 429 / no answer, past the last retry
	std::atomic<int64_t> lost_key {0};       // a configured key that could not be read
	std::atomic<int64_t> lost_stopped {0};   // still to send when the lane stopped
	std::atomic<int64_t> empty {0};          // nothing to send (every dataset dropped)
	std::atomic<int64_t> parent_dropped {0}; // a parent run id that is not a UUID, sent without it
};

//! The retry policy: the first try and 3 retries, 0.5 / 1 / 2 s apart - a sleep the tests replace
//! (it answers false when the wait was cut short: the lane is stopping).
struct LineageRetryPolicy {
	int tries = 4;
	vector<int64_t> backoff_ms = {500, 1000, 2000};
	std::function<bool(int64_t)> sleep_ms;
};

//! POSTs each event of a batch to `<base>/<endpoint>`, by the policy above. Export always answers
//! true: what became of each event is in `stats` (the lane must not count a batch of mixed outcomes
//! as one; shared, so a rebuilt exporter keeps counting where the last stopped), and `LastError()`
//! keeps the last refusal's first line. `Cancel()` ends a batch between attempts - what is left is
//! `lost_stopped` - so stopping the lane never waits out a black-holed backend's retries.
class OpenLineageExporter : public LineageExporter {
public:
	OpenLineageExporter(string url, int64_t timeout_ms, LineageRenderOptions options, LineagePoster poster,
	                    LineageKeyReader key, shared_ptr<LineageDeliveryStats> stats,
	                    LineageRetryPolicy policy = LineageRetryPolicy());
	bool Export(const vector<LineageEvent> &batch, string &error) override;
	string Describe() const override;
	string LastError();
	string KeySource();
	void Cancel();
	shared_ptr<LineageDeliveryStats> stats;

private:
	std::atomic<bool> cancelled {false};
	std::condition_variable cancel_wake;
	string url;
	int64_t timeout_ms;
	LineageRenderOptions options;
	LineagePoster poster;
	LineageKeyReader key;
	LineageRetryPolicy policy;
	std::mutex lock;
	string last_error; // under `lock`
	string key_source; // under `lock`
};

//! `<base>` + `/` + `<endpoint>`, one slash between them whatever either side carries.
string LineageUrl(const string &base, const string &endpoint);

//! The SDK's HTTP client (curl), synchronous, TLS by the URL's scheme; `certificate` a CA file or ''.
LineagePoster MakeSdkLineagePoster(const string &certificate);

//! The key from `OPENLINEAGE_API_KEY`, or - `secret` set (`[service.]name`) - from a secret of type
//! `openlineage` (field `api_key`) in the node's secrets service (an attached `tresor` catalog,
//! never the node's own memory / local_file storage), cached `ttl_s` seconds. The instance is held
//! weakly: a read after it is gone answers an error.
LineageKeyReader MakeLineageKeyReader(const weak_ptr<DatabaseInstance> &db, const string &secret, int64_t ttl_s);

//! Register the `openlineage` secret type (field `api_key`, redacted) - what a tresor secret of the
//! key is created as.
void RegisterLineageSecretType(ExtensionLoader &loader);

} // namespace acl_otel
} // namespace duckdb
