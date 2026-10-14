#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! acl_otel's whole registration (settings, functions, the sink on the base's hooks) - what its own entry calls,
//! and what an extension that builds acl_otel in calls (spec 024: hugr-node's hugr_node). Under ACL_OTEL_EMBEDDED
//! acl_otel's entry point and AclOtelExtension are not compiled.
void LoadAclOtelCore(ExtensionLoader &loader);

#ifndef ACL_OTEL_EMBEDDED
class AclOtelExtension : public Extension {
public:
	void Load(ExtensionLoader &loader) override;
	std::string Name() override;
	std::string Version() const override;
};
#endif

} // namespace duckdb
