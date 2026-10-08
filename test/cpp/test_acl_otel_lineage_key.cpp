// spec 018: the OpenLineage API key from the node's secrets service - against a catalog of type
// `tresor` (the shape tresor attaches: an in-memory catalog and a persistent secret storage of the
// catalog's name; the fake of duckdb-acl's test_acl_identity_secrets.cpp). Read at use, rotated in
// the service without a restart (after the TTL), never from the node's own memory secrets, refused
// when no service or several are attached and none is named; the environment otherwise.

#include "acl_otel_lineage.hpp"
#include "acl_otel_test_util.hpp"

#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret_storage.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

#include <atomic>
#include <cstdlib>
#include <unistd.h>

using namespace duckdb;
using acl_otel_test::Check;

namespace {

class FakeServiceStorage : public CatalogSetSecretStorage {
public:
	FakeServiceStorage(DatabaseInstance &db, const string &name, int64_t offset)
	    : CatalogSetSecretStorage(db, name, offset) {
		secrets = make_uniq<CatalogSet>(Catalog::GetSystemCatalog(db));
		persistent = true;
	}
};

class FakeServiceCatalog : public DuckCatalog {
public:
	explicit FakeServiceCatalog(AttachedDatabase &db) : DuckCatalog(db) {
	}
	string GetCatalogType() override {
		return "tresor";
	}
	void Initialize(bool) override {
		DuckCatalog::Initialize(false);
		GetAttached().SetReadOnlyDatabase();
	}
};

unique_ptr<Catalog> FakeAttach(optional_ptr<StorageExtensionInfo>, ClientContext &context, AttachedDatabase &db,
                               const string &name, AttachInfo &info, AttachOptions &) {
	static std::atomic<int64_t> next_offset {30};
	SecretManager::Get(context).LoadSecretStorage(
	    make_uniq<FakeServiceStorage>(db.GetDatabase(), name, next_offset.fetch_add(1)));
	info.path = IN_MEMORY_PATH;
	return make_uniq<FakeServiceCatalog>(db);
}

unique_ptr<TransactionManager> FakeTransactions(optional_ptr<StorageExtensionInfo>, AttachedDatabase &db, Catalog &) {
	return make_uniq<DuckTransactionManager>(db);
}

void Exec(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	Check(!result->HasError(), sql + (result->HasError() ? " -> " + result->GetError() : string()));
}

} // namespace

int main() {
	std::printf("the OpenLineage key from the secrets service (spec 018)\n");
	DBConfig config;
	char directory[] = "/tmp/acl-otel-lineage-key-XXXXXX";
	Check(mkdtemp(directory) != nullptr, "a secret directory of the test's own");
	config.SetOptionByName("secret_directory", Value(string(directory)));
	DuckDB db(nullptr, &config);
	auto storage = make_shared_ptr<StorageExtension>();
	storage->attach = FakeAttach;
	storage->create_transaction_manager = FakeTransactions;
	StorageExtension::Register(DBConfig::GetConfig(*db.instance), "tresor", std::move(storage));
	{
		ExtensionLoader loader(*db.instance, "acl_otel");
		acl_otel::RegisterLineageSecretType(loader);
	}
	Connection con(db);
	weak_ptr<DatabaseInstance> weak = db.instance;

	// the environment, when no secret is named
	setenv("OPENLINEAGE_API_KEY", "env-key", 1);
	auto from_env = acl_otel::MakeLineageKeyReader(weak, "", 300)(false);
	Check(from_env.value == "env-key" && from_env.source == "env", "no secret named: OPENLINEAGE_API_KEY");
	unsetenv("OPENLINEAGE_API_KEY");
	auto none = acl_otel::MakeLineageKeyReader(weak, "", 300)(false);
	Check(none.value.empty() && none.source == "none" && none.error.empty(), "neither: no key, no error");

	// a secret named, no service attached: refused, whatever the node's own memory holds
	Exec(con, "CREATE SECRET ol (TYPE openlineage, API_KEY 'memory-key')");
	auto no_service = acl_otel::MakeLineageKeyReader(weak, "ol", 300)(false);
	Check(!no_service.error.empty() && no_service.value.empty() &&
	          no_service.error.find("no secrets service") != string::npos,
	      "no service: an error, and the node's memory secret is never read (" + no_service.error + ")");

	Exec(con, "ATTACH '' AS corp (TYPE tresor)");
	auto missing = acl_otel::MakeLineageKeyReader(weak, "ol_corp", 300)(false);
	Check(missing.error.find("no secret \"ol_corp\"") != string::npos, "a missing secret: " + missing.error);

	Exec(con, "CREATE PERSISTENT SECRET ol_corp IN corp (TYPE openlineage, API_KEY 'k-1')");
	auto reader = acl_otel::MakeLineageKeyReader(weak, "ol_corp", 300);
	auto first = reader(false);
	Check(first.value == "k-1" && first.source == "secret" && first.error.empty(), "read from the service");
	// rotated in the service: the cached key serves until the TTL, a reader with no TTL left reads anew
	Exec(con, "CREATE OR REPLACE PERSISTENT SECRET ol_corp IN corp (TYPE openlineage, API_KEY 'k-2')");
	Check(reader(false).value == "k-1", "within the TTL: the cached key");
	Check(reader(true).value == "k-2", "refused by the backend (401): read again at once, the rotated key");
	auto fresh = acl_otel::MakeLineageKeyReader(weak, "corp.ol_corp", 300)(false);
	Check(fresh.value == "k-2", "rotated in the service, no restart: the new key (service named)");

	// of another type: refused
	Exec(con, "CREATE PERSISTENT SECRET wrong IN corp (TYPE http, BEARER_TOKEN 'x')");
	auto wrong = acl_otel::MakeLineageKeyReader(weak, "wrong", 300)(false);
	Check(!wrong.error.empty() && wrong.value.empty(), "a secret of another type: " + wrong.error);

	// two services: name one
	Exec(con, "ATTACH '' AS vault (TYPE tresor)");
	auto ambiguous = acl_otel::MakeLineageKeyReader(weak, "ol_corp", 300)(false);
	Check(ambiguous.error.find("several secrets services") != string::npos, "two services: " + ambiguous.error);
	auto named = acl_otel::MakeLineageKeyReader(weak, "corp.ol_corp", 300)(false);
	Check(named.value == "k-2", "two services, one named: read from it");
	auto not_service = acl_otel::MakeLineageKeyReader(weak, "memory.ol", 300)(false);
	Check(not_service.error.find("the node's own secret storage") != string::npos,
	      "the node's own storage named: refused - " + not_service.error);
	auto unknown = acl_otel::MakeLineageKeyReader(weak, "nowhere.ol", 300)(false);
	Check(unknown.error.find("is not a secrets service") != string::npos,
	      "a catalog that is not a service: " + unknown.error);

	// the redaction: the key never shows in the node's listing
	auto listed = con.Query("SELECT secret_string FROM duckdb_secrets() WHERE name = 'ol_corp'");
	Check(!listed->HasError() && listed->RowCount() == 1 &&
	          listed->Collection().GetValue(0, 0).ToString().find("k-2") == string::npos,
	      "api_key is redacted in duckdb_secrets()");
	std::printf("PASS test_acl_otel_lineage_key\n");
	return 0;
}
