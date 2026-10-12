#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

// Which services and apps mount a store (migration 0079_store_mounts, lake#666,
// docs/control-plane/storage.md#mounts). lake writes the rows when a service or an app is created
// or edited with `stores`; the Orama operator mounts each store into the workload's pods.
// Content-free desired state: ids, a path and a flag.
namespace lake::control {

// One store a workload asks to mount: what create_service, set_service_stores, create_app and
// set_app_stores take. `mount_path` is a volume's absolute path in the workload's containers; a
// bucket has none ('').
struct StoreMountRequest {
    std::string store_id;
    std::string mount_path;
    bool read_only = false;
};

// One row of store_mounts, read with the names a person reads: the store's (name, type,
// provider) and the workload's (`workload_name`, the service's or the app's name).
struct StoreMount {
    std::string store_id;
    std::string object_kind; // service | app
    std::string object_id;
    std::string mount_path;
    bool read_only = false;
    std::string created_at;
    std::string store_name, store_type, store_provider;
    std::string workload_name;
    // The workspace the store belongs to (a workload mounts only its own workspace's stores).
    std::string workspace_id;
};

// The service kinds whose pods the Orama operator builds itself (its lake_service_wrapper.go:
// lake-sqlite, lake-duckdb, lake-polars), so it can add a store's volume or connection to their
// pod template. Every other kind is run by its own upstream operator, which owns its pods.
inline constexpr std::array<std::string_view, 3> store_mounting_service_kinds{
    "sqlite", "duckdb", "polars"};

inline bool service_kind_mounts_stores(std::string_view kind) {
    return std::ranges::find(store_mounting_service_kinds, kind) !=
           store_mounting_service_kinds.end();
}

// The providers of a bucket in the customer's own account, which any number of workloads mount as
// environment variables.
inline bool cloud_bucket_provider(std::string_view provider) {
    return provider == "r2" || provider == "s3" || provider == "b2" || provider == "gcs" ||
           provider == "azure";
}

// The schema's store_mount_path_check: '/' then 1-255 of letters, digits, '.', '_', '/' and '-'.
inline bool valid_mount_path(std::string_view path) {
    if (path.size() < 2 || path.size() > 256 || path.front() != '/') {
        return false;
    }
    return std::ranges::all_of(path.substr(1), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '/' || c == '-';
    });
}

// Paths a store is never mounted at, nor beneath: the root, the directories the operator's
// wrapper services keep their own volumes at (/data, /tmp), and the kernel's.
inline bool reserved_mount_path(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') {
        path.remove_suffix(1);
    }
    if (path == "/") {
        return true;
    }
    for (std::string_view reserved : {"/data", "/tmp", "/proc", "/sys", "/dev"}) {
        if (path == reserved || (path.starts_with(reserved) && path[reserved.size()] == '/')) {
            return true;
        }
    }
    return false;
}

// Why a mounted store is not deleted: "the store uploads is mounted by the service crm and the app
// site; remove the mounts first" (delete_store's conflict, lake#666).
inline std::string mounted_by_message(const std::vector<StoreMount>& mounts) {
    std::string named;
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        if (i != 0) {
            named += i + 1 == mounts.size() ? " and " : ", ";
        }
        named += "the " + mounts[i].object_kind + " " +
                 (mounts[i].workload_name.empty() ? mounts[i].object_id : mounts[i].workload_name);
    }
    const std::string store = mounts.empty() ? std::string("it") : mounts.front().store_name;
    return "the store " + store + " is mounted by " + named + "; remove " +
           (mounts.size() == 1 ? "the mount" : "the mounts") + " first";
}

} // namespace lake::control
