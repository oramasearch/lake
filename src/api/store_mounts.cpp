#include "store_mounts.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace lake::api {

using nlohmann::json;

json store_mounts_schema() {
    return {{"type", "array"},
            {"description",
             "The storage the workload mounts: a volume at an absolute `mount_path` (one workload "
             "per volume), a bucket in your own account as environment variables (no path). "
             "Replaces what it mounted before; [] mounts nothing."},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"store_id", {{"type", "string"}}},
                {"mount_path", {{"type", "string"}}},
                {"read_only", {{"type", "boolean"}}}}},
              {"required", json::array({"store_id"})},
              {"additionalProperties", false}}}};
}

namespace {

std::string label_of(std::string_view kind) {
    const control::ServiceKind* found = control::find_service_kind(kind);
    return found == nullptr ? std::string(kind) : std::string(found->label);
}

// The operator's environment variable prefix for a bucket: STORE_<NAME>_, `-` as `_`.
std::string env_prefix(std::string_view name) {
    std::string prefix = "STORE_";
    for (const char c : name) {
        prefix += c == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return prefix + "_";
}

} // namespace

std::expected<std::vector<control::StoreMountRequest>, ApiResult>
checked_store_mounts(const json& stores,
                     const std::string& customer_id,
                     const std::string& workspace_id,
                     std::string_view service_kind,
                     bool app,
                     control::Store& store) {
    if (!stores.is_array()) {
        return std::unexpected(
            ApiResult::failure("invalid_input", "stores must be an array", "/stores"));
    }
    std::vector<control::StoreMountRequest> mounts;
    if (stores.empty()) {
        return mounts;
    }
    if (!app && !control::service_kind_mounts_stores(service_kind)) {
        return std::unexpected(ApiResult::failure(
            "invalid_input",
            "a " + label_of(service_kind) +
                " service's pods are run by its own operator, so Lake cannot mount storage into "
                "them; SQLite, DuckDB and Polars services and apps mount storage",
            "/stores"));
    }
    std::set<std::string> seen;
    for (std::size_t i = 0; i < stores.size(); ++i) {
        const json& entry = stores[i];
        const std::string at = "/stores/" + std::to_string(i);
        if (!entry.is_object() || !entry.contains("store_id") ||
            !entry.at("store_id").is_string()) {
            return std::unexpected(ApiResult::failure(
                "invalid_input", "each entry names a store_id", at + "/store_id"));
        }
        control::StoreMountRequest mount;
        mount.store_id = entry.at("store_id").get<std::string>();
        if (entry.contains("mount_path") && !entry.at("mount_path").is_null()) {
            if (!entry.at("mount_path").is_string()) {
                return std::unexpected(ApiResult::failure(
                    "invalid_input", "mount_path must be a string", at + "/mount_path"));
            }
            mount.mount_path = entry.at("mount_path").get<std::string>();
        }
        if (entry.contains("read_only")) {
            if (!entry.at("read_only").is_boolean()) {
                return std::unexpected(ApiResult::failure(
                    "invalid_input", "read_only must be true or false", at + "/read_only"));
            }
            mount.read_only = entry.at("read_only").get<bool>();
        }
        if (!seen.insert(mount.store_id).second) {
            return std::unexpected(
                ApiResult::failure("invalid_input", "each store is named once", at + "/store_id"));
        }
        const auto found = store.get_store(mount.store_id);
        if (!found || found->customer_id != customer_id || found->workspace_id != workspace_id) {
            if (found || found.error().code == "not_found" ||
                found.error().code == "invalid_input") {
                return std::unexpected(ApiResult::failure(
                    "not_found", "no such storage in this workspace", at + "/store_id"));
            }
            return std::unexpected(ApiResult::failure(found.error().code, found.error().message));
        }
        if (found->desired != "present") {
            return std::unexpected(
                ApiResult::failure("invalid_input",
                                   "the store " + found->name + " is being deleted",
                                   at + "/store_id"));
        }
        if (found->provider == "zetta" || found->provider == "lake") {
            return std::unexpected(ApiResult::failure(
                "invalid_input",
                "Lake keeps " + found->name +
                    " in the workspace and serves it itself; it is opened as a drive or read "
                    "through Lake, not mounted",
                at + "/store_id"));
        }
        if (found->type == "volume") {
            if (mount.mount_path.empty()) {
                return std::unexpected(ApiResult::failure(
                    "invalid_input",
                    "a volume is mounted at a path: give mount_path, for example /data",
                    at + "/mount_path"));
            }
            if (!control::valid_mount_path(mount.mount_path) ||
                mount.mount_path.find("/../") != std::string::npos ||
                mount.mount_path.ends_with("/..") ||
                mount.mount_path.find("//") != std::string::npos) {
                return std::unexpected(ApiResult::failure(
                    "invalid_input",
                    "mount_path must be an absolute path of letters, digits, '.', '_', '-' and "
                    "'/', at most 256 characters, without '..' or '//'",
                    at + "/mount_path"));
            }
        } else if (found->type == "object" && control::cloud_bucket_provider(found->provider)) {
            if (!mount.mount_path.empty()) {
                return std::unexpected(ApiResult::failure(
                    "invalid_input",
                    "a bucket is mounted as environment variables (" + env_prefix(found->name) +
                        "BUCKET and the rest) and takes no mount_path",
                    at + "/mount_path"));
            }
        } else {
            return std::unexpected(ApiResult::failure("invalid_input",
                                                      "a " + found->provider + " " + found->type +
                                                          " store cannot be mounted yet",
                                                      at + "/store_id"));
        }
        mounts.push_back(std::move(mount));
    }
    return mounts;
}

json workload_stores_json(const std::vector<control::StoreMount>& mounts,
                          std::string_view object_kind,
                          std::string_view object_id) {
    json value = json::array();
    for (const control::StoreMount& mount : mounts) {
        if (mount.object_kind != object_kind || mount.object_id != object_id) {
            continue;
        }
        value.push_back({{"store_id", mount.store_id},
                         {"name", mount.store_name},
                         {"type", mount.store_type},
                         {"provider", mount.store_provider},
                         {"mount_path", mount.mount_path},
                         {"read_only", mount.read_only}});
    }
    return value;
}

} // namespace lake::api
