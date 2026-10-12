// A store Lake keeps in the workspace Zetta (provider `zetta`, lake#664): sealed records for the
// tree and sealed objects for the bytes. See store_kept.hpp.

#include "lake/control/store_kept.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>
#include <vector>

#include "lake/control/agent_bus.hpp"

namespace lake::control {
namespace {

template <class T> using Result = ZettaControlResult<T>;

struct Failed {
    ZettaControlError error;
    template <class T> operator Result<T>() const {
        return std::unexpected(error);
    }
};
Failed failed(ZettaControlError error) {
    return {std::move(error)};
}
Failed failed(std::string code, std::string message) {
    return {ZettaControlError{std::move(code), std::move(message)}};
}

// The most items one folder move or removal walks.
constexpr std::size_t max_tree_items = 10'000;

std::string now_rfc3339() {
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", now);
}

struct Kept {
    StoreItem item;
    std::string key; // the file's object, empty for a folder
    std::int64_t revision = 0;
};

class ZettaStoreContents final : public StoreContents {
public:
    ZettaStoreContents(WorkspaceRecords records, DriveFileStore files, std::string store_id)
        : records_(std::move(records)), files_(std::move(files)), store_id_(std::move(store_id)) {}

    liquid::runtime::task<Result<std::vector<StoreItem>>> list(std::string folder) override {
        auto children = settled(co_await children_of(folder));
        if (!children) {
            co_return failed(children.error());
        }
        std::vector<StoreItem> items;
        for (Kept& kept : *children) {
            items.push_back(std::move(kept.item));
        }
        std::ranges::sort(items, [](const StoreItem& left, const StoreItem& right) {
            return left.folder != right.folder ? left.folder : left.path < right.path;
        });
        co_return items;
    }

    liquid::runtime::task<Result<std::optional<StoreItem>>> stat(std::string path) override {
        if (path.empty()) {
            co_return std::optional<StoreItem>(StoreItem{"", true});
        }
        auto found = settled(co_await find(path));
        if (!found) {
            co_return failed(found.error());
        }
        if (!*found) {
            co_return std::optional<StoreItem>();
        }
        co_return std::optional<StoreItem>((*found)->item);
    }

    liquid::runtime::task<Result<std::string>>
    read(std::string path, std::uint64_t offset, std::uint64_t length) override {
        auto found = settled(co_await find(path));
        if (!found) {
            co_return failed(found.error());
        }
        if (!*found || (*found)->item.folder) {
            co_return failed("not_found", "no file at " + path);
        }
        co_return settled(
            co_await files_.read((*found)->key, (*found)->item.size, offset, length));
    }

    liquid::runtime::task<Result<StoreItem>>
    write(std::string path, std::string bytes, std::string content_type) override {
        if (path.empty()) {
            co_return failed("invalid_input", "a file needs a name");
        }
        auto existing = settled(co_await find(path));
        if (!existing) {
            co_return failed(existing.error());
        }
        if (*existing && (*existing)->item.folder) {
            co_return failed("conflict", "a folder is at " + path);
        }
        auto parent = settled(co_await make_folder(store_parent(path)));
        if (!parent) {
            co_return failed(parent.error());
        }
        auto ready = settled(co_await files_.ready());
        if (!ready) {
            co_return failed(ready.error());
        }
        const std::string key = new_drive_file_key();
        const std::string digest = drive_sha256_hex(bytes);
        const std::uint64_t size = bytes.size();
        auto written = settled(co_await files_.write(key, 0, std::move(bytes), true));
        if (!written) {
            co_return failed(written.error());
        }
        Kept kept{StoreItem{path, false, size, content_type, now_rfc3339(), digest}, key};
        RecordCondition condition;
        if (*existing) {
            condition.if_revision = (*existing)->revision;
        } else {
            condition.if_absent = true;
        }
        auto put = settled(co_await records_.put(
            store_items_collection, id_of(path), lookup_of(store_parent(path)), encode(kept),
            condition));
        if (!put) {
            [[maybe_unused]] auto dropped = co_await files_.remove(key);
            co_return failed(put.error().code == "conflict"
                                 ? ZettaControlError{"conflict", path + " changed meanwhile"}
                                 : put.error());
        }
        if (*existing) {
            // The replaced bytes; a failure leaves an unreachable object, never a wrong file.
            [[maybe_unused]] auto dropped = co_await files_.remove((*existing)->key);
        }
        co_return kept.item;
    }

    liquid::runtime::task<Result<void>> make_folder(std::string path) override {
        std::vector<std::string> levels;
        for (std::string level = path; !level.empty(); level = store_parent(level)) {
            levels.push_back(level);
        }
        std::ranges::reverse(levels);
        for (const std::string& level : levels) {
            auto found = settled(co_await find(level));
            if (!found) {
                co_return failed(found.error());
            }
            if (*found) {
                if (!(*found)->item.folder) {
                    co_return failed("conflict", "a file is at " + level);
                }
                continue;
            }
            Kept kept{StoreItem{level, true, 0, "", now_rfc3339(), ""}, ""};
            auto put = settled(co_await records_.put(store_items_collection,
                                                     id_of(level),
                                                     lookup_of(store_parent(level)),
                                                     encode(kept),
                                                     RecordCondition{true, std::nullopt}));
            if (!put && put.error().code != "conflict") {
                co_return failed(put.error());
            }
        }
        co_return Result<void>();
    }

    liquid::runtime::task<Result<void>> move(std::string from, std::string to) override {
        if (from.empty() || to.empty()) {
            co_return failed("invalid_input", "the store's top is never moved");
        }
        if (to.starts_with(from + "/")) {
            co_return failed("invalid_input", "a folder cannot move into itself");
        }
        auto source = settled(co_await find(from));
        if (!source) {
            co_return failed(source.error());
        }
        if (!*source) {
            co_return failed("not_found", "nothing is at " + from);
        }
        auto target = settled(co_await find(to));
        if (!target) {
            co_return failed(target.error());
        }
        if (*target) {
            co_return failed("conflict", "something is already at " + to);
        }
        auto parent = settled(co_await make_folder(store_parent(to)));
        if (!parent) {
            co_return failed(parent.error());
        }
        std::vector<Kept> items{**source};
        if ((*source)->item.folder) {
            auto below = settled(co_await descendants(from));
            if (!below) {
                co_return failed(below.error());
            }
            items.insert(items.end(), below->begin(), below->end());
        }
        // New records first, parents before children; then the old ones, children first.
        for (const Kept& kept : items) {
            Kept moved = kept;
            moved.item.path = to + kept.item.path.substr(from.size());
            auto put = settled(co_await records_.put(store_items_collection,
                                                     id_of(moved.item.path),
                                                     lookup_of(store_parent(moved.item.path)),
                                                     encode(moved),
                                                     RecordCondition{true, std::nullopt}));
            if (!put) {
                co_return failed(put.error());
            }
        }
        for (auto kept = items.rbegin(); kept != items.rend(); ++kept) {
            auto removed = settled(co_await records_.remove(
                store_items_collection, id_of(kept->item.path), std::nullopt));
            if (!removed && removed.error().code != "not_found") {
                co_return failed(removed.error());
            }
        }
        co_return Result<void>();
    }

    liquid::runtime::task<Result<void>> remove(std::string path) override {
        if (path.empty()) {
            co_return failed("invalid_input", "the store itself is never removed");
        }
        auto found = settled(co_await find(path));
        if (!found) {
            co_return failed(found.error());
        }
        if (!*found) {
            co_return failed("not_found", "nothing is at " + path);
        }
        std::vector<Kept> items{**found};
        if ((*found)->item.folder) {
            auto below = settled(co_await descendants(path));
            if (!below) {
                co_return failed(below.error());
            }
            items.insert(items.end(), below->begin(), below->end());
        }
        for (auto kept = items.rbegin(); kept != items.rend(); ++kept) {
            auto removed = settled(co_await records_.remove(
                store_items_collection, id_of(kept->item.path), std::nullopt));
            if (!removed && removed.error().code != "not_found") {
                co_return failed(removed.error());
            }
            if (!kept->key.empty()) {
                auto dropped = settled(co_await files_.remove(kept->key));
                if (!dropped) {
                    co_return failed(dropped.error());
                }
            }
        }
        co_return Result<void>();
    }

private:
    std::string id_of(std::string_view path) const {
        std::string lookup = "item\n" + store_id_ + "\n";
        lookup += path;
        return "si-" + records_.index_of(store_items_collection, lookup);
    }
    std::string lookup_of(std::string_view folder) const {
        std::string lookup = "folder\n" + store_id_ + "\n";
        lookup += folder;
        return lookup;
    }

    nlohmann::json encode(const Kept& kept) const {
        return {{"store_id", store_id_},
                {"path", kept.item.path},
                {"folder", kept.item.folder},
                {"size", kept.item.size},
                {"content_type", kept.item.content_type},
                {"modified_at", kept.item.modified_at},
                {"sha256", kept.item.etag},
                {"key", kept.key}};
    }

    std::optional<Kept> decode(const StoredRecord& record) const {
        const nlohmann::json& body = record.body;
        if (!body.is_object() || body.value("store_id", std::string()) != store_id_ ||
            !body.contains("path") || !body.at("path").is_string()) {
            return std::nullopt;
        }
        Kept kept;
        kept.item.path = body.at("path").get<std::string>();
        kept.item.folder = body.value("folder", false);
        kept.item.size = body.value("size", std::uint64_t{0});
        kept.item.content_type = body.value("content_type", std::string());
        kept.item.modified_at = body.value("modified_at", std::string());
        kept.item.etag = body.value("sha256", std::string());
        kept.key = body.value("key", std::string());
        kept.revision = record.revision;
        return kept;
    }

    liquid::runtime::task<Result<std::optional<Kept>>> find(std::string path) {
        auto record = settled(co_await records_.get(store_items_collection, id_of(path)));
        if (!record) {
            co_return failed(record.error());
        }
        if (!*record) {
            co_return std::optional<Kept>();
        }
        auto kept = decode(**record);
        if (kept && kept->item.path != path) {
            kept.reset();
        }
        co_return kept;
    }

    liquid::runtime::task<Result<std::vector<Kept>>> children_of(std::string folder) {
        auto listed =
            settled(co_await records_.list(store_items_collection, lookup_of(folder)));
        if (!listed) {
            co_return failed(listed.error());
        }
        std::vector<Kept> out;
        for (const StoredRecord& record : *listed) {
            if (auto kept = decode(record); kept && store_parent(kept->item.path) == folder) {
                out.push_back(std::move(*kept));
            }
        }
        co_return out;
    }

    // Everything below a folder, each folder before what is in it.
    liquid::runtime::task<Result<std::vector<Kept>>> descendants(std::string folder) {
        std::vector<Kept> out;
        std::vector<std::string> pending{folder};
        while (!pending.empty()) {
            const std::string next = pending.back();
            pending.pop_back();
            auto children = settled(co_await children_of(next));
            if (!children) {
                co_return failed(children.error());
            }
            for (Kept& kept : *children) {
                if (kept.item.folder) {
                    pending.push_back(kept.item.path);
                }
                out.push_back(std::move(kept));
            }
            if (out.size() > max_tree_items) {
                co_return failed("invalid_input",
                                 std::format("a folder of more than {} items cannot be moved or "
                                             "removed in one call",
                                             max_tree_items));
            }
        }
        std::ranges::stable_sort(out, [](const Kept& left, const Kept& right) {
            return std::ranges::count(left.item.path, '/') < std::ranges::count(right.item.path, '/');
        });
        co_return out;
    }

    WorkspaceRecords records_;
    DriveFileStore files_;
    std::string store_id_;
};

} // namespace

std::string kept_store_bucket(std::string_view slug) {
    std::string out = "lake_";
    for (const char c : slug) {
        out.push_back(c == '-' ? '_' : c);
    }
    return out;
}

std::shared_ptr<StoreContents>
zetta_store_contents(WorkspaceRecords records, DriveFileStore files, std::string store_id) {
    return std::make_shared<ZettaStoreContents>(
        std::move(records), std::move(files), std::move(store_id));
}

} // namespace lake::control
