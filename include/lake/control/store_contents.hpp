#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::control {

// A store's contents as a tree of slash paths (docs/api/drives.md#store-drives, lake#656): what a
// drive with `storage: store` browses. The bytes stay in the store's own provider; the drive's
// record (in the workspace Zetta, like every drive's) only names the store. Paths are relative to
// the store's top, with no leading or trailing slash; "" is the top. Each name in a path is a
// valid drive name (valid_drive_name).
struct StoreItem {
    std::string path;
    bool folder = false;
    std::uint64_t size = 0;
    std::string content_type; // a file's, when the provider keeps one
    std::string modified_at;  // RFC 3339, when the provider says
    std::string etag;         // the provider's version of a file's bytes, when it has one
};

// One store's contents (the interface every browsable provider implements). Errors use the API's
// codes: `not_found` (no such path), `conflict` (a path taken), `invalid_input`, `size_invalid`,
// `store_unavailable` (the provider did not answer).
class StoreContents {
public:
    virtual ~StoreContents() = default;
    // The items directly in a folder ("" for the top), folders first, then by name.
    virtual liquid::runtime::task<ZettaControlResult<std::vector<StoreItem>>>
    list(std::string folder) = 0;
    // One item, or nullopt when nothing is at `path`.
    virtual liquid::runtime::task<ZettaControlResult<std::optional<StoreItem>>>
    stat(std::string path) = 0;
    // [offset, offset + length) of a file.
    virtual liquid::runtime::task<ZettaControlResult<std::string>>
    read(std::string path, std::uint64_t offset, std::uint64_t length) = 0;
    // Writes a whole file at `path`, replacing one there, its folders made where missing.
    virtual liquid::runtime::task<ZettaControlResult<StoreItem>>
    write(std::string path, std::string bytes, std::string content_type) = 0;
    // Makes a folder and every missing level above it; one already there is fine.
    virtual liquid::runtime::task<ZettaControlResult<void>> make_folder(std::string path) = 0;
    // Moves a file or a folder with everything in it; `to` must be free.
    virtual liquid::runtime::task<ZettaControlResult<void>> move(std::string from,
                                                                 std::string to) = 0;
    // Removes a file, or a folder with everything in it. Never the store itself.
    virtual liquid::runtime::task<ZettaControlResult<void>> remove(std::string path) = 0;
};

// What Lake hands a store browser to open one store (lake#664): the store, what the operator
// delivered for it (`store_connection`), and the fields of the workspace secret its spec names,
// read by Lake in the workspace's secret store (decision 58: the workspace's credential, never
// Orama's). `credential` is empty for a store that names none.
struct StoreOpening {
    ManagedStore store;
    nlohmann::json connection;
    std::map<std::string, std::string> credential;
};

// Opens the contents of a customer's own bucket (store_external_bucket). Injected
// (ServerOptions::store_browser, tests' fakes); the HTTP server's default is cloud_store_browser
// over its public-only outbound transport (store_buckets.hpp). Empty, Lake browses no external
// bucket and store_browse_refusal says why. Stores Lake keeps itself (zetta, lake) are opened by
// Lake, not by this.
using StoreBrowser =
    std::function<ZettaControlResult<std::shared_ptr<StoreContents>>(const StoreOpening& opening)>;

// The issue that added browsing each bucket and filesystem provider (lake#664).
inline constexpr std::string_view store_browse_issue =
    "https://github.com/oramasearch/lake/issues/664";

// Why no store of this type opens as a drive (a volume, whose contents belong to the service that
// mounts it; anything but a bucket or a filesystem), or empty.
std::string store_kind_refusal(std::string_view type);
// Why Lake cannot open a store of this type and provider when it has no StoreBrowser: the kind's
// refusal, or that this Lake is not configured to reach external buckets.
std::string store_browse_refusal(std::string_view type, std::string_view provider);

// Whether `path` is a store path: "" or names joined by "/", each a valid drive name.
bool valid_store_path(std::string_view path);
// "a/b" for ("a", "b"); "b" for ("", "b").
std::string join_store_path(std::string_view folder, std::string_view name);
// "a" for "a/b"; "" for "b".
std::string store_parent(std::string_view path);
// "b" for "a/b".
std::string store_name(std::string_view path);

} // namespace lake::control
