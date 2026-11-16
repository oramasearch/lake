// SPDX-License-Identifier: MIT

// A `lake` store over a liquid repository (lake#664). See store_repository.hpp.

#include "store_repository.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "lake/api/drive.hpp"
#include "lake/api/registry.hpp"
#include <cstring>

#include "lake/control/agent_bus.hpp"
#include "lake/control/drives.hpp"

namespace lake::api {
namespace {

template <class T> using Result = control::ZettaControlResult<T>;

struct Failed {
    control::ZettaControlError error;
    template <class T> operator Result<T>() const {
        return std::unexpected(error);
    }
};
Failed failed(std::string code, std::string message) {
    return {control::ZettaControlError{std::move(code), std::move(message)}};
}

// The folder marker a drive keeps in an empty folder; never shown.
constexpr std::string_view folder_marker = ".lake-folder";

// A repository-store answer, or the pool's failure, as the API's error.
template <class T>
Result<T> answer(liquid::result<SyncStoreResult<T>> ran) {
    if (!ran) {
        return std::unexpected(control::ZettaControlError{"store_unavailable", ran.error().message});
    }
    if (!*ran) {
        return std::unexpected(control::ZettaControlError{
            ran->error().code.empty() ? "store_error" : ran->error().code, ran->error().message});
    }
    if constexpr (std::is_void_v<T>) {
        return Result<void>();
    } else {
        return std::move(**ran);
    }
}

class RepositoryStoreContents final : public control::StoreContents {
public:
    explicit RepositoryStoreContents(RepositoryBranch branch) : branch_(std::move(branch)) {}

    liquid::runtime::task<Result<std::vector<control::StoreItem>>>
    list(std::string folder) override {
        auto entries = control::settled(co_await tree());
        if (!entries) {
            co_return failed(entries.error().code, entries.error().message);
        }
        std::vector<control::StoreItem> items;
        for (const RepositoryTreeEntry& entry : *entries) {
            if (control::store_parent(entry.path) == folder && shown(entry.path)) {
                items.push_back(item(entry));
            }
        }
        std::ranges::sort(items, [](const control::StoreItem& left, const control::StoreItem& right) {
            return left.folder != right.folder ? left.folder : left.path < right.path;
        });
        co_return items;
    }

    liquid::runtime::task<Result<std::optional<control::StoreItem>>>
    stat(std::string path) override {
        if (path.empty()) {
            co_return std::optional<control::StoreItem>(control::StoreItem{"", true});
        }
        auto entries = control::settled(co_await tree());
        if (!entries) {
            co_return failed(entries.error().code, entries.error().message);
        }
        for (const RepositoryTreeEntry& entry : *entries) {
            if (entry.path == path && shown(path)) {
                co_return std::optional<control::StoreItem>(item(entry));
            }
        }
        co_return std::optional<control::StoreItem>();
    }

    liquid::runtime::task<Result<std::string>>
    read(std::string path, std::uint64_t offset, std::uint64_t length) override {
        auto file = answer<RepositoryFile>(
            co_await run_blocking([read = branch_.file, path] { return read(path); }));
        if (!file) {
            co_return failed(file.error().code, file.error().message);
        }
        std::string bytes = file->raw_contents.empty()
                                ? std::move(file->contents)
                                : std::string(reinterpret_cast<const char*>(file->raw_contents.data()),
                                              file->raw_contents.size());
        if (offset >= bytes.size()) {
            co_return std::string();
        }
        co_return bytes.substr(offset, length);
    }

    liquid::runtime::task<Result<control::StoreItem>>
    write(std::string path, std::string bytes, std::string content_type) override {
        auto there = control::settled(co_await stat(path));
        if (!there) {
            co_return failed(there.error().code, there.error().message);
        }
        if (*there && (*there)->folder) {
            co_return failed("conflict", "a folder is at " + path);
        }
        if (auto blocked = control::settled(co_await file_above(path)); !blocked || *blocked) {
            co_return blocked ? failed("conflict", "a file is at " + **blocked)
                              : failed(blocked.error().code, blocked.error().message);
        }
        const std::uint64_t size = bytes.size();
        RepositoryWriteOperation operation{.kind = RepositoryWriteKind::File, .path = path};
        operation.contents.resize(bytes.size());
        std::memcpy(operation.contents.data(), bytes.data(), bytes.size());
        auto committed = control::settled(co_await commit("Write " + path, {std::move(operation)}));
        if (!committed) {
            co_return failed(committed.error().code, committed.error().message);
        }
        auto written = control::settled(co_await stat(path));
        if (written && *written) {
            control::StoreItem out = **written;
            out.content_type = content_type;
            co_return out;
        }
        co_return control::StoreItem{path, false, size, content_type, "", ""};
    }

    liquid::runtime::task<Result<void>> make_folder(std::string path) override {
        if (path.empty()) {
            co_return Result<void>();
        }
        auto there = control::settled(co_await stat(path));
        if (!there) {
            co_return failed(there.error().code, there.error().message);
        }
        if (*there) {
            if ((*there)->folder) {
                co_return Result<void>();
            }
            co_return failed("conflict", "a file is at " + path);
        }
        if (auto blocked = control::settled(co_await file_above(path)); !blocked || *blocked) {
            co_return blocked ? failed("conflict", "a file is at " + **blocked)
                              : failed(blocked.error().code, blocked.error().message);
        }
        co_return control::settled(co_await commit(
            "Create folder " + path, {{.kind = RepositoryWriteKind::Folder, .path = path}}));
    }

    liquid::runtime::task<Result<void>> move(std::string from, std::string to) override {
        if (from.empty() || to.empty() || to.starts_with(from + "/")) {
            co_return failed("invalid_input", "a folder cannot move into itself or the top");
        }
        auto source = control::settled(co_await stat(from));
        if (!source) {
            co_return failed(source.error().code, source.error().message);
        }
        if (!*source) {
            co_return failed("not_found", "nothing is at " + from);
        }
        auto target = control::settled(co_await stat(to));
        if (!target) {
            co_return failed(target.error().code, target.error().message);
        }
        if (*target) {
            co_return failed("conflict", "something is already at " + to);
        }
        auto parent = control::settled(co_await make_folder(control::store_parent(to)));
        if (!parent) {
            co_return failed(parent.error().code, parent.error().message);
        }
        co_return control::settled(co_await commit(
            "Move " + from + " to " + to,
            {{.kind = RepositoryWriteKind::Moved, .path = to, .source_path = from}}));
    }

    liquid::runtime::task<Result<void>> remove(std::string path) override {
        auto there = control::settled(co_await stat(path));
        if (!there) {
            co_return failed(there.error().code, there.error().message);
        }
        if (!*there || path.empty()) {
            co_return failed("not_found", "nothing is at " + path);
        }
        co_return control::settled(co_await commit(
            "Delete " + path,
            {{.kind = (*there)->folder ? RepositoryWriteKind::DeletedFolder
                                       : RepositoryWriteKind::DeletedFile,
              .path = path}}));
    }

private:
    static bool shown(std::string_view path) {
        return control::store_name(path) != folder_marker && control::valid_store_path(path);
    }

    static control::StoreItem item(const RepositoryTreeEntry& entry) {
        const bool folder = entry.kind == "tree";
        return {entry.path,
                folder,
                folder ? 0 : entry.size,
                "",
                entry.last_changed_at,
                folder ? std::string() : entry.blob};
    }

    // The branch's tree; empty for a repository whose branch has no change yet.
    liquid::runtime::task<Result<std::vector<RepositoryTreeEntry>>> tree() {
        auto entries = answer<std::vector<RepositoryTreeEntry>>(
            co_await run_blocking([tree = branch_.tree] { return tree(); }));
        if (!entries) {
            if (entries.error().code == "not_found" &&
                entries.error().message.find("repository") == std::string::npos) {
                co_return std::vector<RepositoryTreeEntry>();
            }
            co_return failed(entries.error().code, entries.error().message);
        }
        co_return std::move(*entries);
    }

    liquid::runtime::task<Result<std::optional<std::string>>> file_above(std::string path) {
        auto entries = control::settled(co_await tree());
        if (!entries) {
            co_return failed(entries.error().code, entries.error().message);
        }
        for (std::string level = control::store_parent(path); !level.empty();
             level = control::store_parent(level)) {
            for (const RepositoryTreeEntry& entry : *entries) {
                if (entry.path == level && entry.kind != "tree") {
                    co_return std::optional<std::string>(level);
                }
            }
        }
        co_return std::optional<std::string>();
    }

    liquid::runtime::task<Result<void>> commit(std::string message,
                                               std::vector<RepositoryWriteOperation> operations) {
        auto committed = answer<std::string>(co_await run_blocking(
            [commit = branch_.commit, message = std::move(message),
             operations = std::move(operations)]() mutable {
                return commit(message, std::move(operations));
            }));
        if (!committed) {
            co_return failed(committed.error().code, committed.error().message);
        }
        co_return Result<void>();
    }

    RepositoryBranch branch_;
};

} // namespace

std::shared_ptr<control::StoreContents> repository_store_contents(RepositoryBranch branch) {
    return std::make_shared<RepositoryStoreContents>(std::move(branch));
}

std::shared_ptr<control::StoreContents> repository_store_contents(SyncStore* repositories,
                                                                  std::string customer_id,
                                                                  std::string repository_id,
                                                                  std::string branch,
                                                                  ApiContext& context) {
    ApiContext* caller = &context;
    return repository_store_contents(RepositoryBranch{
        [=] { return repositories->repository_tree(customer_id, repository_id, branch); },
        [=](const std::string& path) {
            return repositories->repository_file(customer_id, repository_id, branch, path);
        },
        [=](const std::string& message, std::vector<RepositoryWriteOperation> operations) {
            const auto repository = caller->store->get_repository(repository_id);
            if (!repository) {
                return SyncStoreResult<std::string>(std::unexpected(
                    SyncStoreError{repository.error().code, repository.error().message}));
            }
            auto authorize = repository_write_authorizer(
                *repository, operations, *caller, branch,
                branch_protection_blocking(*caller, *repository));
            return repositories->commit_repository_files(customer_id,
                                                         repository_id,
                                                         branch,
                                                         message,
                                                         std::move(operations),
                                                         std::move(authorize));
        }});
}

} // namespace lake::api
