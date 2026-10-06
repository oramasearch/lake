#include "lake/search/index.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include <liquid/format.hpp>
#include <liquid/liquid.hpp>
#include <oramacore/index.hpp>

#include "lake/control/policy.hpp"

namespace lake::search {
namespace {

using nlohmann::json;

struct Occurrence {
    std::string repository;
    std::string ref;
    std::string change;
    std::string path;
    std::string blob;
    PublicationKind kind = PublicationKind::BranchTip;
    std::string artifact;
    std::string artifact_version;
    std::string digest;
    bool indexed = true;
    std::string reason;
};

struct Segment {
    std::string repository;
    std::string ref;
    std::string change;
    PublicationKind kind = PublicationKind::BranchTip;
    std::string file;
    std::vector<Occurrence> occurrences;
};

struct BlobRecord {
    std::string content;
};

struct ArtifactSegment {
    std::string repository;
    std::string artifact;
    std::string version;
    std::vector<Occurrence> members;
};

struct ArtifactScope {
    std::string name;
    std::string version;
};

struct TrigramPosting {
    std::string blob;
    std::vector<std::size_t> positions;
};

std::string publication_kind_name(PublicationKind kind) {
    return kind == PublicationKind::TagSnapshot ? "tag" : "branch";
}

PublicationKind publication_kind(const json& value) {
    return value.value("kind", "branch") == "tag" ? PublicationKind::TagSnapshot
                                                  : PublicationKind::BranchTip;
}

std::string slot_key(PublicationKind kind, const std::string& repository, const std::string& ref) {
    return publication_kind_name(kind) + "\n" + repository + "\n" + ref;
}

std::string hex_string(std::string_view value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string output;
    output.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    return output;
}

std::string segment_name(const Segment& segment) {
    return "segment-" + segment.repository + "-" + segment.change + "-" +
           publication_kind_name(segment.kind) + "-" + hex_string(segment.ref) + ".json";
}

std::vector<std::string> trigrams(const std::string& text) {
    std::vector<std::string> output;
    if (text.size() < 3) {
        return output;
    }
    output.reserve(text.size() - 2);
    for (std::size_t index = 0; index + 3 <= text.size(); ++index) {
        output.push_back(text.substr(index, 3));
    }
    return output;
}

std::string snippet_for(const std::string& content, const std::string& needle) {
    std::size_t at = content.find(needle);
    if (at == std::string::npos) {
        at = 0;
    }
    const std::size_t begin = at > 48 ? at - 48 : 0;
    const std::size_t length = std::min<std::size_t>(160, content.size() - begin);
    std::string snippet = content.substr(begin, length);
    std::replace(snippet.begin(), snippet.end(), '\n', ' ');
    return snippet;
}

std::size_t line_for(const std::string& content,
                     const std::string& needle,
                     const std::optional<std::regex>& expression) {
    std::size_t position = content.find(needle);
    if (expression) {
        std::smatch match;
        if (std::regex_search(content, match, *expression)) {
            position = static_cast<std::size_t>(match.position());
        }
    }
    if (position == std::string::npos) {
        return 1;
    }
    return static_cast<std::size_t>(std::count(
               content.begin(), content.begin() + static_cast<std::ptrdiff_t>(position), '\n')) +
           1;
}

bool is_text(std::span<const std::byte> bytes) {
    for (const std::byte value : bytes) {
        const unsigned char character = std::to_integer<unsigned char>(value);
        if (character == 0) {
            return false;
        }
        if (character < 0x09 || (character > 0x0d && character < 0x20)) {
            return false;
        }
    }
    return true;
}

Result<std::pair<std::string, std::optional<ArtifactScope>>>
split_artifact_scope(const std::string& input) {
    constexpr std::string_view prefix = "in:artifact:";
    std::istringstream stream(input);
    std::vector<std::string> terms;
    std::optional<ArtifactScope> scope;
    for (std::string term; stream >> term;) {
        if (!term.starts_with(prefix)) {
            terms.push_back(std::move(term));
            continue;
        }
        if (scope) {
            return std::unexpected(Error{"invalid_input", "artifact scope is repeated"});
        }
        std::string value = term.substr(prefix.size());
        if (value.empty()) {
            return std::unexpected(Error{"invalid_input", "artifact scope requires a name"});
        }
        ArtifactScope parsed;
        const std::size_t at = value.rfind('@');
        if (at == std::string::npos) {
            parsed.name = std::move(value);
        } else {
            parsed.name = value.substr(0, at);
            parsed.version = value.substr(at + 1);
            if (parsed.name.empty() || parsed.version.empty()) {
                return std::unexpected(
                    Error{"invalid_input", "artifact scope has an empty name or version"});
            }
        }
        scope = std::move(parsed);
    }
    std::string text;
    for (const std::string& term : terms) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += term;
    }
    if (text.empty()) {
        return std::unexpected(Error{"invalid_input", "query is required"});
    }
    return std::pair{std::move(text), std::move(scope)};
}

const TrigramPosting* posting_for(const std::vector<TrigramPosting>& postings,
                                  const std::string& blob) {
    const auto found =
        std::find_if(postings.begin(), postings.end(), [&](const TrigramPosting& posting) {
            return posting.blob == blob;
        });
    return found == postings.end() ? nullptr : &*found;
}

bool has_contiguous_positions(
    const std::unordered_map<std::string, std::vector<TrigramPosting>>& index,
    const std::vector<std::string>& keys,
    const std::string& blob) {
    if (keys.empty()) {
        return true;
    }
    const auto first = index.find(keys.front());
    if (first == index.end()) {
        return false;
    }
    const TrigramPosting* initial = posting_for(first->second, blob);
    if (!initial) {
        return false;
    }
    for (const std::size_t position : initial->positions) {
        bool matches = true;
        for (std::size_t offset = 1; offset < keys.size(); ++offset) {
            const auto next = index.find(keys[offset]);
            const TrigramPosting* posting =
                next == index.end() ? nullptr : posting_for(next->second, blob);
            if (!posting || !std::ranges::binary_search(posting->positions, position + offset)) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

std::string regex_literal(const std::string& expression) {
    std::string longest;
    std::string current;
    bool escaped = false;
    for (const char character : expression) {
        if (escaped) {
            if (character == 's' || character == 'S' || character == 'd' || character == 'D' ||
                character == 'w' || character == 'W') {
                if (current.size() > longest.size()) {
                    longest = current;
                }
                current.clear();
                escaped = false;
                continue;
            }
            current.push_back(character);
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' ||
            character == '-') {
            current.push_back(character);
            continue;
        }
        if (current.size() > longest.size()) {
            longest = current;
        }
        current.clear();
    }
    if (current.size() > longest.size()) {
        longest = current;
    }
    return longest;
}

// Text the index keeps: no control bytes other than whitespace, and valid UTF-8, so the catalogue
// stays JSON. Anything else is listed by digest only (artifacts) or not indexed (files).
bool is_indexable_text(std::span<const std::byte> bytes) {
    if (!is_text(bytes)) {
        return false;
    }
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto lead = std::to_integer<unsigned char>(bytes[index]);
        std::size_t length = 0;
        if (lead < 0x80) {
            length = 1;
        } else if ((lead & 0xe0) == 0xc0 && lead >= 0xc2) {
            length = 2;
        } else if ((lead & 0xf0) == 0xe0) {
            length = 3;
        } else if ((lead & 0xf8) == 0xf0 && lead <= 0xf4) {
            length = 4;
        } else {
            return false;
        }
        if (index + length > bytes.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset < length; ++offset) {
            if ((std::to_integer<unsigned char>(bytes[index + offset]) & 0xc0) != 0x80) {
                return false;
            }
        }
        index += length;
    }
    return true;
}

// Where one index keeps its files: a directory on lake's volume (a space's index) or the objects
// beside a workspace's store (lake#381).
class Storage {
public:
    virtual ~Storage() = default;
    virtual Result<std::optional<std::string>> read(const std::string& name) = 0;
    virtual Result<void> write(const std::string& name, const std::string& bytes) = 0;
    virtual Result<void> remove(const std::string& name) = 0;
    virtual Result<std::vector<std::string>> list() = 0;
    virtual Result<void> sync() = 0;
};

class DirectoryStorage final : public Storage {
public:
    explicit DirectoryStorage(std::filesystem::path directory) : directory_(std::move(directory)) {}

    Result<std::optional<std::string>> read(const std::string& name) override {
        std::ifstream input(directory_ / name, std::ios::binary);
        if (!input) {
            return std::optional<std::string>{};
        }
        return std::optional<std::string>{
            std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>())};
    }

    Result<void> write(const std::string& name, const std::string& bytes) override {
        const std::filesystem::path path = directory_ / name;
        std::error_code error;
        std::filesystem::create_directories(directory_, error);
        if (error) {
            return std::unexpected(
                Error{"store_error", "could not create index directory: " + error.message()});
        }
        static std::atomic<std::uint64_t> next_temporary{0};
        const std::filesystem::path temporary =
            path.string() + ".tmp-" + std::to_string(++next_temporary);
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                return std::unexpected(Error{"store_error", "could not write index file"});
            }
            output << bytes;
            output.flush();
            if (!output) {
                return std::unexpected(Error{"store_error", "could not finish index file"});
            }
        }
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary);
            return std::unexpected(
                Error{"store_error", "could not publish index file: " + error.message()});
        }
        return {};
    }

    Result<void> remove(const std::string& name) override {
        std::error_code ignored;
        std::filesystem::remove(directory_ / name, ignored);
        return {};
    }

    Result<std::vector<std::string>> list() override {
        std::vector<std::string> output;
        std::error_code error;
        if (!std::filesystem::exists(directory_, error)) {
            return output;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
            if (entry.is_regular_file() &&
                entry.path().filename().string().find(".tmp-") == std::string::npos) {
                output.push_back(entry.path().filename().string());
            }
        }
        if (error) {
            return std::unexpected(Error{"store_error", "could not list the index: " + error.message()});
        }
        return output;
    }

    Result<void> sync() override {
        return {};
    }

    // Removes the directory once the index holds nothing.
    void remove_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

private:
    std::filesystem::path directory_;
};

class ObjectStorage final : public Storage {
public:
    explicit ObjectStorage(std::shared_ptr<IndexObjects> objects) : objects_(std::move(objects)) {}

    Result<std::optional<std::string>> read(const std::string& name) override {
        return objects_->get(name);
    }
    Result<void> write(const std::string& name, const std::string& bytes) override {
        return objects_->put(name, bytes);
    }
    Result<void> remove(const std::string& name) override {
        return objects_->remove(name);
    }
    Result<std::vector<std::string>> list() override {
        return objects_->list();
    }
    Result<void> sync() override {
        return objects_->sync();
    }

private:
    std::shared_ptr<IndexObjects> objects_;
};

// A workspace's search index objects (lake#381): keys `search/<name>` in the workspace store's
// bucket `lake_store`, written through a view of the store's own ZettaObjectBackend, so they are
// sealed with the workspace's data key and every write carries the store's lease fence. Liquid
// never lists or writes under `search/`.
class StoreIndexObjects final : public IndexObjects {
public:
    StoreIndexObjects(std::shared_ptr<liquid::store_backend> backend, std::function<Result<void>()> sync)
        : backend_(std::move(backend)), sync_(std::move(sync)) {}

    Result<std::optional<std::string>> get(const std::string& name) override {
        const std::string key = std::string(workspace_index_prefix) + name;
        auto listed = liquid::runtime::block_on(backend_->list(key));
        if (!listed) {
            return std::unexpected(error_of(listed.error()));
        }
        const auto found = std::ranges::find(*listed, key, &liquid::store_backend_entry::key);
        if (found == listed->end()) {
            return std::optional<std::string>{};
        }
        auto bytes = liquid::runtime::block_on(backend_->get_range(key, 0, found->size));
        if (!bytes) {
            if (bytes.error().code == liquid::errc::not_found) {
                return std::optional<std::string>{};
            }
            return std::unexpected(error_of(bytes.error()));
        }
        return std::optional<std::string>{
            std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size())};
    }

    Result<void> put(const std::string& name, std::string bytes) override {
        const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
        auto written = liquid::runtime::block_on(backend_->put(
            std::string(workspace_index_prefix) + name, std::vector<std::byte>(data, data + bytes.size())));
        if (!written) {
            return std::unexpected(error_of(written.error()));
        }
        return {};
    }

    Result<void> remove(const std::string& name) override {
        auto removed = liquid::runtime::block_on(backend_->remove(std::string(workspace_index_prefix) + name));
        if (!removed && removed.error().code != liquid::errc::not_found) {
            return std::unexpected(error_of(removed.error()));
        }
        return {};
    }

    Result<std::vector<std::string>> list() override {
        auto listed = liquid::runtime::block_on(backend_->list(std::string(workspace_index_prefix)));
        if (!listed) {
            return std::unexpected(error_of(listed.error()));
        }
        std::vector<std::string> output;
        for (const liquid::store_backend_entry& entry : *listed) {
            if (entry.key.starts_with(workspace_index_prefix)) {
                output.push_back(entry.key.substr(workspace_index_prefix.size()));
            }
        }
        return output;
    }

    Result<void> sync() override {
        return sync_ ? sync_() : Result<void>{};
    }

private:
    static Error error_of(const liquid::error& error) {
        if (error.code == liquid::errc::locked) {
            return {"store_unavailable", error.message};
        }
        return {std::string(liquid::to_string(error.code)), error.message};
    }

    std::shared_ptr<liquid::store_backend> backend_;
    std::function<Result<void>()> sync_;
};

constexpr std::string_view blobs_file = "blobs.json";
constexpr std::string_view manifest_file = "manifest.json";
constexpr std::string_view artifacts_file = "artifacts.json";

} // namespace

std::string_view to_string(QueryMode mode) {
    switch (mode) {
    case QueryMode::Hot:
        return "hot";
    case QueryMode::Snapshot:
        return "snapshot";
    case QueryMode::Scan:
        return "scan";
    case QueryMode::Artifact:
        return "artifact";
    }
    return "hot";
}

struct Service::Impl {
    struct CustomerState {
        std::map<std::string, BlobRecord> blobs;
        std::map<std::string, Segment> active;
        std::map<std::string, ArtifactSegment> artifacts;
        std::unordered_map<std::string, std::vector<TrigramPosting>> trigram_index;
        std::vector<std::string> blob_documents;
        oramacore::index::MemoryIndex blob_index{{{"content", {1.0, 0.75}}}};
        // Blobs added since the catalogue was last written.
        bool blobs_added = false;
        // Approximate memory of the loaded index.
        std::size_t bytes = 0;
    };

    // One index: a space's on lake's volume, or a workspace's beside its store. Its mutex is taken
    // after any store lane and after Impl::mutex is released, never the other way round.
    struct IndexState {
        std::mutex mutex;
        bool loaded = false;
        std::uint64_t generation = 0;
        CustomerState data;
    };

    std::filesystem::path root;
    // Guards the maps and the configuration below, and nothing else: never held across I/O, a
    // store lane or an index's mutex wait.
    mutable std::mutex mutex;
    std::map<std::string, std::shared_ptr<IndexState>> spaces;
    std::map<std::string, std::shared_ptr<IndexState>> workspaces;
    std::map<std::string, std::uint64_t> last_used;
    std::uint64_t tick = 0;
    std::size_t memory_bound = 256ULL * 1024ULL * 1024ULL;
    RepositoryAccess access;
    // Once lake's repository store has lent repositories, search never opens a store itself.
    bool access_configured = false;
    WorkspaceAccess workspace_access;

    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}

    [[nodiscard]] WorkspaceAccess workspace_config() const {
        std::lock_guard lock(mutex);
        return workspace_access;
    }

    [[nodiscard]] std::shared_ptr<IndexState> space_state(const std::string& customer) {
        std::lock_guard lock(mutex);
        auto& slot = spaces[customer];
        if (!slot) {
            slot = std::make_shared<IndexState>();
        }
        return slot;
    }

    [[nodiscard]] std::shared_ptr<IndexState> workspace_state(const std::string& workspace) {
        std::lock_guard lock(mutex);
        auto& slot = workspaces[workspace];
        if (!slot) {
            slot = std::make_shared<IndexState>();
        }
        last_used[workspace] = ++tick;
        return slot;
    }

    [[nodiscard]] std::filesystem::path index_path(const std::string& customer) const {
        return root / customer / "index";
    }

    // The repository from lake's repository store when one is set; otherwise opened here, on a
    // store instance of its own.
    [[nodiscard]] Result<BorrowedRepository> borrow(const std::string& customer,
                                                    const std::string& repository) const {
        RepositoryAccess lend;
        bool configured = false;
        {
            std::lock_guard lock(mutex);
            lend = access;
            configured = access_configured;
        }
        if (lend) {
            return lend(customer, repository);
        }
        if (configured) {
            return std::unexpected(Error{
                "store_unavailable",
                "search reads repositories only through lake's repository store, which is closed"});
        }
        struct Owned {
            liquid::store store;
            liquid::repository repository;
        };
        auto store = liquid::runtime::block_on(liquid::store::open(root / customer / "store"));
        if (!store) {
            return std::unexpected(Error{"store_error", store.error().message});
        }
        auto owned = std::make_shared<Owned>(Owned{std::move(*store), liquid::repository{}});
        liquid::repository_options options;
        options.store = &owned->store;
        auto opened = liquid::runtime::block_on(
            liquid::repository::open(root / customer / "repositories" / repository, options));
        if (!opened) {
            return std::unexpected(Error{"store_error", opened.error().message});
        }
        owned->repository = std::move(*opened);
        return BorrowedRepository{owned, &owned->repository, std::nullopt};
    }

    [[nodiscard]] static std::string artifact_key(const ArtifactSegment& segment) {
        return segment.repository + "\n" + segment.version + "\n" + segment.artifact;
    }

    [[nodiscard]] static Result<void> append_artifact_members(const liquid::format_tree& tree,
                                                              const std::string& prefix,
                                                              const std::string& repository,
                                                              const std::string& artifact,
                                                              const std::string& version,
                                                              ArtifactSegment& segment,
                                                              CustomerState& state,
                                                              ReindexResult& result) {
        for (const liquid::format_entry& entry : tree.entries) {
            const std::string path = prefix + "!/" + entry.path;
            if (entry.kind != liquid::format_entry_kind::directory) {
                const std::string digest = liquid::to_string(entry.content);
                const bool indexed = !entry.bytes.empty() && is_indexable_text(entry.bytes);
                std::string reason = entry.opaque_reason;
                if (!indexed && reason.empty()) {
                    reason = entry.bytes.empty() ? "member bytes are unavailable" : "binary member";
                }
                if (indexed) {
                    const bool added =
                        state.blobs
                            .emplace(digest,
                                     BlobRecord{std::string(
                                         reinterpret_cast<const char*>(entry.bytes.data()),
                                         entry.bytes.size())})
                            .second;
                    state.blobs_added = state.blobs_added || added;
                    ++result.indexed;
                } else {
                    ++result.binary;
                }
                segment.members.push_back(Occurrence{.repository = repository,
                                                     .path = path,
                                                     .blob = digest,
                                                     .kind = PublicationKind::BranchTip,
                                                     .artifact = artifact,
                                                     .artifact_version = version,
                                                     .digest = digest,
                                                     .indexed = indexed,
                                                     .reason = std::move(reason)});
                ++result.members;
            }
            if (entry.nested) {
                const auto nested = append_artifact_members(
                    *entry.nested, path, repository, artifact, version, segment, state, result);
                if (!nested) {
                    return nested;
                }
            }
        }
        return {};
    }

    // Builds the in-memory search structures from the blobs and estimates their memory.
    static void build_memory(CustomerState& state) {
        state.trigram_index.clear();
        state.blob_documents.clear();
        state.blob_index = oramacore::index::MemoryIndex({{"content", {1.0, 0.75}}});
        std::size_t bytes = 0;
        for (const auto& [digest, blob] : state.blobs) {
            bytes += digest.size() + blob.content.size() * 6;
            const auto document = state.blob_index.add({{"content", blob.content}});
            if (state.blob_documents.size() <= document) {
                state.blob_documents.resize(document + 1);
            }
            state.blob_documents[document] = digest;
            for (std::size_t position = 0; position + 3 <= blob.content.size(); ++position) {
                const std::string trigram = blob.content.substr(position, 3);
                auto& postings = state.trigram_index[trigram];
                auto found = std::find_if(
                    postings.begin(), postings.end(), [&](const TrigramPosting& posting) {
                        return posting.blob == digest;
                    });
                if (found == postings.end()) {
                    postings.push_back(TrigramPosting{digest, {}});
                    found = std::prev(postings.end());
                }
                found->positions.push_back(position);
            }
        }
        for (const auto& [slot, segment] : state.active) {
            for (const Occurrence& occurrence : segment.occurrences) {
                bytes += 96 + occurrence.path.size();
            }
        }
        for (const auto& [key, segment] : state.artifacts) {
            for (const Occurrence& member : segment.members) {
                bytes += 128 + member.path.size();
            }
        }
        state.bytes = bytes;
    }

    // Walks a snapshot's tree. A file that is not text is not indexed.
    [[nodiscard]] static Result<Segment> collect_snapshot(liquid::repository& repository,
                                                          CustomerState& state,
                                                          const std::string& repository_id,
                                                          const std::string& ref,
                                                          const std::string& change,
                                                          PublicationKind kind,
                                                          const liquid::tree& root_tree) {
        Segment segment{.repository = repository_id, .ref = ref, .change = change, .kind = kind};
        const auto walk = [&](const auto& self,
                              const liquid::tree& tree,
                              const std::string& prefix) -> Result<void> {
            for (const auto& entry : tree.entries) {
                const std::string path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
                if (entry.kind == liquid::node_kind::tree) {
                    auto nested = liquid::runtime::block_on(repository.tree(entry.ref));
                    if (!nested) {
                        return std::unexpected(Error{"store_error", nested.error().message});
                    }
                    const auto nested_walk = self(self, *nested, path);
                    if (!nested_walk) {
                        return nested_walk;
                    }
                    continue;
                }
                if (entry.kind != liquid::node_kind::blob) {
                    continue;
                }
                const std::string digest = liquid::to_string(entry.ref);
                if (!state.blobs.contains(digest)) {
                    auto contents = liquid::runtime::block_on(repository.blob(entry.ref));
                    if (!contents) {
                        return std::unexpected(Error{"store_error", contents.error().message});
                    }
                    if (!is_indexable_text(*contents)) {
                        continue;
                    }
                    state.blobs.emplace(
                        digest,
                        BlobRecord{std::string(reinterpret_cast<const char*>(contents->data()),
                                               contents->size())});
                    state.blobs_added = true;
                }
                segment.occurrences.push_back(
                    Occurrence{repository_id, ref, change, path, digest, kind});
            }
            return {};
        };
        const auto walked = walk(walk, root_tree, "");
        if (!walked) {
            return std::unexpected(walked.error());
        }
        return segment;
    }

    [[nodiscard]] static std::string blobs_text(const CustomerState& state) {
        json blobs = json::object();
        for (const auto& [digest, record] : state.blobs) {
            blobs[digest] = {{"content", record.content}};
        }
        return blobs.dump();
    }

    [[nodiscard]] static std::string
    artifacts_text(const std::map<std::string, ArtifactSegment>& artifacts) {
        json value{{"version", 1}, {"artifacts", json::array()}};
        for (const auto& [key, segment] : artifacts) {
            json item{{"repository", segment.repository},
                      {"artifact", segment.artifact},
                      {"version", segment.version},
                      {"members", json::array()}};
            for (const Occurrence& member : segment.members) {
                item["members"].push_back({{"path", member.path},
                                           {"digest", member.digest},
                                           {"indexed", member.indexed},
                                           {"reason", member.reason}});
            }
            value["artifacts"].push_back(std::move(item));
        }
        return value.dump();
    }

    [[nodiscard]] static std::string manifest_text(const std::map<std::string, Segment>& active) {
        json manifest{{"version", 2}, {"active", json::array()}};
        for (const auto& [slot, segment] : active) {
            manifest["active"].push_back({{"repository", segment.repository},
                                          {"ref", segment.ref},
                                          {"change", segment.change},
                                          {"kind", publication_kind_name(segment.kind)},
                                          {"segment", segment.file}});
        }
        return manifest.dump();
    }

    [[nodiscard]] static std::string segment_text(const Segment& segment) {
        json value{{"version", 2},
                   {"repository", segment.repository},
                   {"ref", segment.ref},
                   {"change", segment.change},
                   {"kind", publication_kind_name(segment.kind)},
                   {"occurrences", json::array()}};
        for (const Occurrence& occurrence : segment.occurrences) {
            value["occurrences"].push_back({{"path", occurrence.path}, {"blob", occurrence.blob}});
        }
        return value.dump();
    }

    // Publishes the next state of an index: new segments and the blobs they add first, then the
    // manifest (one atomic replacement), then what nothing references any more. A failure before
    // the manifest leaves the previous state whole. `sweep` also removes every other object of the
    // index (a rebuild).
    [[nodiscard]] static Result<void> commit(Storage& storage,
                                             CustomerState& state,
                                             std::map<std::string, Segment> next_active,
                                             std::map<std::string, ArtifactSegment> next_artifacts,
                                             bool artifacts_changed,
                                             bool sweep) {
        std::set<std::string> keep{std::string(blobs_file), std::string(manifest_file),
                                   std::string(artifacts_file)};
        for (auto& [slot, segment] : next_active) {
            if (segment.file.empty()) {
                segment.file = segment_name(segment);
                const auto written = storage.write(segment.file, segment_text(segment));
                if (!written) {
                    return written;
                }
            }
            keep.insert(segment.file);
        }
        if (state.blobs_added || sweep) {
            const auto written = storage.write(std::string(blobs_file), blobs_text(state));
            if (!written) {
                return written;
            }
        }
        if (artifacts_changed || sweep) {
            const auto written = storage.write(std::string(artifacts_file), artifacts_text(next_artifacts));
            if (!written) {
                return written;
            }
        }
        if (const auto synced = storage.sync(); !synced) {
            return synced;
        }
        if (const auto published = storage.write(std::string(manifest_file), manifest_text(next_active));
            !published) {
            return published;
        }
        if (const auto synced = storage.sync(); !synced) {
            return synced;
        }
        state.blobs_added = false;
        std::set<std::string> dropped;
        for (const auto& [slot, segment] : state.active) {
            if (!keep.contains(segment.file)) {
                dropped.insert(segment.file);
            }
        }
        state.active = std::move(next_active);
        state.artifacts = std::move(next_artifacts);
        // Blobs nothing references any more go after the manifest stops naming them.
        std::set<std::string> referenced;
        for (const auto& [slot, segment] : state.active) {
            for (const Occurrence& occurrence : segment.occurrences) {
                referenced.insert(occurrence.blob);
            }
        }
        for (const auto& [key, artifact] : state.artifacts) {
            for (const Occurrence& member : artifact.members) {
                referenced.insert(member.blob);
            }
        }
        const std::size_t before = state.blobs.size();
        std::erase_if(state.blobs, [&](const auto& item) { return !referenced.contains(item.first); });
        if (state.blobs.size() != before) {
            const auto written = storage.write(std::string(blobs_file), blobs_text(state));
            if (!written) {
                state.blobs_added = true;
            }
        }
        build_memory(state);
        if (sweep) {
            const auto listed = storage.list();
            if (listed) {
                for (const std::string& name : *listed) {
                    if (!keep.contains(name)) {
                        dropped.insert(name);
                    }
                }
            }
        }
        for (const std::string& file : dropped) {
            static_cast<void>(storage.remove(file));
        }
        return {};
    }

    // Reads an index. False when it has no manifest (never written, or wiped).
    [[nodiscard]] static Result<bool> load_data(Storage& storage, CustomerState& state) {
        const auto blob_value = storage.read(std::string(blobs_file));
        if (!blob_value) {
            return std::unexpected(blob_value.error());
        }
        if (*blob_value) {
            const json blobs = json::parse(**blob_value);
            for (auto item = blobs.begin(); item != blobs.end(); ++item) {
                state.blobs.emplace(item.key(),
                                    BlobRecord{item.value().at("content").get<std::string>()});
            }
        }
        const auto manifest_value = storage.read(std::string(manifest_file));
        if (!manifest_value) {
            return std::unexpected(manifest_value.error());
        }
        if (*manifest_value) {
            const json manifest = json::parse(**manifest_value);
            for (const json& active : manifest.at("active")) {
                const std::string file = active.at("segment").get<std::string>();
                const auto segment_bytes = storage.read(file);
                if (!segment_bytes) {
                    return std::unexpected(segment_bytes.error());
                }
                if (!*segment_bytes) {
                    return std::unexpected(
                        Error{"not_found", "index segment " + file + " named by the manifest is absent"});
                }
                const json segment_value = json::parse(**segment_bytes);
                Segment segment;
                segment.repository = segment_value.at("repository").get<std::string>();
                segment.ref = segment_value.at("ref").get<std::string>();
                segment.change = segment_value.at("change").get<std::string>();
                segment.kind = publication_kind(segment_value);
                segment.file = file;
                for (const json& value : segment_value.at("occurrences")) {
                    segment.occurrences.push_back(Occurrence{segment.repository,
                                                             segment.ref,
                                                             segment.change,
                                                             value.at("path").get<std::string>(),
                                                             value.at("blob").get<std::string>(),
                                                             segment.kind});
                }
                state.active.emplace(slot_key(segment.kind, segment.repository, segment.ref),
                                     std::move(segment));
            }
        }
        const auto artifacts_value = storage.read(std::string(artifacts_file));
        if (!artifacts_value) {
            return std::unexpected(artifacts_value.error());
        }
        if (*artifacts_value) {
            const json artifacts = json::parse(**artifacts_value);
            for (const json& value : artifacts.value("artifacts", json::array())) {
                ArtifactSegment segment{.repository = value.at("repository").get<std::string>(),
                                        .artifact = value.at("artifact").get<std::string>(),
                                        .version = value.at("version").get<std::string>()};
                for (const json& member : value.value("members", json::array())) {
                    const std::string digest = member.at("digest").get<std::string>();
                    segment.members.push_back(
                        Occurrence{.repository = segment.repository,
                                   .path = member.at("path").get<std::string>(),
                                   .blob = digest,
                                   .kind = PublicationKind::BranchTip,
                                   .artifact = segment.artifact,
                                   .artifact_version = segment.version,
                                   .digest = digest,
                                   .indexed = member.value("indexed", false),
                                   .reason = member.value("reason", "")});
                }
                state.artifacts.emplace(artifact_key(segment), std::move(segment));
            }
        }
        build_memory(state);
        return manifest_value->has_value();
    }

    // Replaces the slot of one publication in `next_active`. A tag snapshot never moves.
    [[nodiscard]] static Result<bool> publish_into(liquid::repository& opened,
                                                   CustomerState& state,
                                                   const Publish& publication,
                                                   std::map<std::string, Segment>& next_active) {
        const std::string key = slot_key(publication.kind, publication.repository, publication.ref);
        const auto existing = next_active.find(key);
        if (publication.kind == PublicationKind::TagSnapshot && existing != next_active.end()) {
            if (existing->second.change == publication.change) {
                return false;
            }
            return std::unexpected(
                Error{"conflict", "a tag snapshot cannot move to another change"});
        }
        const auto parsed = liquid::parse_digest(publication.change);
        if (!parsed) {
            return std::unexpected(Error{"invalid_input", parsed.error().message});
        }
        auto snapshot = liquid::runtime::block_on(opened.at(liquid::change_id{*parsed}));
        if (!snapshot) {
            return std::unexpected(Error{"not_found", snapshot.error().message});
        }
        auto segment = collect_snapshot(opened,
                                        state,
                                        publication.repository,
                                        publication.ref,
                                        publication.change,
                                        publication.kind,
                                        snapshot->root);
        if (!segment) {
            return std::unexpected(segment.error());
        }
        next_active[key] = std::move(*segment);
        return true;
    }

    // A workspace repository's branch tips as its references name them now: a reference that
    // moved is published again, one that is gone is dropped, tag snapshots are kept.
    [[nodiscard]] static Result<void> publish_refs(liquid::repository& opened,
                                                   CustomerState& state,
                                                   const std::string& repository,
                                                   std::map<std::string, Segment>& next_active) {
        const auto refs = liquid::runtime::block_on(opened.refs());
        if (!refs) {
            return std::unexpected(Error{"store_error", refs.error().message});
        }
        std::set<std::string> current;
        for (const liquid::ref& ref : *refs) {
            const std::string change = liquid::to_string(ref.id.value);
            const std::string key = slot_key(PublicationKind::BranchTip, repository, ref.name);
            current.insert(key);
            if (const auto found = next_active.find(key);
                found != next_active.end() && found->second.change == change) {
                continue;
            }
            const auto published = publish_into(
                opened, state, Publish{"", repository, ref.name, change, PublicationKind::BranchTip},
                next_active);
            if (!published) {
                return std::unexpected(published.error());
            }
        }
        std::erase_if(next_active, [&](const auto& item) {
            return item.second.repository == repository &&
                   item.second.kind == PublicationKind::BranchTip && !current.contains(item.first);
        });
        return {};
    }

    // Decomposes every published artifact of one repository into `next_artifacts`.
    [[nodiscard]] static Result<ReindexResult>
    reindex_into(liquid::repository& opened,
                 CustomerState& state,
                 const std::string& repository,
                 std::map<std::string, ArtifactSegment>& next_artifacts) {
        std::erase_if(next_artifacts,
                      [&](const auto& item) { return item.second.repository == repository; });
        ReindexResult result;
        liquid::record_page_options page{.limit = 1'000};
        do {
            const auto records = liquid::runtime::block_on(opened.results({.page = page}));
            if (!records) {
                return std::unexpected(Error{"store_error", records.error().message});
            }
            for (const liquid::repository_result& record : records->records) {
                const std::string version = liquid::to_string(record.id.value);
                for (const liquid::action_output& output : record.value.outputs) {
                    ArtifactSegment segment{
                        .repository = repository, .artifact = output.path, .version = version};
                    ++result.artifacts;
                    const auto contents = liquid::runtime::block_on(opened.blob(output.blob));
                    if (!contents) {
                        return std::unexpected(Error{"store_error", contents.error().message});
                    }
                    const auto tree = liquid::format_registry::global().unpack(
                        *contents,
                        output.path,
                        {.maximum_depth = 2,
                         .byte_budget = 256ULL * 1024ULL * 1024ULL,
                         .maximum_entries = 100'000});
                    if (!tree) {
                        if (tree.error().code != liquid::errc::not_found) {
                            return std::unexpected(Error{"store_error", tree.error().message});
                        }
                    } else {
                        if (tree->truncated) {
                            ++result.truncated;
                        }
                        const auto appended = append_artifact_members(
                            *tree, output.path, repository, output.path, version, segment, state,
                            result);
                        if (!appended) {
                            return std::unexpected(appended.error());
                        }
                    }
                    next_artifacts.insert_or_assign(artifact_key(segment), std::move(segment));
                }
            }
            page.cursor = records->next;
            if (records->done) {
                break;
            }
        } while (page.cursor);
        return result;
    }

    // Rebuilds a workspace's index from its repositories (lake#381: rebuilt, never copied).
    // The caller holds the workspace's lane (through `index`) and the state's mutex.
    [[nodiscard]] static Result<void> rebuild_workspace(IndexState& state, const WorkspaceIndex& index) {
        if (!index.repositories || !index.open) {
            return std::unexpected(
                Error{"store_unavailable", "the workspace's repositories cannot be read to rebuild its index"});
        }
        const auto repositories = index.repositories();
        if (!repositories) {
            return std::unexpected(repositories.error());
        }
        CustomerState fresh;
        std::map<std::string, Segment> active;
        std::map<std::string, ArtifactSegment> artifacts;
        for (const std::string& repository : *repositories) {
            auto opened = index.open(repository);
            if (!opened && opened.error().code == "not_found") {
                continue;
            }
            if (!opened) {
                return std::unexpected(Error{opened.error().code,
                                             "could not rebuild the search index of repository " +
                                                 repository + ": " + opened.error().message});
            }
            if (const auto published = publish_refs(**opened, fresh, repository, active); !published) {
                return std::unexpected(published.error());
            }
            if (const auto reindexed = reindex_into(**opened, fresh, repository, artifacts); !reindexed) {
                return std::unexpected(reindexed.error());
            }
        }
        ObjectStorage storage(index.objects);
        if (const auto committed =
                commit(storage, fresh, std::move(active), std::move(artifacts), true, true);
            !committed) {
            return committed;
        }
        state.data = std::move(fresh);
        state.loaded = true;
        state.generation = index.generation;
        return {};
    }

    // Loads a workspace's index on its first use, and again when its store is owned under another
    // fence (another process may have written it since). An index without a manifest (never
    // written, or wiped), one missing a segment its manifest names, or one that does not read back
    // (`corrupt`, unparseable) is rebuilt from the repositories, never served empty: it is derived
    // data. A store that cannot be read is an error.
    [[nodiscard]] static Result<void> ensure_workspace(IndexState& state, const WorkspaceIndex& index) {
        if (state.loaded && state.generation == index.generation) {
            return {};
        }
        state.loaded = false;
        state.data = CustomerState{};
        ObjectStorage storage(index.objects);
        Result<bool> loaded = false;
        try {
            loaded = load_data(storage, state.data);
        } catch (const json::exception& error) {
            loaded = std::unexpected(Error{"corrupt", error.what()});
        }
        if (!loaded && loaded.error().code != "not_found" && loaded.error().code != "corrupt") {
            return std::unexpected(loaded.error());
        }
        if (!loaded || !*loaded) {
            state.data = CustomerState{};
            return rebuild_workspace(state, index);
        }
        state.loaded = true;
        state.generation = index.generation;
        return {};
    }

    // Drops from a space's index every repository a workspace store serves now, and the blobs
    // no segment references (history scans keep them; they cannot be told apart by repository),
    // so the index on lake's volume keeps nothing of a switched workspace (lake#381). A repository
    // whose route cannot be read is kept, and left out of results. Answers how many repositories
    // were dropped.
    [[nodiscard]] Result<std::size_t> purge_switched(const std::string& customer, IndexState& state) {
        const WorkspaceAccess config = workspace_config();
        if (!config.route) {
            return std::size_t{0};
        }
        std::set<std::string> repositories;
        std::set<std::string> referenced;
        for (const auto& [slot, segment] : state.data.active) {
            repositories.insert(segment.repository);
            for (const Occurrence& occurrence : segment.occurrences) {
                referenced.insert(occurrence.blob);
            }
        }
        for (const auto& [key, artifact] : state.data.artifacts) {
            repositories.insert(artifact.repository);
            for (const Occurrence& member : artifact.members) {
                referenced.insert(member.blob);
            }
        }
        std::set<std::string> moved;
        for (const std::string& repository : repositories) {
            const auto route = config.route(customer, repository);
            if (route && route->has_value()) {
                moved.insert(repository);
            }
        }
        const bool unreferenced = std::ranges::any_of(
            state.data.blobs, [&](const auto& item) { return !referenced.contains(item.first); });
        if (moved.empty() && !unreferenced) {
            return std::size_t{0};
        }
        auto next_active = state.data.active;
        std::erase_if(next_active, [&](const auto& item) { return moved.contains(item.second.repository); });
        auto next_artifacts = state.data.artifacts;
        std::erase_if(next_artifacts,
                      [&](const auto& item) { return moved.contains(item.second.repository); });
        DirectoryStorage storage(index_path(customer));
        if (const auto committed =
                commit(storage, state.data, std::move(next_active), std::move(next_artifacts), true, false);
            !committed) {
            return std::unexpected(committed.error());
        }
        if (state.data.active.empty() && state.data.artifacts.empty()) {
            storage.remove_directory();
        }
        return moved.size();
    }

    // Loads a space's index from lake's volume; the caller holds the state's mutex.
    [[nodiscard]] Result<void> ensure_space(const std::string& customer, IndexState& state) {
        if (state.loaded) {
            return {};
        }
        state.data = CustomerState{};
        std::error_code error;
        if (std::filesystem::exists(index_path(customer), error)) {
            DirectoryStorage storage(index_path(customer));
            const auto loaded = load_data(storage, state.data);
            if (!loaded) {
                return std::unexpected(loaded.error());
            }
        }
        state.loaded = true;
        const auto purged = purge_switched(customer, state);
        if (!purged) {
            return std::unexpected(purged.error());
        }
        return {};
    }

    // Evicts idle workspace indexes, least recently used first, while those loaded exceed the
    // bound. One in use (its mutex held) and the one used last are kept.
    void trim() {
        std::vector<std::shared_ptr<IndexState>> evicted;
        std::lock_guard lock(mutex);
        std::size_t total = 0;
        for (const auto& [workspace, state] : workspaces) {
            std::unique_lock held(state->mutex, std::try_to_lock);
            total += held ? state->data.bytes : 0;
        }
        std::vector<std::pair<std::uint64_t, std::string>> order;
        for (const auto& [workspace, used] : last_used) {
            order.emplace_back(used, workspace);
        }
        std::ranges::sort(order);
        if (!order.empty()) {
            order.pop_back();
        }
        for (const auto& [used, workspace] : order) {
            if (total <= memory_bound) {
                break;
            }
            const auto found = workspaces.find(workspace);
            if (found == workspaces.end()) {
                continue;
            }
            std::unique_lock held(found->second->mutex, std::try_to_lock);
            if (!held) {
                continue;
            }
            total -= std::min(total, found->second->data.bytes);
            held.unlock();
            evicted.push_back(std::move(found->second));
            workspaces.erase(found);
            last_used.erase(workspace);
        }
    }

    struct ScanOutput {
        std::vector<Occurrence> occurrences;
        std::size_t changes = 0;
    };

    // `opened` was borrowed before any index's mutex was taken, so search never waits on a
    // store's lane while holding one.
    [[nodiscard]] static Result<ScanOutput>
    scan(const Query& query, CustomerState& state, liquid::repository* opened, Storage& storage) {
        std::vector<liquid::change_id> changes;
        if (!query.change.empty()) {
            const auto parsed = liquid::parse_digest(query.change);
            if (!parsed) {
                return std::unexpected(Error{"invalid_input", parsed.error().message});
            }
            changes.push_back(liquid::change_id{*parsed});
        } else {
            const auto history = liquid::runtime::block_on(opened->log());
            if (!history) {
                return std::unexpected(Error{"store_error", history.error().message});
            }
            changes.reserve(history->size());
            for (const liquid::change& change : *history) {
                changes.push_back(change.id);
            }
        }
        ScanOutput output;
        output.changes = changes.size();
        for (const liquid::change_id& id : changes) {
            auto snapshot = liquid::runtime::block_on(opened->at(id));
            if (!snapshot) {
                return std::unexpected(Error{"not_found", snapshot.error().message});
            }
            const std::string change = liquid::to_string(id.value);
            auto segment = collect_snapshot(*opened,
                                            state,
                                            query.repository,
                                            "",
                                            change,
                                            PublicationKind::BranchTip,
                                            snapshot->root);
            if (!segment) {
                return std::unexpected(segment.error());
            }
            output.occurrences.insert(output.occurrences.end(),
                                      std::make_move_iterator(segment->occurrences.begin()),
                                      std::make_move_iterator(segment->occurrences.end()));
        }
        if (state.blobs_added) {
            const auto saved = storage.write(std::string(blobs_file), blobs_text(state));
            if (!saved) {
                return std::unexpected(saved.error());
            }
            state.blobs_added = false;
            build_memory(state);
        }
        return output;
    }

    struct Matcher {
        std::string text;
        std::optional<ArtifactScope> artifact_scope;
        bool scans_history = false;
        bool is_regex = false;
        bool is_substring = false;
        std::string needle;
        std::optional<std::regex> expression;
    };

    // The hits of one index, not yet authorized. `skip` leaves a repository out.
    static void collect(const Query& query,
                        const Matcher& matcher,
                        const std::string& customer,
                        const std::string& workspace,
                        const CustomerState& state,
                        const std::vector<Occurrence>* scanned,
                        const std::function<bool(const std::string&)>& skip,
                        QueryResult& output,
                        std::vector<Hit>& hits) {
        std::vector<Occurrence> selected;
        if (matcher.artifact_scope) {
            for (const auto& [key, artifact] : state.artifacts) {
                if (skip(artifact.repository)) {
                    continue;
                }
                if ((!query.repository.empty() && query.repository != artifact.repository) ||
                    matcher.artifact_scope->name != artifact.artifact ||
                    (!matcher.artifact_scope->version.empty() &&
                     matcher.artifact_scope->version != artifact.version)) {
                    continue;
                }
                selected.insert(selected.end(), artifact.members.begin(), artifact.members.end());
            }
        } else if (scanned != nullptr) {
            selected = *scanned;
        } else {
            for (const auto& [slot, segment] : state.active) {
                if (!query.repository.empty() && query.repository != segment.repository) {
                    continue;
                }
                if (skip(segment.repository)) {
                    continue;
                }
                if (query.ref.empty()) {
                    if (segment.kind != PublicationKind::BranchTip) {
                        continue;
                    }
                } else {
                    if (query.ref != segment.ref) {
                        continue;
                    }
                    const bool tag_matches = state.active.contains(
                        slot_key(PublicationKind::TagSnapshot, segment.repository, query.ref));
                    if (tag_matches && segment.kind != PublicationKind::TagSnapshot) {
                        continue;
                    }
                }
                if (segment.kind == PublicationKind::TagSnapshot) {
                    output.mode = QueryMode::Snapshot;
                }
                selected.insert(
                    selected.end(), segment.occurrences.begin(), segment.occurrences.end());
            }
        }
        const std::string& needle = matcher.needle;
        std::unordered_map<std::string, double> scores;
        if (!matcher.is_regex && !matcher.is_substring) {
            for (const auto& result : state.blob_index.search(needle, 0).hits) {
                if (result.doc_id < state.blob_documents.size()) {
                    scores[state.blob_documents[result.doc_id]] += result.score;
                }
            }
            oramacore::index::MemoryIndex path_index({{"path", {3.0, 0.75}}});
            std::vector<std::string> path_documents;
            for (const Occurrence& occurrence : selected) {
                const auto document = path_index.add({{"path", occurrence.path}});
                if (path_documents.size() <= document) {
                    path_documents.resize(document + 1);
                }
                path_documents[document] = occurrence.blob;
            }
            for (const auto& result : path_index.search(needle, 0).hits) {
                if (result.doc_id < path_documents.size()) {
                    scores[path_documents[result.doc_id]] += result.score;
                }
            }
        } else {
            const std::string prefilter = matcher.is_regex ? regex_literal(needle) : needle;
            std::unordered_set<std::string> candidates;
            if (prefilter.size() < 3) {
                for (const auto& [digest, record] : state.blobs) {
                    candidates.insert(digest);
                }
            } else {
                const std::vector<std::string> keys = trigrams(prefilter);
                if (!keys.empty()) {
                    const auto initial = state.trigram_index.find(keys.front());
                    if (initial != state.trigram_index.end()) {
                        for (const TrigramPosting& posting : initial->second) {
                            candidates.insert(posting.blob);
                        }
                    }
                    for (std::size_t index = 1; index < keys.size(); ++index) {
                        const auto postings = state.trigram_index.find(keys[index]);
                        if (postings == state.trigram_index.end()) {
                            candidates.clear();
                            break;
                        }
                        std::unordered_set<std::string> next;
                        for (const TrigramPosting& posting : postings->second) {
                            if (candidates.contains(posting.blob)) {
                                next.insert(posting.blob);
                            }
                        }
                        candidates = std::move(next);
                    }
                    std::erase_if(candidates, [&](const std::string& digest) {
                        return !has_contiguous_positions(state.trigram_index, keys, digest);
                    });
                }
            }
            for (const std::string& digest : candidates) {
                const auto blob = state.blobs.find(digest);
                if (blob == state.blobs.end()) {
                    continue;
                }
                const bool matched =
                    matcher.is_regex ? std::regex_search(blob->second.content, *matcher.expression)
                                     : blob->second.content.find(needle) != std::string::npos;
                if (matched) {
                    scores[digest] = 1.0;
                }
            }
        }
        for (const Occurrence& occurrence : selected) {
            const auto score = scores.find(occurrence.blob);
            if (score == scores.end()) {
                continue;
            }
            const auto blob = state.blobs.find(occurrence.blob);
            if (blob == state.blobs.end()) {
                continue;
            }
            hits.push_back(Hit{customer,
                               occurrence.repository,
                               occurrence.path,
                               occurrence.change,
                               snippet_for(blob->second.content, needle),
                               score->second,
                               occurrence.artifact,
                               occurrence.artifact_version,
                               occurrence.digest,
                               line_for(blob->second.content, needle, matcher.expression),
                               workspace});
        }
    }

    [[nodiscard]] std::vector<std::string> space_directories() const {
        std::vector<std::string> output;
        std::error_code error;
        if (!std::filesystem::exists(root, error)) {
            return output;
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (entry.is_directory() && std::filesystem::exists(entry.path() / "index", error)) {
                output.push_back(entry.path().filename().string());
            }
        }
        return output;
    }
};

std::shared_ptr<IndexObjects> make_store_index_objects(std::shared_ptr<liquid::store_backend> backend,
                                                       std::function<Result<void>()> sync) {
    return std::make_shared<StoreIndexObjects>(std::move(backend), std::move(sync));
}

Service::Service(std::filesystem::path store_root)
    : impl_(std::make_shared<Impl>(std::move(store_root))) {}

void Service::set_repository_access(RepositoryAccess access) {
    std::lock_guard lock(impl_->mutex);
    impl_->access_configured = impl_->access_configured || static_cast<bool>(access);
    impl_->access = std::move(access);
}

void Service::set_workspace_access(WorkspaceAccess access) {
    std::lock_guard lock(impl_->mutex);
    impl_->workspace_access = std::move(access);
}

void Service::set_memory_bound(std::size_t bytes) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->memory_bound = bytes;
    }
    impl_->trim();
}

std::size_t Service::workspace_memory() const {
    std::vector<std::shared_ptr<Impl::IndexState>> states;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& [workspace, state] : impl_->workspaces) {
            states.push_back(state);
        }
    }
    std::size_t total = 0;
    for (const auto& state : states) {
        std::lock_guard held(state->mutex);
        total += state->loaded ? state->data.bytes : 0;
    }
    return total;
}

std::vector<std::string> Service::loaded_workspaces() const {
    std::vector<std::pair<std::string, std::shared_ptr<Impl::IndexState>>> states;
    {
        std::lock_guard lock(impl_->mutex);
        for (const auto& [workspace, state] : impl_->workspaces) {
            states.emplace_back(workspace, state);
        }
    }
    std::vector<std::string> output;
    for (const auto& [workspace, state] : states) {
        std::lock_guard held(state->mutex);
        if (state->loaded) {
            output.push_back(workspace);
        }
    }
    return output;
}

Result<void> Service::publish(const Publish& publication) {
    try {
        if (publication.customer.empty() || publication.repository.empty() ||
            publication.ref.empty() || publication.change.empty()) {
            return std::unexpected(
                Error{"invalid_input", "customer, repository, ref and change are required"});
        }
        auto borrowed = impl_->borrow(publication.customer, publication.repository);
        if (!borrowed) {
            return std::unexpected(borrowed.error());
        }
        liquid::repository* opened = borrowed->repository;
        if (borrowed->workspace) {
            const WorkspaceIndex& index = *borrowed->workspace;
            const auto state = impl_->workspace_state(index.workspace);
            {
                std::lock_guard held(state->mutex);
                if (const auto ensured = Impl::ensure_workspace(*state, index); !ensured) {
                    return ensured;
                }
                auto next_active = state->data.active;
                const auto changed = Impl::publish_into(*opened, state->data, publication, next_active);
                if (!changed) {
                    return std::unexpected(changed.error());
                }
                if (*changed) {
                    ObjectStorage storage(index.objects);
                    if (const auto committed = Impl::commit(storage, state->data, std::move(next_active),
                                                            state->data.artifacts, false, false);
                        !committed) {
                        state->loaded = false;
                        return committed;
                    }
                }
            }
            impl_->trim();
            // What the space's index held of it from before the switch goes.
            return drop_repository(publication.customer, publication.repository);
        }
        const auto state = impl_->space_state(publication.customer);
        std::lock_guard held(state->mutex);
        if (const auto ensured = impl_->ensure_space(publication.customer, *state); !ensured) {
            return ensured;
        }
        auto next_active = state->data.active;
        const auto changed = Impl::publish_into(*opened, state->data, publication, next_active);
        if (!changed) {
            return std::unexpected(changed.error());
        }
        if (!*changed) {
            return {};
        }
        DirectoryStorage storage(impl_->index_path(publication.customer));
        return Impl::commit(storage, state->data, std::move(next_active), state->data.artifacts,
                            false, false);
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<void> Service::drop_repository(const std::string& customer, const std::string& repository) {
    try {
        if (customer.empty() || repository.empty()) {
            return std::unexpected(Error{"invalid_input", "customer and repository are required"});
        }
        const auto state = impl_->space_state(customer);
        std::lock_guard held(state->mutex);
        if (const auto ensured = impl_->ensure_space(customer, *state); !ensured) {
            return ensured;
        }
        auto next_active = state->data.active;
        std::erase_if(next_active,
                      [&](const auto& item) { return item.second.repository == repository; });
        auto next_artifacts = state->data.artifacts;
        std::erase_if(next_artifacts,
                      [&](const auto& item) { return item.second.repository == repository; });
        if (next_active.size() == state->data.active.size() &&
            next_artifacts.size() == state->data.artifacts.size()) {
            return {};
        }
        DirectoryStorage storage(impl_->index_path(customer));
        if (const auto committed = Impl::commit(storage, state->data, std::move(next_active),
                                                std::move(next_artifacts), true, false);
            !committed) {
            return committed;
        }
        if (state->data.active.empty() && state->data.artifacts.empty()) {
            storage.remove_directory();
        }
        return {};
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<std::size_t> Service::drop_switched(const std::string& customer) {
    try {
        if (customer.empty()) {
            return std::unexpected(Error{"invalid_input", "customer is required"});
        }
        const auto state = impl_->space_state(customer);
        std::lock_guard held(state->mutex);
        if (!state->loaded) {
            state->data = Impl::CustomerState{};
            std::error_code error;
            if (std::filesystem::exists(impl_->index_path(customer), error)) {
                DirectoryStorage storage(impl_->index_path(customer));
                const auto loaded = Impl::load_data(storage, state->data);
                if (!loaded) {
                    return std::unexpected(loaded.error());
                }
            }
            state->loaded = true;
        }
        return impl_->purge_switched(customer, *state);
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<void> Service::drop_workspace(const std::string& workspace) {
    try {
        if (workspace.empty()) {
            return std::unexpected(Error{"invalid_input", "workspace is required"});
        }
        const WorkspaceAccess config = impl_->workspace_config();
        std::optional<BorrowedWorkspace> borrowed;
        if (config.lend_if_open) {
            auto lent = config.lend_if_open(workspace);
            if (!lent) {
                return std::unexpected(lent.error());
            }
            borrowed = std::move(*lent);
        }
        std::shared_ptr<Impl::IndexState> state;
        {
            std::lock_guard lock(impl_->mutex);
            if (const auto found = impl_->workspaces.find(workspace); found != impl_->workspaces.end()) {
                state = found->second;
                impl_->workspaces.erase(found);
            }
            impl_->last_used.erase(workspace);
        }
        if (state) {
            std::lock_guard held(state->mutex);
            state->loaded = false;
            state->data = Impl::CustomerState{};
        }
        if (!borrowed) {
            return {};
        }
        const auto listed = borrowed->index.objects->list();
        if (!listed) {
            return std::unexpected(listed.error());
        }
        for (const std::string& name : *listed) {
            if (const auto removed = borrowed->index.objects->remove(name); !removed) {
                return removed;
            }
        }
        return borrowed->index.objects->sync();
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<void> Service::rebuild_workspace(const std::string& customer, const std::string& workspace) {
    try {
        const WorkspaceAccess config = impl_->workspace_config();
        if (!config.lend) {
            return std::unexpected(Error{"unsupported", "workspace indexes are not configured"});
        }
        auto borrowed = config.lend(customer, workspace);
        if (!borrowed) {
            return std::unexpected(borrowed.error());
        }
        const auto state = impl_->workspace_state(workspace);
        {
            std::lock_guard held(state->mutex);
            state->loaded = false;
            state->data = Impl::CustomerState{};
            if (const auto rebuilt = Impl::rebuild_workspace(*state, borrowed->index); !rebuilt) {
                return rebuilt;
            }
        }
        impl_->trim();
        return {};
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<ReindexResult> Service::reindex_artifacts(const std::string& customer,
                                                 const std::string& repository) {
    try {
        if (customer.empty() || repository.empty()) {
            return std::unexpected(Error{"invalid_input", "customer and repository are required"});
        }
        auto borrowed = impl_->borrow(customer, repository);
        if (!borrowed) {
            return std::unexpected(borrowed.error());
        }
        liquid::repository* opened = borrowed->repository;
        if (borrowed->workspace) {
            // In a workspace's index, reindexing a repository also publishes its branch tips again
            // as its references name them now, so the index follows every push (lake#381).
            const WorkspaceIndex& index = *borrowed->workspace;
            const auto state = impl_->workspace_state(index.workspace);
            ReindexResult result;
            {
                std::lock_guard held(state->mutex);
                if (const auto ensured = Impl::ensure_workspace(*state, index); !ensured) {
                    return std::unexpected(ensured.error());
                }
                auto next_active = state->data.active;
                if (const auto published =
                        Impl::publish_refs(*opened, state->data, repository, next_active);
                    !published) {
                    return std::unexpected(published.error());
                }
                auto next_artifacts = state->data.artifacts;
                auto reindexed = Impl::reindex_into(*opened, state->data, repository, next_artifacts);
                if (!reindexed) {
                    return std::unexpected(reindexed.error());
                }
                result = *reindexed;
                ObjectStorage storage(index.objects);
                if (const auto committed = Impl::commit(storage, state->data, std::move(next_active),
                                                        std::move(next_artifacts), true, false);
                    !committed) {
                    state->loaded = false;
                    return std::unexpected(committed.error());
                }
            }
            impl_->trim();
            if (const auto dropped = drop_repository(customer, repository); !dropped) {
                return std::unexpected(dropped.error());
            }
            return result;
        }
        const auto state = impl_->space_state(customer);
        std::lock_guard held(state->mutex);
        if (const auto ensured = impl_->ensure_space(customer, *state); !ensured) {
            return std::unexpected(ensured.error());
        }
        auto next_artifacts = state->data.artifacts;
        auto result = Impl::reindex_into(*opened, state->data, repository, next_artifacts);
        if (!result) {
            return std::unexpected(result.error());
        }
        DirectoryStorage storage(impl_->index_path(customer));
        if (const auto committed = Impl::commit(storage, state->data, state->data.active,
                                                std::move(next_artifacts), true, false);
            !committed) {
            return std::unexpected(committed.error());
        }
        return *result;
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<std::vector<ArtifactMember>> Service::artifact_members(const std::string& customer,
                                                              const std::string& repository,
                                                              const std::string& version) const {
    try {
        const WorkspaceAccess config = impl_->workspace_config();
        std::optional<BorrowedWorkspace> borrowed;
        if (config.route && config.lend) {
            const auto route = config.route(customer, repository);
            if (!route) {
                return std::unexpected(route.error());
            }
            if (*route) {
                auto lent = config.lend(customer, **route);
                if (!lent) {
                    return std::unexpected(lent.error());
                }
                borrowed = std::move(*lent);
            }
        }
        std::vector<ArtifactMember> output;
        const auto read = [&](const Impl::CustomerState& state) {
            for (const auto& [key, segment] : state.artifacts) {
                if (segment.repository != repository || segment.version != version) {
                    continue;
                }
                for (const Occurrence& member : segment.members) {
                    output.push_back(ArtifactMember{segment.artifact,
                                                    segment.version,
                                                    member.path,
                                                    member.digest,
                                                    member.indexed,
                                                    member.reason});
                }
            }
        };
        if (borrowed) {
            const auto state = impl_->workspace_state(borrowed->index.workspace);
            {
                std::lock_guard held(state->mutex);
                if (const auto ensured = Impl::ensure_workspace(*state, borrowed->index); !ensured) {
                    return std::unexpected(ensured.error());
                }
                read(state->data);
            }
            impl_->trim();
        } else {
            const auto state = impl_->space_state(customer);
            std::lock_guard held(state->mutex);
            if (const auto ensured = impl_->ensure_space(customer, *state); !ensured) {
                return std::unexpected(ensured.error());
            }
            read(state->data);
        }
        std::ranges::sort(output, {}, &ArtifactMember::path);
        return output;
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<QueryResult>
Service::query(const Query& query, const std::string& principal, control::Store& store) const {
    try {
        const auto scoped = split_artifact_scope(query.text);
        if (!scoped) {
            return std::unexpected(scoped.error());
        }
        Impl::Matcher matcher;
        matcher.text = scoped->first;
        matcher.artifact_scope = scoped->second;
        const std::string& text = matcher.text;
        if (query.history && !query.change.empty()) {
            return std::unexpected(
                Error{"invalid_input", "history and change are mutually exclusive"});
        }
        matcher.scans_history = query.history || !query.change.empty();
        if (matcher.scans_history &&
            (!query.ref.empty() || query.customer.empty() || query.repository.empty())) {
            return std::unexpected(Error{
                "invalid_input", "scan queries require customer and repository, without ref"});
        }
        if (matcher.artifact_scope && (matcher.scans_history || !query.ref.empty())) {
            return std::unexpected(Error{
                "invalid_input", "artifact scope cannot be combined with ref, change, or history"});
        }
        matcher.is_regex = text.size() > 2 && text.front() == '/' && text.back() == '/';
        matcher.is_substring = text.size() > 1 && text.front() == '"' && text.back() == '"';
        matcher.needle = matcher.is_regex       ? text.substr(1, text.size() - 2)
                         : matcher.is_substring ? text.substr(1, text.size() - 2)
                                                : text;
        if (matcher.is_regex) {
            try {
                matcher.expression.emplace(matcher.needle);
            } catch (const std::regex_error& error) {
                return std::unexpected(
                    Error{"invalid_input", "invalid regex: " + std::string(error.what())});
            }
        }
        QueryResult output;
        output.mode = matcher.artifact_scope  ? QueryMode::Artifact
                      : matcher.scans_history ? QueryMode::Scan
                                              : QueryMode::Hot;
        std::vector<Hit> hits;
        const WorkspaceAccess config = impl_->workspace_config();

        // Which indexes the query reads: one workspace's, a repository's own, or a space's (or
        // every space's) index on lake's volume with each of its switched workspaces' indexes.
        std::vector<std::pair<std::string, std::string>> workspaces; // (space, workspace)
        bool read_spaces = true;
        bool named = false; // the workspace was named or follows from the repository
        if (!query.workspace.empty()) {
            if (!config.lend) {
                return std::unexpected(Error{"unsupported", "workspace indexes are not configured"});
            }
            workspaces.emplace_back(query.customer, query.workspace);
            read_spaces = false;
            named = true;
        } else if (!query.repository.empty() && config.route) {
            const auto route = config.route(query.customer, query.repository);
            if (!route) {
                return std::unexpected(route.error());
            }
            if (*route) {
                workspaces.emplace_back(query.customer, **route);
                read_spaces = false;
                named = true;
            }
        } else if (config.switched) {
            const auto listed = config.switched(query.customer);
            if (!listed) {
                return std::unexpected(listed.error());
            }
            workspaces = *listed;
        }

        // A repository a workspace store serves is never answered from a space's index.
        std::map<std::string, bool> routed;
        const auto skip_in_space = [&](const std::string& customer) {
            return [&, customer](const std::string& repository) {
                if (!config.route) {
                    return false;
                }
                const std::string key = customer + "/" + repository;
                if (const auto found = routed.find(key); found != routed.end()) {
                    return found->second;
                }
                const auto route = config.route(customer, repository);
                const bool skip = !route || route->has_value();
                routed.emplace(key, skip);
                return skip;
            };
        };

        if (read_spaces) {
            if (matcher.scans_history) {
                // A history scan borrows its repository before any index's mutex is taken.
                auto borrowed = impl_->borrow(query.customer, query.repository);
                if (!borrowed) {
                    return std::unexpected(borrowed.error());
                }
                const auto state = impl_->space_state(query.customer);
                std::lock_guard held(state->mutex);
                if (const auto ensured = impl_->ensure_space(query.customer, *state); !ensured) {
                    return std::unexpected(ensured.error());
                }
                DirectoryStorage storage(impl_->index_path(query.customer));
                const auto scanned =
                    Impl::scan(query, state->data, borrowed->repository, storage);
                if (!scanned) {
                    return std::unexpected(scanned.error());
                }
                output.scanned_changes += scanned->changes;
                Impl::collect(query, matcher, query.customer, "", state->data, &scanned->occurrences,
                              [](const std::string&) { return false; }, output, hits);
            } else {
                const std::vector<std::string> customers =
                    query.customer.empty() ? impl_->space_directories()
                                           : std::vector<std::string>{query.customer};
                for (const std::string& customer : customers) {
                    const auto state = impl_->space_state(customer);
                    std::lock_guard held(state->mutex);
                    if (const auto ensured = impl_->ensure_space(customer, *state); !ensured) {
                        return std::unexpected(ensured.error());
                    }
                    Impl::collect(query, matcher, customer, "", state->data, nullptr,
                                  skip_in_space(customer), output, hits);
                }
            }
        }

        for (const auto& [customer, workspace] : workspaces) {
            // The workspace's lane is taken before its index's mutex, and no index's mutex is
            // held while waiting on it.
            std::optional<BorrowedRepository> scanned_repository;
            std::optional<BorrowedWorkspace> lent;
            if (matcher.scans_history) {
                auto borrowed = impl_->borrow(query.customer, query.repository);
                if (!borrowed) {
                    return std::unexpected(borrowed.error());
                }
                if (!borrowed->workspace) {
                    return std::unexpected(Error{"store_unavailable",
                                                 "the repository's store changed during the search; retry"});
                }
                scanned_repository = std::move(*borrowed);
            } else {
                auto borrowed = config.lend(customer, workspace);
                if (!borrowed) {
                    if (named) {
                        return std::unexpected(borrowed.error());
                    }
                    output.skipped_workspaces.push_back(workspace);
                    continue;
                }
                lent = std::move(*borrowed);
            }
            const WorkspaceIndex& index =
                scanned_repository ? *scanned_repository->workspace : lent->index;
            const std::string space = !index.customer.empty() ? index.customer
                                      : customer.empty()      ? query.customer
                                                              : customer;
            const auto state = impl_->workspace_state(index.workspace);
            {
                std::lock_guard held(state->mutex);
                if (const auto ensured = Impl::ensure_workspace(*state, index); !ensured) {
                    if (named) {
                        return std::unexpected(ensured.error());
                    }
                    output.skipped_workspaces.push_back(workspace);
                    continue;
                }
                if (scanned_repository) {
                    ObjectStorage storage(index.objects);
                    const auto scanned =
                        Impl::scan(query, state->data, scanned_repository->repository, storage);
                    if (!scanned) {
                        return std::unexpected(scanned.error());
                    }
                    output.scanned_changes += scanned->changes;
                    Impl::collect(query, matcher, space, index.workspace, state->data,
                                  &scanned->occurrences, [](const std::string&) { return false; },
                                  output, hits);
                } else {
                    Impl::collect(query, matcher, space, index.workspace, state->data, nullptr,
                                  [](const std::string&) { return false; }, output, hits);
                }
            }
            impl_->trim();
        }

        // Every candidate is authorized in one batch, after every index and lane is let go.
        std::vector<std::string> candidate_objects;
        candidate_objects.reserve(hits.size());
        std::vector<std::string> policy_object_ids;
        std::vector<Hit> kept;
        for (Hit& hit : hits) {
            std::string policy_object_id = hit.repository;
            if (principal != "admin") {
                const auto policy_object = store.find_policy_object("repository", hit.repository);
                if (!policy_object) {
                    if (policy_object.error().code == "not_found") {
                        continue;
                    }
                    return std::unexpected(
                        Error{policy_object.error().code, policy_object.error().message});
                }
                policy_object_id = policy_object->id;
            }
            candidate_objects.push_back(policy_object_id);
            policy_object_ids.push_back(policy_object_id);
            kept.push_back(std::move(hit));
        }
        std::sort(policy_object_ids.begin(), policy_object_ids.end());
        policy_object_ids.erase(std::unique(policy_object_ids.begin(), policy_object_ids.end()),
                                policy_object_ids.end());
        control::AuthorizationCache cache;
        const auto decisions =
            control::authorize_batch(store, principal, policy_object_ids, "read", cache);
        if (!decisions) {
            return std::unexpected(Error{decisions.error().code, decisions.error().message});
        }
        for (std::size_t index = 0; index < kept.size(); ++index) {
            const auto decision = decisions->find(candidate_objects[index]);
            if (decision == decisions->end() || !decision->second.allowed) {
                continue;
            }
            output.items.push_back(std::move(kept[index]));
        }
        std::sort(output.items.begin(), output.items.end(), [](const Hit& left, const Hit& right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.repository != right.repository) {
                return left.repository < right.repository;
            }
            return left.path < right.path;
        });
        const int limit = query.limit <= 0 ? 20 : std::min(query.limit, 1000);
        if (output.items.size() > static_cast<std::size_t>(limit)) {
            output.items.resize(static_cast<std::size_t>(limit));
        }
        return output;
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

} // namespace lake::search
