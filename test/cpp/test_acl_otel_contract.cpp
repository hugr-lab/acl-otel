// The contract stamp (duckdb-acl spec 069, C2a), the round trip the base's own test binary cannot
// stage: two REAL loadables in one process. A registry created first by "an extension built from
// another acl_audit.hpp" (stamped 999 here) is refused by both sides - acl loads and audits on a
// private registry (acl_metrics() says so, the stale sink hears nothing), acl_otel refuses to attach
// and says why in its status; and on a fresh instance, in either load order, the two share one.
// Needs ACL_EXT (the base's built extension); says SKIP without it. Run via `make test-cpp`, and by
// CI's beside-acl step with ACL_EXT set.

#include "acl_audit.hpp"
#include "acl_otel_test_util.hpp"

#include "duckdb.hpp"

#include <cstdlib>
#include <mutex>

using namespace duckdb;
using namespace acl_otel_test;

namespace {

struct CountingSink : acl::AuditSink {
	std::mutex lock;
	int64_t seen = 0;
	void OnEvent(const acl::AuditEvent &) override {
		std::lock_guard<std::mutex> guard(lock);
		seen++;
	}
	int64_t Seen() {
		std::lock_guard<std::mutex> guard(lock);
		return seen;
	}
};

string Scalar(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		return "ERROR: " + result->GetError();
	}
	return result->RowCount() ? result->GetValue(0, 0).ToString() : string("<no rows>");
}

void Exec(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	Check(!result->HasError(), sql + (result->HasError() ? ": " + result->GetError() : ""));
}

} // namespace

int main(int argc, char *argv[]) {
	std::printf("test_acl_otel_contract\n");
	const char *acl_ext = std::getenv("ACL_EXT");
	if (!acl_ext || !*acl_ext) {
		std::printf(
		    "SKIP: ACL_EXT not set (the base's built extension) - the contract round trip needs two loadables\n");
		return 0;
	}
	string otel_ext = argc > 1 ? argv[1] : "build/release/extension/acl_otel/acl_otel.duckdb_extension";
	string load_acl = "LOAD '" + string(acl_ext) + "'";
	string load_otel = "LOAD '" + otel_ext + "'";
	{
		// --- a registry stamped by another header revision, before either extension loads --------
		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB db(nullptr, &config);
		auto &cache = db.instance->GetObjectCache();
		auto stale = cache.GetOrCreate<acl::AuditHooks>(acl::AuditHooks::ObjectType());
		stale->contract_version = 999;
		auto stale_sink = make_shared_ptr<CountingSink>();
		stale->AddSink(stale_sink);
		Connection con(db);
		Exec(con, load_acl);
		Exec(con, "SET GLOBAL acl_audit_level = 'denied'");
		auto contract = Scalar(con, "SELECT attributes FROM acl_metrics() WHERE name = 'acl.audit.contract'");
		Check(contract.find("\"registry\":\"private\"") != string::npos &&
		          contract.find("\"found\":\"999\"") != string::npos,
		      "acl audits on a private registry and names the stamp it found: " + contract);
		auto refused = con.Query("ACL ROLE nobody SELECT 1");
		Check(refused->HasError(), "a statement under an unknown role is refused as ever");
		Exec(con, "SELECT acl_audit_flush()");
		auto recorded = Scalar(con, "SELECT count(*) FROM acl_audit_events() WHERE kind = 'policy' AND detail = "
		                            "'contract_mismatch' AND reason_code = 'policy_error'");
		Check(recorded == "1", "the mismatch is one policy_error refusal in the base's own audit: " + recorded);
		auto statements = Scalar(con, "SELECT count(*) FROM acl_audit_events() WHERE kind = 'statement'");
		Check(statements == "1", "...and the refusal above is recorded there too: " + statements);
		Check(stale_sink->Seen() == 0, "the stale registry's sink heard nothing - never called");
		// acl_otel loads beside: the same refusal, said in its status
		Exec(con, load_otel);
		auto status = Scalar(con, "SELECT acl_otel_status()");
		Check(status.find("\"attached\":false") != string::npos && status.find("999") != string::npos &&
		          status.find("\"attach_error\":\"") != string::npos,
		      "acl_otel did not attach and says why: " + status);
		Check(Scalar(con, "SELECT acl_otel_start()") == "false", "acl_otel_start() answers false while it stands");
		Check(stale_sink->Seen() == 0, "still nothing on the stale sink");
	}
	{
		// --- a fresh instance: acl_otel first, then acl - one shared registry ---------------------
		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB db(nullptr, &config);
		Connection con(db);
		Exec(con, load_otel);
		Exec(con, load_acl);
		auto status = Scalar(con, "SELECT acl_otel_status()");
		Check(status.find("\"attached\":true") != string::npos && status.find("\"attach_error\":null") != string::npos,
		      "acl_otel attached, no error: " + status);
		auto contract = Scalar(con, "SELECT attributes FROM acl_metrics() WHERE name = 'acl.audit.contract'");
		Check(contract == "{\"registry\":\"shared\"}", "acl shares the registry acl_otel created: " + contract);
		string why;
		auto hooks = acl::AuditHooks::Reach(db.instance->GetObjectCache(), why);
		Check(hooks && hooks->contract_version == acl::AuditHooks::CONTRACT_VERSION,
		      "the registry is stamped with the version both were built from");
	}
	std::printf("PASS\n");
	return 0;
}
