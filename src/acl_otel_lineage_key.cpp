// spec 018: where the OpenLineage API key comes from - the environment, or a secret of the node's
// secrets service (an attached tresor catalog), read at use and cached for a TTL; and the secret type
// the key is created as. Apart from the HTTP client, so the tests compile it without the SDK.

#include "acl_otel_lineage.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace duckdb {
namespace acl_otel {

namespace {

//! The node's secrets service: an attached catalog of type tresor - `named`, or the only one.
//! Never the node's own memory / local_file storage: a key there would be written on the node.
bool SecretService(DatabaseInstance &db, const string &named, string &service, string &error) {
	vector<string> services;
	for (auto &attached : DatabaseManager::Get(db).GetDatabases()) {
		if (!attached->IsSystem() && !attached->IsTemporary() && attached->GetCatalog().GetCatalogType() == "tresor") {
			services.push_back(attached->GetName().GetIdentifierName());
		}
	}
	if (!named.empty()) {
		// the node's own storages are never a service, whatever a catalog is called
		for (auto reserved : {"memory", "local_file", "__transaction"}) {
			if (StringUtil::CIEquals(named, reserved)) {
				error = "\"" + named + "\" is the node's own secret storage, not a secrets service";
				return false;
			}
		}
		for (auto &candidate : services) {
			if (StringUtil::CIEquals(candidate, named)) {
				service = candidate;
				return true;
			}
		}
		error = "\"" + named + "\" is not a secrets service attached to this node (a catalog of type tresor)";
		return false;
	}
	if (services.size() == 1) {
		service = services[0];
		for (auto reserved : {"memory", "local_file", "__transaction"}) {
			if (StringUtil::CIEquals(service, reserved)) {
				error = "a tresor catalog attached as \"" + service + "\" shadows the node's own storage: refused";
				return false;
			}
		}
		return true;
	}
	error = services.empty() ? "no secrets service (a catalog of type tresor) is attached to this node"
	                         : "several secrets services are attached - name one: acl_otel_lineage_secret = "
	                           "'<service>.<name>'";
	return false;
}

} // namespace

LineageKeyReader MakeLineageKeyReader(const weak_ptr<DatabaseInstance> &db, const string &secret, int64_t ttl_s) {
	if (secret.empty()) {
		// the environment: OpenLineage's own convention, what an orchestrator already sets
		return [](bool) {
			LineageKey key;
			auto env = std::getenv("OPENLINEAGE_API_KEY");
			key.value = env ? string(env) : string();
			key.source = key.value.empty() ? "none" : "env";
			return key;
		};
	}
	struct Cache {
		std::mutex lock;
		string value;
		int64_t read_at_us = 0;
	};
	auto cache = std::make_shared<Cache>();
	auto ttl_us = (ttl_s > 0 ? ttl_s : 300) * 1000000;
	return [db, secret, cache, ttl_us](bool refresh) {
		LineageKey key;
		key.source = "secret";
		auto now =
		    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
		        .count();
		{
			std::lock_guard<std::mutex> guard(cache->lock);
			if (!refresh && cache->read_at_us > 0 && now - cache->read_at_us < ttl_us) {
				key.value = cache->value;
				return key;
			}
		}
		auto instance = db.lock();
		if (!instance) {
			key.error = "the database is gone";
			return key;
		}
		// this runs on the lane's worker: it must never be the one to drop the last reference - the
		// instance's teardown stops the lane, which would then wait for itself. A last reference is
		// handed to a thread of its own to release.
		struct Release {
			shared_ptr<DatabaseInstance> &held;
			~Release() {
				if (held && held.use_count() == 1) {
					std::thread([last = std::move(held)]() mutable { last.reset(); }).detach();
				}
			}
		} release {instance};
		auto dot = secret.find('.');
		auto named_service = dot == string::npos ? string() : secret.substr(0, dot);
		auto name = dot == string::npos ? secret : secret.substr(dot + 1);
		string service;
		if (!SecretService(*instance, named_service, service, key.error)) {
			return key;
		}
		try {
			// read at use, on the node's own connection: the node is the caller, as for any secret
			// it reads from its service; the value lives in memory for the cache's TTL, nowhere else
			Connection con(*instance);
			unique_ptr<SecretEntry> entry;
			con.context->RunFunctionInTransaction([&]() {
				auto &manager = SecretManager::Get(*con.context);
				entry = manager.GetSecretByName(CatalogTransaction::GetSystemCatalogTransaction(*con.context), name,
				                                service);
			});
			if (!entry || !entry->secret || !StringUtil::CIEquals(entry->storage_mode, service) ||
			    !StringUtil::CIEquals(entry->secret->GetType().GetIdentifierName(), "openlineage")) {
				key.error = "no secret \"" + name + "\" of type openlineage in " + service;
				return key;
			}
			auto &values = static_cast<const KeyValueSecret &>(*entry->secret);
			Value api_key;
			if (!values.TryGetValue("api_key", api_key) || api_key.IsNull()) {
				key.error = "the secret \"" + name + "\" carries no api_key";
				return key;
			}
			key.value = api_key.ToString();
		} catch (std::exception &ex) {
			key.error = "the secret \"" + name + "\" could not be read: " + ErrorData(ex).RawMessage();
			return key;
		}
		std::lock_guard<std::mutex> guard(cache->lock);
		cache->value = key.value;
		cache->read_at_us = now;
		return key;
	};
}

namespace {

unique_ptr<BaseSecret> CreateLineageSecret(ClientContext &, CreateSecretInput &input) {
	vector<string> scope = input.scope;
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	for (auto &option : input.options) {
		if (!option.second.IsNull()) {
			secret->secret_map[Identifier(StringUtil::Lower(option.first))] = Value(option.second.ToString());
		}
	}
	secret->redact_keys = {"api_key"};
	return std::move(secret);
}

} // namespace

void RegisterLineageSecretType(ExtensionLoader &loader) {
	SecretType type;
	type.name = "openlineage";
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	type.default_provider = "config";
	type.extension = "acl_otel";
	loader.RegisterSecretType(type);
	CreateSecretFunction function;
	function.secret_type = "openlineage";
	function.provider = "config";
	function.function = CreateLineageSecret;
	function.named_parameters["api_key"] = LogicalType::VARCHAR;
	loader.RegisterFunction(std::move(function));
}

} // namespace acl_otel
} // namespace duckdb
