// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace lake::control {
class Store;
class ZettaObjectClient;
class WorkspaceStoreCipher;
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
    // Narrows the search to one workspace's index (lake#381). The caller has checked that the
    // principal may search it and set `customer` to its space.
    std::string workspace;
    // The spaces whose switched workspaces a space-wide search may open (and, with `customer`
    // empty, whose indexes on lake's volume it reads); nothing: every space (the operator). A
    // workspace outside them is never lent, opened or rebuilt for this query.
    std::optional<std::set<std::string>> spaces;
    // An agent credential's confinement (lake#985): while `confined`, each hit's repository is
    // authorized only inside the account `confined_customer` and, when set, the workspace
    // `confined_workspace`, as control::AuthorizationConfinement decides.
    bool confined = false;
    std::string confined_customer;
    std::string confined_workspace;
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
    // Workspaces whose index is being built (after a switch, or because it was lost): they answer
    // nothing until it is ready, and are not waited for.
    std::vector<std::string> building_workspaces;
    // Workspaces whose index answered but is partly stale (lake#540): a write to it failed, and the
    // repositories it left pending are retried, after a growing delay, until it succeeds.
    std::vector<std::string> stale_workspaces;
};

// The objects of one workspace's search index (lake#381), kept beside the
// workspace's liquid store in its workspace Zetta. Names are plain names (`manifest.json`,
// `runs/<id>.json`, ...).
class IndexObjects {
public:
    virtual ~IndexObjects() = default;
    // Nothing when the object is absent.
    virtual Result<std::optional<std::string>> get(const std::string& name) = 0;
    // Replaces the object atomically; with `once`, refuses (`conflict`) an object that exists.
    virtual Result<void> put(const std::string& name, std::string bytes, bool once = false) = 0;
    // Removing an absent object is a success.
    virtual Result<void> remove(const std::string& name) = 0;
    virtual Result<std::vector<std::string>> list() = 0;
    // Everything put before it is durable once it answers.
    virtual Result<void> sync() = 0;
};

// Where a workspace's index objects live in its store's bucket: `search/<name>`. Liquid never lists
// or writes under it.
inline constexpr std::string_view workspace_index_prefix = "search/";
// The object a workspace's index is loaded from; without it the index is rebuilt.
inline constexpr std::string_view workspace_index_manifest = "search/manifest.json";

// A workspace's index objects in its store's bucket, through their own client: each object is
// `LKSIDX1\0` then AES-256-GCM with the workspace's data key (the store's cipher), its associated
// data binding the workspace and the object's name. Every write carries `fence`, the store's lease
// fence. A failure here never marks the store failed; a write refused as fenced calls `on_lost`,
// since the store's lease has passed to another holder. Objects carry no key version: a rotated
// data key makes them unreadable (`corrupt`), and the index is rebuilt. A write-once put retried
// after Zetta already took it (`already_exists` with the same content) is a success.
std::shared_ptr<IndexObjects>
make_sealed_index_objects(std::shared_ptr<control::ZettaObjectClient> client,
                          std::shared_ptr<const control::WorkspaceStoreCipher> cipher,
                          std::uint64_t fence,
                          std::function<void()> on_lost);

// One workspace's index as lent by lake's repository store.
struct WorkspaceIndex {
    std::string customer;
    std::string workspace;
    // The workspace store's lease fence: an index loaded under another fence is loaded again.
    std::uint64_t generation = 0;
    // Safe to use after the lend's hold is let go (they hold their own client and key).
    std::shared_ptr<IndexObjects> objects;
    // The workspace's live repositories, which a rebuild indexes. Fails rather than answer a
    // partial list.
    std::function<Result<std::vector<std::string>>()> repositories;
    // A repository's releases as (tag, change), from its release records; a rebuild republishes
    // their snapshots.
    std::function<Result<std::vector<std::pair<std::string, std::string>>>(const std::string& repository)>
        tags;
    // How many more bytes the index may add to the workspace's bucket, so it never takes the room
    // liquid's own writes need under the bucket quota; nothing: no quota.
    std::function<std::optional<std::uint64_t>()> write_budget;
    // Told how many bytes each commit wrote, so commits between two usage readings do not each
    // spend the whole budget.
    std::function<void(std::uint64_t)> spent;
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
// `lake_work`: lake's own work (a rebuild) takes the store's lane without admission, waiting for it,
// rather than being refused when the lane is busy with requests.
using RepositoryAccess = std::function<Result<BorrowedRepository>(
    const std::string& customer, const std::string& repository, bool lake_work)>;

struct BorrowedWorkspace {
    std::shared_ptr<void> hold;
    WorkspaceIndex index;
};

// How search reaches workspace indexes (lake#381). Every member is set by the server.
struct WorkspaceAccess {
    // Lends a workspace's index under its store's lane; `customer` may be empty (looked up).
    std::function<Result<BorrowedWorkspace>(const std::string& customer, const std::string& workspace,
                                            bool lake_work)>
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
    // Runs Service::rebuild_workspace after `delay`, off every request path (lake's search index
    // worker). Unset: it runs at once, on the calling thread, after search let go of every lane and
    // lock (a delayed retry is then left to the workspace's next use).
    std::function<void(const std::string& customer, const std::string& workspace,
                       std::chrono::milliseconds delay)>
        schedule;
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
    // Removes one repository from its workspace's index, and from its space's index (a repository
    // deletion). A workspace that is not switched, or whose index is being built, has nothing to
    // remove.
    [[nodiscard]] Result<void> forget_repository(const std::string& customer,
                                                 const std::string& workspace,
                                                 const std::string& repository);
    // Marks a workspace's index as being built and schedules its rebuild (a switch).
    void schedule_rebuild(const std::string& customer, const std::string& workspace);
    // The scheduled job for a workspace's index. While the index is missing or being built, it
    // rebuilds it from the repositories: each read under its store's lane (lake's own work, not
    // admitted), one at a time, and the index written without holding a lane. Repositories written
    // meanwhile are indexed again afterwards, and ones deleted meanwhile are left out. With the
    // index ready, it applies only the writes and deletions still pending. A failed rebuild is
    // retried on the workspace's next use after a growing delay (kept in memory only: a restarted
    // lake tries again at once).
    [[nodiscard]] Result<void> rebuild_workspace(const std::string& customer, const std::string& workspace);
    // The first delay before a workspace's pending repositories are tried again after a failed
    // index write (lake#540); later retries double it, up to 10 minutes. Default 15 s.
    void set_pending_retry(std::chrono::milliseconds first);
    // Stops scheduling and running rebuilds (the server is stopping); one in progress stops at its
    // next repository. set_workspace_access starts them again.
    void shutdown();
    // The workspaces search keeps state for (tests).
    [[nodiscard]] std::vector<std::string> tracked_workspaces() const;
    // lake#536: the workspaces whose stored index is behind what search knows: a rebuild running
    // or due, or repositories pending or removed but not applied yet. That knowledge is in memory
    // only, so a draining lake makes the next process rebuild them (it removes their manifest).
    [[nodiscard]] std::vector<std::string> unsettled_workspaces() const;

private:
    [[nodiscard]] Result<void> apply_pending(const std::string& customer, const std::string& workspace);

    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace lake::search
