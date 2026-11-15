#pragma once

#include <map>
#include <set>

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

// Tenant images (lake#383 phase A, docs/api/registry.md#where-a-tenant-image-
// lives). The registry splits: a tenant OCI repository (`<space>/<name>`, `<space>/cache/<name>`)
// belongs to one workspace, found through the content-free central route oci_repository_routes,
// and its manifests and tags are sealed records in that workspace's Zetta
// (control/oci_records.hpp). npm, Python, debuginfod and legacy one-component OCI repositories stay
// with the instance. Since phase B a tenant blob is an object of its workspace's liquid store
// (lake#380), found through its lake_oci_blobs record.
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

// Whether `image` is pinned by digest, `name@sha256:<64 hex digits>`: the only reference the
// operator runs an image app with (the LakeApp CRD's pattern and orama-operator's
// lakeAppRowSource, which accept either case of hex). create_app, deploy_app and rollback_app
// refuse anything else (lake#783).
bool digest_pinned_image(std::string_view image);

// The image a build target names in `space` (`<authority>/<space>/<image>:<tag>`); empty when it
// names another space or is malformed. A build's first push may route only `<image>` and
// `cache/<image>` (lake#383).
std::string build_target_image(std::string_view target, std::string_view space);

// The owner of a tenant repository the instance registry holds from before lake#383, where it is
// unambiguous: its route if it has one; else the one live workspace of the space whose apps or
// builds use the image's name (`cache/<image>` follows `<image>`); else the space's only live
// workspace. `outcome` is route_exists | image_workspace | sole_workspace (a workspace found) or
// needs_selection | invalid_name (none). Only with `write` (the copy, never plan or verify) is a
// route written for a workspace found; `routed` says whether a route exists afterwards.
struct ImageOwner {
    std::string workspace_id;
    std::string outcome;
    bool routed = false;
};
ImageOwner legacy_image_owner(control::Store& store, const control::Customer& customer,
                              const std::string& name, bool write);

// The unique bytes a space's tenant images hold, each manifest and referenced blob counted once
// (docs/api/metering.md), read from the tags in each routed workspace (one list per repository,
// and one list of its blob records per workspace). `bytes` is what the images add to the space's
// storage: their manifests and the blobs still in the instance registry. `stored_bytes` is the
// blobs that are objects of a workspace's liquid store (lake#383 phase B), already counted in that
// store's usage and so not again. `unmeasured` names each workspace that could not be read.
struct ImageBytes {
    std::uint64_t bytes = 0;
    std::uint64_t stored_bytes = 0;
    std::vector<std::string> unmeasured;
    // `bytes` split by the workspace whose routes name the image, each digest once per workspace
    // (lake#564): a digest two workspaces' images share counts in both, so the split can add up
    // to more than `bytes`. A workspace that could not be read is absent.
    std::map<std::string, std::uint64_t> workspace_bytes;
    std::set<std::string> unmeasured_workspaces;
};
liquid::runtime::task<ImageBytes> tenant_image_bytes(ApiContext& context,
                                                     std::string customer_id);

// create_image_repository, list_image_repositories and the operator's unroute_image_repository
// (docs/api/registry.md).
void register_image_capabilities(Registry& registry);

} // namespace lake::api
