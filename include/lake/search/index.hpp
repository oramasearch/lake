#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lake::control {
class Store;
}

namespace liquid {
class repository;
class store_backend;
}

namespace lake::search {

struct Error {
    std::string code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;

enum class PublicationKind { BranchTip, TagSnapshot };

enum class QueryMode { Hot, Snapshot, Scan, Artifact };

[[nodiscard]] std::string_view to_string(QueryMode mode);

struct Publish {
    std::string customer;
    std::string repository;
    std::string ref;
    std::string change;
    PublicationKind kind = PublicationKind::BranchTip;
};

struct Query {
    std::string text;
    std::string customer;
    std::string repository;
    std::string ref;
    int limit = 20;
    std::string change;
    bool history = false;
    // Narrows the search to one workspace's index (lake#381).
    std::string workspace;
};

struct Hit {
    std::string customer;
    std::string repository;
    std::string path;
    std::string change;
    std::string snippet;
    double score = 0.0;
    std::string artifact;
    std::string artifact_version;
    std::string digest;
    std::size_t line = 0;
    // The workspace whose index holds the hit; empty for the space's index on lake's volume.
    std::string workspace;
};

struct ArtifactMember {
    std::string artifact;
    std::string version;
    std::string path;
    std::string digest;
    bool indexed = false;
    std::string reason;
};

struct ReindexResult {
    std::size_t artifacts = 0;
    std::size_t members = 0;
    std::size_t indexed = 0;
    std::size_t binary = 0;
    std::size_t truncated = 0;
};

struct QueryResult {
    QueryMode mode = QueryMode::Hot;
    std::size_t scanned_changes = 0;
    std::vector<Hit> items;
    // Workspaces a space-wide search could not read (stopped, or their store unavailable).
    std::vector<std::string> skipped_workspaces;
};

// The objects of one workspace's search index (lake#381, decision 54 S8): kept beside the
// workspace's liquid store in its workspace Zetta and sealed with the workspace's data key by the
// workspace store. Names are plain file names (`manifest.json`, `blobs.json`, ...).
class IndexObjects {
public:
    virtual ~IndexObjects() = default;
    // Nothing when the object is absent.
    virtual Result<std::optional<std::string>> get(const std::string& name) = 0;
    // Replaces the object atomically.
    virtual Result<void> put(const std::string& name, std::string bytes) = 0;
    // Removing an absent object is a success.
    virtual Result<void> remove(const std::string& name) = 0;
    virtual Result<std::vector<std::string>> list() = 0;
    // Everything put before it is durable once it answers.
    virtual Result<void> sync() = 0;
};

// Where a workspace's index objects live in its store's bucket: `search/<name>`. Liquid never lists
// or writes under it.
inline constexpr std::string_view workspace_index_prefix = "search/";

// The index objects of a workspace store, written through `backend`: a view of the workspace
// store's own backend fenced by its lease, so they are sealed with the workspace's data key like
// every object of the store and every write carries the lease's fence. `sync` makes what was put
// durable (Zetta's `:barrier`); the view's own barrier is never called, so the view never removes
// an object outside `search/`.
std::shared_ptr<IndexObjects> make_store_index_objects(std::shared_ptr<liquid::store_backend> backend,
                                                       std::function<Result<void>()> sync);

// One workspace's index as lent by lake's repository store, valid while the lender's hold (the
// workspace store's lane) is kept.
struct WorkspaceIndex {
    std::string customer;
    std::string workspace;
    // The workspace store's lease fence: an index loaded under another fence is loaded again.
    std::uint64_t generation = 0;
    std::shared_ptr<IndexObjects> objects;
    // The workspace's live repositories, which a rebuild indexes.
    std::function<Result<std::vector<std::string>>()> repositories;
    // A repository of the workspace, opened under the hold already taken.
    std::function<Result<liquid::repository*>(const std::string& repository)> open;
};

// A repository lent to search by lake's repository store (lake#380): `hold` keeps the store's
// lane and the shared open instance for as long as search reads it.
struct BorrowedRepository {
    std::shared_ptr<void> hold;
    liquid::repository* repository = nullptr;
    // Set when the repository is served by its workspace's store (lake#381): its index is that
    // workspace's, lent under the same hold.
    std::optional<WorkspaceIndex> workspace;
};
using RepositoryAccess =
    std::function<Result<BorrowedRepository>(const std::string& customer, const std::string& repository)>;

struct BorrowedWorkspace {
    std::shared_ptr<void> hold;
    WorkspaceIndex index;
};

// How search reaches workspace indexes (lake#381). Every member is set by the server.
struct WorkspaceAccess {
    // Lends a workspace's index under its store's lane; `customer` may be empty (looked up).
    std::function<Result<BorrowedWorkspace>(const std::string& customer, const std::string& workspace)>
        lend;
    // The same, but only when lake has the workspace's store open already (workspace deletion).
    std::function<Result<std::optional<BorrowedWorkspace>>(const std::string& workspace)> lend_if_open;
    // A space's switched workspaces as (space, workspace); every space's when `customer` is empty.
    std::function<Result<std::vector<std::pair<std::string, std::string>>>(const std::string& customer)>
        switched;
    // The workspace whose store serves a repository, or nothing for its space's store. `customer`
    // may be empty.
    std::function<Result<std::optional<std::string>>(const std::string& customer,
                                                     const std::string& repository)>
        route;
};

class Service {
public:
    explicit Service(std::filesystem::path store_root);

    // How search reads a repository. The server sets it to its repository store, which lends the
    // repository from whichever store serves it, under that store's lane, so search never opens
    // a second instance of a store another writer has open. Unset, search opens the space store
    // itself, which only a service that never had a repository store may do; once one was set,
    // search refuses instead.
    void set_repository_access(RepositoryAccess access);
    // Where switched workspaces' indexes are (lake#381). A repository served by a workspace store
    // is indexed in that workspace's index, beside its store, and never on lake's volume; what the
    // space's index still holds for it is dropped.
    void set_workspace_access(WorkspaceAccess access);
    // The bound on the memory of loaded workspace indexes (LAKE_SEARCH_MEMORY_BYTES); idle
    // workspaces are evicted, least recently used first, and loaded again on their next use.
    void set_memory_bound(std::size_t bytes);
    // The approximate memory of the workspace indexes loaded now, and which are loaded.
    [[nodiscard]] std::size_t workspace_memory() const;
    [[nodiscard]] std::vector<std::string> loaded_workspaces() const;

    [[nodiscard]] Result<void> publish(const Publish& change);
    [[nodiscard]] Result<ReindexResult> reindex_artifacts(const std::string& customer,
                                                          const std::string& repository);
    [[nodiscard]] Result<std::vector<ArtifactMember>>
    artifact_members(const std::string& customer,
                     const std::string& repository,
                     const std::string& version) const;
    [[nodiscard]] Result<QueryResult>
    query(const Query& query, const std::string& principal, control::Store& store) const;
    // Drops one repository's search segments and artifact members, and the indexed blobs nothing
    // else references (a workspace deletion, lake#514). Dropping a repository with none is a
    // success, so a resumed deletion can repeat it.
    [[nodiscard]] Result<void> drop_repository(const std::string& customer,
                                               const std::string& repository);
    // Drops from a space's index on lake's volume what it holds of repositories now served by a
    // workspace store, and the history-scan blobs no segment references (a switch, lake#381).
    // Answers how many repositories were dropped.
    [[nodiscard]] Result<std::size_t> drop_switched(const std::string& customer);
    // Forgets a workspace's index and removes its objects when lake has the workspace's store
    // open (a workspace deletion, lake#514); otherwise they go with the workspace Zetta.
    [[nodiscard]] Result<void> drop_workspace(const std::string& workspace);
    // Rebuilds a workspace's index from its repositories, replacing what it holds.
    [[nodiscard]] Result<void> rebuild_workspace(const std::string& customer, const std::string& workspace);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lake::search
