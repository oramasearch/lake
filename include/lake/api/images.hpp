#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/api/registry.hpp"
#include "lake/control/oci_records.hpp"
#include "lake/control/store.hpp"

// Tenant images (lake#383 phase A, decision 54 S4-S6, docs/api/registry.md#where-a-tenant-image-
// lives). The registry splits: a tenant OCI repository (`<space>/<name>`, `<space>/cache/<name>`)
// belongs to one workspace, found through the content-free central route oci_repository_routes,
// and its manifests and tags are sealed records in that workspace's Zetta
// (control/oci_records.hpp). npm, Python, debuginfod and legacy one-component OCI repositories stay
// with the instance. Blobs stay in the instance registry until phase B (lake#380).
namespace lake::api {

// `<space>/<name>` split at its first `/`; nullopt for a one-component (instance) repository.
struct TenantRepository {
    std::string space; // the customer's name
    std::string name;  // the route's name: `<image>` or `cache/<image>`
};
std::optional<TenantRepository> tenant_repository(std::string_view repository);

// The customer whose name is `space`; nullopt when there is none.
std::optional<control::Customer> space_customer(control::Store& store, std::string_view space);

// The digests a schema version 2 manifest references (its config, then its layers); an error
// names what is malformed.
std::expected<std::vector<std::string>, std::string>
manifest_digests(const nlohmann::json& manifest);

// Routes a tenant repository the instance registry holds from before lake#383, where its owner is
// unambiguous: its route if it has one; else the one live workspace of the space whose apps or
// builds use the image's name (`cache/<image>` follows `<image>`); else the space's only live
// workspace. Answers the route, or an `outcome` that is not: `needs_selection` (an operator routes
// it with create_image_repository) or `unknown_space`.
struct ImageOwner {
    std::optional<control::OciRepositoryRoute> route;
    std::string outcome; // route_exists | image_workspace | sole_workspace | needs_selection |
                         // unknown_space | invalid_name
};
ImageOwner route_legacy_image(control::Store& store, const control::Customer& customer,
                              const std::string& name);

// The unique bytes a space's tenant images hold, each manifest and referenced blob counted once
// (docs/api/metering.md), read from the tags in each routed workspace (one list per repository).
// `unmeasured` names each workspace that could not be read.
struct ImageBytes {
    std::uint64_t bytes = 0;
    std::vector<std::string> unmeasured;
};
liquid::runtime::task<ImageBytes> tenant_image_bytes(ApiContext& context,
                                                     std::string customer_id);

// create_image_repository and list_image_repositories (docs/api/registry.md).
void register_image_capabilities(Registry& registry);

} // namespace lake::api
