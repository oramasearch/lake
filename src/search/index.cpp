#include "lake/search/index.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <random>
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
#include "lake/control/workspace_store.hpp"

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

// A regex query is bounded (LSR-117, lake#984): std::regex backtracks, so the pattern is short and
// has no quantified group that itself repeats, only the first kRegexInputBytes of a blob are
// searched, and a query stops scanning after kRegexScanBudget.
constexpr std::size_t kRegexPatternBytes = 256;
constexpr std::size_t kRegexInputBytes = 1024 * 1024;
constexpr auto kRegexScanBudget = std::chrono::seconds(2);

// Why `pattern` is refused as a regex query, or empty when it is accepted.
std::string regex_pattern_problem(const std::string& pattern) {
    if (pattern.size() > kRegexPatternBytes) {
        return "a regex query is at most " + std::to_string(kRegexPatternBytes) + " characters";
    }
    // A group that holds a quantifier and is quantified itself, such as (a+)+ or (x*y)*, can take
    // exponential time on a backtracking engine.
    static const std::regex nested(R"(\([^()]*[*+?}][^()]*\)\s*[*+{])");
    if (std::regex_search(pattern, nested)) {
        return "a regex query cannot repeat a group that itself repeats";
    }
    return {};
}

bool bounded_regex_search(const std::string& content, const std::regex& expression) {
    const auto end = content.begin() +
                     static_cast<std::ptrdiff_t>(std::min(content.size(), kRegexInputBytes));
    return std::regex_search(content.begin(), end, expression);
}

std::size_t line_for(const std::string& content,
                     const std::string& needle,
                     const std::optional<std::regex>& expression) {
    std::size_t position = content.find(needle);
    if (expression) {
        std::smatch match;
        const auto end = content.begin() +
                         static_cast<std::ptrdiff_t>(std::min(content.size(), kRegexInputBytes));
        if (std::regex_search(content.begin(), end, match, *expression)) {
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

std::string random_name() {
    static thread_local std::mt19937_64 generator(std::random_device{}());
    return std::format("{:016x}{:016x}", generator(), generator());
}

// Where one index keeps its files: a directory on lake's volume (a space's index, one blob
// catalogue `blobs.json` replaced whole) or the objects beside a workspace's store (lake#381:
// blobs in write-once runs, every object named by the manifest).
class Storage {
public:
    virtual ~Storage() = default;
    virtual Result<std::optional<std::string>> read(const std::string& name) = 0;
    virtual Result<void> write(const std::string& name, const std::string& bytes, bool once) = 0;
    virtual Result<void> remove(const std::string& name) = 0;
    virtual Result<std::vector<std::string>> list() = 0;
    virtual Result<void> sync() = 0;
    // Whether blobs are kept in write-once runs (a workspace's objects) rather than one catalogue.
    [[nodiscard]] virtual bool runs() const = 0;
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

    Result<void> write(const std::string& name, const std::string& bytes, bool) override {
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
                std::filesystem::remove(temporary, error);
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

    bool runs() const override {
        return false;
    }

    // Temporary files a write left behind when its process stopped.
    void remove_temporaries() {
        std::error_code error;
        if (!std::filesystem::exists(directory_, error)) {
            return;
        }
        std::vector<std::filesystem::path> stale;
        for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
            if (entry.path().filename().string().find(".tmp-") != std::string::npos) {
                stale.push_back(entry.path());
            }
        }
        for (const auto& path : stale) {
            std::filesystem::remove(path, error);
        }
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
    Result<void> write(const std::string& name, const std::string& bytes, bool once) override {
        return objects_->put(name, bytes, once);
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
    bool runs() const override {
        return true;
    }

private:
    std::shared_ptr<IndexObjects> objects_;
};

static_assert(workspace_index_prefix == control::workspace_store_search_prefix,
              "the workspace store's backend reserves the search index's prefix");

constexpr std::string_view sealed_magic{"LKSIDX1\0", 8};

class SealedIndexObjects final : public IndexObjects {
public:
    SealedIndexObjects(std::shared_ptr<control::ZettaObjectClient> client,
                       std::shared_ptr<const control::WorkspaceStoreCipher> cipher,
                       std::uint64_t fence,
                       std::function<void()> on_lost)
        : client_(std::move(client)), cipher_(std::move(cipher)), fence_(fence),
          on_lost_(std::move(on_lost)) {}

    Result<std::optional<std::string>> get(const std::string& name) override {
        auto bytes = liquid::runtime::block_on(client_->get_all(key(name)));
        if (!bytes) {
            if (bytes.error().code == liquid::errc::not_found) {
                return std::optional<std::string>{};
            }
            return std::unexpected(error_of(bytes.error()));
        }
        if (!bytes->starts_with(sealed_magic)) {
            return std::unexpected(Error{"corrupt", "index object " + name + " is not sealed"});
        }
        auto plain = cipher_->open(associated(name), std::string_view(*bytes).substr(sealed_magic.size()));
        if (!plain) {
            return std::unexpected(Error{"corrupt", "index object " + name + ": " + plain.error().message});
        }
        return std::optional<std::string>{
            std::string(reinterpret_cast<const char*>(plain->data()), plain->size())};
    }

    Result<void> put(const std::string& name, std::string bytes, bool once) override {
        auto sealed = cipher_->seal(
            associated(name),
            std::span(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()));
        if (!sealed) {
            return std::unexpected(error_of(sealed.error()));
        }
        auto written = liquid::runtime::block_on(
            client_->put(key(name), std::string(sealed_magic) + *sealed, fence_, once));
        if (!written) {
            // A retried write-once put whose first attempt Zetta took: the same content is there.
            if (once && written.error().code == liquid::errc::already_exists) {
                const auto present = get(name);
                if (present && *present && **present == bytes) {
                    return {};
                }
            }
            return std::unexpected(error_of(written.error()));
        }
        return {};
    }

    Result<void> remove(const std::string& name) override {
        auto removed = liquid::runtime::block_on(client_->remove(key(name), fence_));
        if (!removed && removed.error().code != liquid::errc::not_found) {
            return std::unexpected(error_of(removed.error()));
        }
        return {};
    }

    Result<std::vector<std::string>> list() override {
        auto listed = liquid::runtime::block_on(client_->list(std::string(workspace_index_prefix)));
        if (!listed) {
            return std::unexpected(error_of(listed.error()));
        }
        std::vector<std::string> output;
        for (const control::ObjectListEntry& entry : *listed) {
            if (entry.key.starts_with(workspace_index_prefix)) {
                output.push_back(entry.key.substr(workspace_index_prefix.size()));
            }
        }
        return output;
    }

    Result<void> sync() override {
        auto synced = liquid::runtime::block_on(client_->barrier());
        if (!synced) {
            return std::unexpected(error_of(synced.error()));
        }
        return {};
    }

private:
    static std::string key(const std::string& name) {
        return std::string(workspace_index_prefix) + name;
    }

    std::string associated(const std::string& name) const {
        return "lake/search-index/v1/" + cipher_->workspace_id() + "/" + name;
    }

    Error error_of(const liquid::error& error) const {
        if (error.code == liquid::errc::locked) {
            // Zetta refused the write as fenced: the store's lease is someone else's now.
            if (on_lost_) {
                on_lost_();
            }
            return {"store_unavailable", error.message};
        }
        if (error.code == liquid::errc::busy) {
            return {"busy", error.message};
        }
        return {std::string(liquid::to_string(error.code)), error.message};
    }

    std::shared_ptr<control::ZettaObjectClient> client_;
    std::shared_ptr<const control::WorkspaceStoreCipher> cipher_;
    std::uint64_t fence_ = 0;
    std::function<void()> on_lost_;
};

constexpr std::string_view blobs_file = "blobs.json";
constexpr std::string_view manifest_file = "manifest.json";
static_assert(workspace_index_manifest.substr(0, workspace_index_prefix.size()) == workspace_index_prefix &&
                  workspace_index_manifest.substr(workspace_index_prefix.size()) == manifest_file,
              "workspace_index_manifest is manifest_file under workspace_index_prefix");
constexpr std::string_view artifacts_file = "artifacts.json";

} // namespace

std::shared_ptr<IndexObjects>
make_sealed_index_objects(std::shared_ptr<control::ZettaObjectClient> client,
                          std::shared_ptr<const control::WorkspaceStoreCipher> cipher,
                          std::uint64_t fence,
                          std::function<void()> on_lost) {
    return std::make_shared<SealedIndexObjects>(std::move(client), std::move(cipher), fence,
                                                std::move(on_lost));
}

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
        // Blobs added since the catalogue was last written (a space's index).
        bool blobs_added = false;
        // A workspace's index: the run that holds each blob, the bytes of each run, and the
        // artifact members' object.
        std::map<std::string, std::string> blob_run;
        std::map<std::string, std::uint64_t> run_bytes;
        std::string artifacts_object;
        // Approximate memory of the loaded index.
        std::size_t bytes = 0;
    };

    // One index: a space's on lake's volume, or a workspace's beside its store.
    //
    // Lock order: a store's lane (through a lend), then an index's mutex, then Impl::mutex, which
    // is only ever held briefly (its maps, the configuration) and never across I/O or a lane wait.
    // trim() holds Impl::mutex and only try-locks an index's mutex, so it never waits on one.
    struct IndexState {
        std::mutex mutex;
        bool loaded = false;
        std::uint64_t generation = 0;
        CustomerState data;
        // A workspace's index being built: it answers nothing, and writes to it are remembered in
        // `pending` and applied once it is ready.
        bool building = false;
        std::set<std::string> pending;
        // Repositories deleted while the index was being built or while it could not be reached:
        // left out of the build, and removed from the index once it is ready.
        std::set<std::string> removed;
        // After a failed rebuild, none is scheduled again before this.
        std::chrono::steady_clock::time_point retry_after{};
        std::chrono::seconds backoff{0};
        std::string last_error;
        // lake#540: the delay before the last retry of `pending` and `removed`; it doubles on each
        // retry that fails, up to pending_retry_most, and is reset once they are all applied.
        std::chrono::milliseconds pending_backoff{0};
    };

    enum class Readiness { Ready, Building, Schedule };

    std::filesystem::path root;
    // Guards the maps and the configuration below, and nothing else.
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
    // Deleted workspaces: no state is kept for them again.
    std::set<std::string> dropped;
    std::atomic<bool> stopping{false};

    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}

    [[nodiscard]] WorkspaceAccess workspace_config() const {
        std::lock_guard lock(mutex);
        return workspace_access;
    }

    // The first delay before a workspace's pending repositories are tried again (lake#540).
    std::chrono::milliseconds pending_retry_first{std::chrono::seconds(15)};
    static constexpr std::chrono::milliseconds pending_retry_most{std::chrono::minutes(10)};

    // The delay before the next retry of a workspace's pending repositories and removals: the
    // first after pending_retry_first, then doubling up to 10 minutes. The caller holds the state's
    // mutex (Impl::mutex is taken after it, briefly).
    [[nodiscard]] std::chrono::milliseconds next_pending_retry(IndexState& state) {
        std::chrono::milliseconds first;
        {
            std::lock_guard lock(mutex);
            first = pending_retry_first;
        }
        state.pending_backoff = state.pending_backoff.count() == 0
                                    ? first
                                    : std::min<std::chrono::milliseconds>(pending_retry_most, state.pending_backoff * 2);
        return state.pending_backoff;
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
        if (dropped.contains(workspace)) {
            return std::make_shared<IndexState>();
        }
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
                                                    const std::string& repository,
                                                    bool lake_work = false) const {
        RepositoryAccess lend;
        bool configured = false;
        {
            std::lock_guard lock(mutex);
            lend = access;
            configured = access_configured;
        }
        if (lend) {
            return lend(customer, repository, lake_work);
        }
        if (configured) {
            return std::unexpected(Error{
                "store_unavailable",
                "search reads repositories only through Lake's repository store, which is closed"});
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

    [[nodiscard]] static std::string blobs_text(const CustomerState& state,
                                                const std::set<std::string>* only = nullptr) {
        json blobs = json::object();
        for (const auto& [digest, record] : state.blobs) {
            if (only == nullptr || only->contains(digest)) {
                blobs[digest] = {{"content", record.content}};
            }
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

    [[nodiscard]] static json manifest_active(const std::map<std::string, Segment>& active) {
        json entries = json::array();
        for (const auto& [slot, segment] : active) {
            entries.push_back({{"repository", segment.repository},
                               {"ref", segment.ref},
                               {"change", segment.change},
                               {"kind", publication_kind_name(segment.kind)},
                               {"segment", segment.file}});
        }
        return entries;
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

    // The blobs an index's segments and indexed artifact members name.
    [[nodiscard]] static std::set<std::string>
    referenced_blobs(const std::map<std::string, Segment>& active,
                     const std::map<std::string, ArtifactSegment>& artifacts) {
        std::set<std::string> referenced;
        for (const auto& [slot, segment] : active) {
            for (const Occurrence& occurrence : segment.occurrences) {
                referenced.insert(occurrence.blob);
            }
        }
        for (const auto& [key, artifact] : artifacts) {
            for (const Occurrence& member : artifact.members) {
                if (member.indexed) {
                    referenced.insert(member.blob);
                }
            }
        }
        return referenced;
    }

    // Publishes the next state of an index. New segments and the blobs they add are written
    // first, then the manifest (one atomic replacement), then what nothing references any more is
    // removed. A failure before the manifest leaves the previous state whole. `sweep` also removes
    // every other object of the index (a rebuild). `budget`: the bytes the index may add (a
    // workspace's quota room); a commit that needs more writes nothing and answers `busy`.
    //
    // In a workspace's objects, blobs are written once, in a new run per commit holding only the
    // blobs no run has; the manifest names the runs. When more than half of the runs' bytes are
    // unreferenced, the live blobs are written into one new run and the old runs removed.
    [[nodiscard]] static Result<void> commit(Storage& storage,
                                             CustomerState& state,
                                             std::map<std::string, Segment> next_active,
                                             std::map<std::string, ArtifactSegment> next_artifacts,
                                             bool artifacts_changed,
                                             bool sweep,
                                             std::optional<std::uint64_t> budget = std::nullopt,
                                             std::uint64_t* added = nullptr) {
        const std::set<std::string> referenced = referenced_blobs(next_active, next_artifacts);
        std::set<std::string> keep{std::string(manifest_file)};
        std::vector<std::pair<std::string, std::string>> writes; // name, bytes; written once
        for (auto& [slot, segment] : next_active) {
            if (segment.file.empty()) {
                segment.file = storage.runs() ? "segments/" + random_name() + ".json" : segment_name(segment);
                writes.emplace_back(segment.file, segment_text(segment));
            }
            keep.insert(segment.file);
        }
        std::string next_artifacts_object = state.artifacts_object;
        std::map<std::string, std::string> next_blob_run;
        std::map<std::string, std::uint64_t> next_run_bytes;
        if (storage.runs()) {
            if (artifacts_changed || sweep || next_artifacts_object.empty()) {
                next_artifacts_object = "artifacts/" + random_name() + ".json";
                writes.emplace_back(next_artifacts_object, artifacts_text(next_artifacts));
            }
            keep.insert(next_artifacts_object);
            // Live bytes per existing run, and the blobs no run holds.
            std::map<std::string, std::uint64_t> live;
            std::set<std::string> unplaced;
            std::uint64_t total = 0;
            std::uint64_t alive = 0;
            for (const std::string& digest : referenced) {
                const auto blob = state.blobs.find(digest);
                if (blob == state.blobs.end()) {
                    return std::unexpected(Error{"corrupt", "the index names blob " + digest +
                                                                " but holds no text for it"});
                }
                const auto run = sweep ? state.blob_run.end() : state.blob_run.find(digest);
                if (run == state.blob_run.end()) {
                    unplaced.insert(digest);
                } else {
                    live[run->second] += blob->second.content.size();
                }
            }
            for (const auto& [run, bytes] : live) {
                total += state.run_bytes.at(run);
                alive += bytes;
            }
            const bool compact = !sweep && total > 1024 * 1024 && alive * 2 < total;
            std::set<std::string> placed_in_new = unplaced;
            if (compact) {
                placed_in_new = referenced;
            } else {
                for (const std::string& digest : referenced) {
                    if (!unplaced.contains(digest)) {
                        const std::string& run = state.blob_run.at(digest);
                        next_blob_run[digest] = run;
                        next_run_bytes[run] = state.run_bytes.at(run);
                        keep.insert(run);
                    }
                }
            }
            if (!placed_in_new.empty()) {
                const std::string run = "runs/" + random_name() + ".json";
                std::uint64_t bytes = 0;
                for (const std::string& digest : placed_in_new) {
                    next_blob_run[digest] = run;
                    bytes += state.blobs.at(digest).content.size();
                }
                next_run_bytes[run] = bytes;
                writes.emplace_back(run, blobs_text(state, &placed_in_new));
                keep.insert(run);
            }
        }
        std::uint64_t adding = 0;
        for (const auto& [name, bytes] : writes) {
            adding += bytes.size();
        }
        if (added != nullptr) {
            *added = 0;
        }
        if (budget) {
            if (adding > *budget) {
                return std::unexpected(Error{
                    "busy", std::format("the workspace's storage quota leaves no room for {} more "
                                        "bytes of its search index; indexing is stopped so the "
                                        "repositories' own writes keep their room",
                                        adding)});
            }
        }
        for (const auto& [name, bytes] : writes) {
            if (const auto written = storage.write(name, bytes, storage.runs()); !written) {
                return written;
            }
        }
        if (added != nullptr) {
            *added = adding;
        }
        if (!storage.runs()) {
            if (state.blobs_added || sweep) {
                if (const auto written = storage.write(std::string(blobs_file), blobs_text(state), false);
                    !written) {
                    return written;
                }
            }
            if (artifacts_changed || sweep) {
                if (const auto written = storage.write(std::string(artifacts_file),
                                                       artifacts_text(next_artifacts), false);
                    !written) {
                    return written;
                }
            }
            keep.insert(std::string(blobs_file));
            keep.insert(std::string(artifacts_file));
        }
        // No sync before the manifest (lake#535): the objects it names were each answered before
        // it is sent, to the one bucket whose log keeps the order they were sent in, so a crash
        // that keeps the manifest keeps them. The sync after it makes the commit durable.
        json manifest{{"version", storage.runs() ? 3 : 2}, {"active", manifest_active(next_active)}};
        if (storage.runs()) {
            std::set<std::string> runs;
            for (const auto& [digest, run] : next_blob_run) {
                runs.insert(run);
            }
            manifest["runs"] = runs;
            manifest["artifacts"] = next_artifacts_object;
        }
        if (const auto published = storage.write(std::string(manifest_file), manifest.dump(), false);
            !published) {
            return published;
        }
        if (const auto synced = storage.sync(); !synced) {
            return synced;
        }
        // The manifest is replaced: what follows only removes what it no longer names.
        std::set<std::string> dropped;
        for (const auto& [slot, segment] : state.active) {
            if (!keep.contains(segment.file)) {
                dropped.insert(segment.file);
            }
        }
        for (const auto& [run, bytes] : state.run_bytes) {
            if (!keep.contains(run)) {
                dropped.insert(run);
            }
        }
        if (!state.artifacts_object.empty() && !keep.contains(state.artifacts_object)) {
            dropped.insert(state.artifacts_object);
        }
        state.active = std::move(next_active);
        state.artifacts = std::move(next_artifacts);
        state.artifacts_object = next_artifacts_object;
        state.blob_run = std::move(next_blob_run);
        state.run_bytes = std::move(next_run_bytes);
        const std::size_t before = state.blobs.size();
        std::erase_if(state.blobs, [&](const auto& item) { return !referenced.contains(item.first); });
        state.blobs_added = false;
        Result<void> outcome;
        if (!storage.runs() && state.blobs.size() != before) {
            // The catalogue on lake's volume is pruned in place; a failure is reported, and the
            // next commit writes it again.
            if (auto written = storage.write(std::string(blobs_file), blobs_text(state), false); !written) {
                state.blobs_added = true;
                outcome = std::unexpected(written.error());
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
        return outcome;
    }

    // Reads an index. False when it has no manifest (never written, or wiped). A blob, run or
    // segment the manifest names that is not there is `corrupt`.
    [[nodiscard]] static Result<bool> load_data(Storage& storage, CustomerState& state) {
        const auto manifest_value = storage.read(std::string(manifest_file));
        if (!manifest_value) {
            return std::unexpected(manifest_value.error());
        }
        if (!*manifest_value) {
            return false;
        }
        const json manifest = json::parse(**manifest_value);
        const auto parse_blobs = [&](const std::string& text, const std::string& run) {
            const json blobs = json::parse(text);
            std::uint64_t bytes = 0;
            for (auto item = blobs.begin(); item != blobs.end(); ++item) {
                std::string content = item.value().at("content").get<std::string>();
                bytes += content.size();
                if (!run.empty()) {
                    state.blob_run[item.key()] = run;
                }
                state.blobs.emplace(item.key(), BlobRecord{std::move(content)});
            }
            if (!run.empty()) {
                state.run_bytes[run] = bytes;
            }
        };
        std::optional<std::string> artifacts_text_value;
        if (storage.runs()) {
            for (const json& run : manifest.value("runs", json::array())) {
                const std::string name = run.get<std::string>();
                const auto bytes = storage.read(name);
                if (!bytes) {
                    return std::unexpected(bytes.error());
                }
                if (!*bytes) {
                    return std::unexpected(Error{"corrupt", "blob run " + name + " named by the manifest is absent"});
                }
                parse_blobs(**bytes, name);
            }
            state.artifacts_object = manifest.value("artifacts", std::string());
            if (!state.artifacts_object.empty()) {
                const auto bytes = storage.read(state.artifacts_object);
                if (!bytes) {
                    return std::unexpected(bytes.error());
                }
                if (!*bytes) {
                    return std::unexpected(Error{"corrupt", "artifact members " + state.artifacts_object +
                                                                " named by the manifest are absent"});
                }
                artifacts_text_value = **bytes;
            }
        } else {
            const auto blob_value = storage.read(std::string(blobs_file));
            if (!blob_value) {
                return std::unexpected(blob_value.error());
            }
            if (*blob_value) {
                parse_blobs(**blob_value, "");
            }
            const auto artifacts_value = storage.read(std::string(artifacts_file));
            if (!artifacts_value) {
                return std::unexpected(artifacts_value.error());
            }
            artifacts_text_value = *artifacts_value;
        }
        for (const json& active : manifest.at("active")) {
            const std::string file = active.at("segment").get<std::string>();
            const auto segment_bytes = storage.read(file);
            if (!segment_bytes) {
                return std::unexpected(segment_bytes.error());
            }
            if (!*segment_bytes) {
                return std::unexpected(
                    Error{"corrupt", "index segment " + file + " named by the manifest is absent"});
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
        if (artifacts_text_value) {
            const json artifacts = json::parse(*artifacts_text_value);
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
        for (const std::string& digest : referenced_blobs(state.active, state.artifacts)) {
            if (!state.blobs.contains(digest)) {
                return std::unexpected(
                    Error{"corrupt", "the index names blob " + digest + " but holds no text for it"});
            }
        }
        build_memory(state);
        return true;
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

    // A repository's branch tips as its references name them now (in a workspace's index or a
    // space's, lake#584): a reference that moved is published again, one that is gone is
    // dropped, tag snapshots are kept.
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

    // A repository's release snapshots, from its release records.
    [[nodiscard]] static Result<void> publish_tags(liquid::repository& opened,
                                                   CustomerState& state,
                                                   const WorkspaceIndex& index,
                                                   const std::string& repository,
                                                   std::map<std::string, Segment>& next_active) {
        if (!index.tags) {
            return {};
        }
        const auto tags = index.tags(repository);
        if (!tags) {
            return std::unexpected(tags.error());
        }
        for (const auto& [tag, change] : *tags) {
            const auto published = publish_into(
                opened, state, Publish{"", repository, tag, change, PublicationKind::TagSnapshot},
                next_active);
            // A release naming a change the store does not hold, or a conflicting duplicate, is
            // left out rather than failing the whole index.
            if (!published && published.error().code != "not_found" &&
                published.error().code != "conflict" && published.error().code != "invalid_input") {
                return std::unexpected(published.error());
            }
        }
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

    // Loads a workspace's index on its first use, and again when its store is owned under another
    // fence (another process may have written it since). An index without a manifest (never
    // written, or wiped), one missing an object its manifest names, or one that does not read back
    // (`corrupt`, unparseable) is not served: it answers Schedule (its rebuild is to be scheduled
    // once the caller let go of its lane) or Building. The caller holds the state's mutex.
    [[nodiscard]] static Result<Readiness> ensure_workspace(IndexState& state, const WorkspaceIndex& index) {
        if (state.building) {
            return Readiness::Building;
        }
        if (state.loaded && state.generation == index.generation) {
            return Readiness::Ready;
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
            state.data = CustomerState{};
            return std::unexpected(loaded.error());
        }
        if (loaded && *loaded) {
            state.loaded = true;
            state.generation = index.generation;
            sweep_orphans(storage, state.data);
            return Readiness::Ready;
        }
        state.data = CustomerState{};
        if (std::chrono::steady_clock::now() < state.retry_after) {
            return Readiness::Building;
        }
        state.building = true;
        return Readiness::Schedule;
    }

    // Removes the objects of a workspace's index its manifest does not name: left by a commit that
    // failed before its manifest, or by a process that stopped mid-commit. The caller holds the
    // state's mutex; no commit of this index runs meanwhile (a rebuild holds `building`, and every
    // other commit holds the mutex). A failure leaves them for the next load.
    static void sweep_orphans(Storage& storage, const CustomerState& state) {
        std::set<std::string> keep{std::string(manifest_file)};
        for (const auto& [slot, segment] : state.active) {
            keep.insert(segment.file);
        }
        for (const auto& [run, bytes] : state.run_bytes) {
            keep.insert(run);
        }
        if (!state.artifacts_object.empty()) {
            keep.insert(state.artifacts_object);
        }
        const auto listed = storage.list();
        if (!listed) {
            return;
        }
        for (const std::string& name : *listed) {
            if (!keep.contains(name)) {
                static_cast<void>(storage.remove(name));
            }
        }
    }

    // Runs or queues a workspace's index job. Never called with a lane or an index's mutex held.
    void schedule(const std::string& customer, const std::string& workspace, Service& service,
                  std::chrono::milliseconds delay = std::chrono::milliseconds(0)) {
        if (stopping.load()) {
            return;
        }
        const WorkspaceAccess config = workspace_config();
        if (config.schedule) {
            config.schedule(customer, workspace, delay);
            return;
        }
        if (delay.count() == 0) {
            static_cast<void>(service.rebuild_workspace(customer, workspace));
        }
    }

    // Drops from a space's index every repository a workspace store serves now, and the blobs
    // no segment references (history scans keep them), so the index on lake's volume keeps nothing
    // of a switched workspace (lake#381). A repository whose route cannot be read is kept, and left
    // out of results. Answers how many repositories were dropped.
    [[nodiscard]] Result<std::size_t> purge_switched(const std::string& customer, IndexState& state) {
        const WorkspaceAccess config = workspace_config();
        DirectoryStorage storage(index_path(customer));
        storage.remove_temporaries();
        if (!config.route) {
            return std::size_t{0};
        }
        std::set<std::string> repositories;
        for (const auto& [slot, segment] : state.data.active) {
            repositories.insert(segment.repository);
        }
        for (const auto& [key, artifact] : state.data.artifacts) {
            repositories.insert(artifact.repository);
        }
        const std::set<std::string> referenced =
            referenced_blobs(state.data.active, state.data.artifacts);
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
                state.data = CustomerState{};
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
    // bound. One in use (its mutex held), one being built and the one used last are kept.
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
            if (!held || found->second->building || !found->second->pending.empty() ||
                std::chrono::steady_clock::now() < found->second->retry_after) {
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

    // `opened` was borrowed before any index's mutex was taken. A space's index keeps the blobs a
    // scan read (on lake's volume); a workspace's keeps them in memory only until its next commit.
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
            if (!storage.runs()) {
                const auto saved = storage.write(std::string(blobs_file), blobs_text(state), false);
                if (!saved) {
                    return std::unexpected(saved.error());
                }
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
            const auto scan_started = std::chrono::steady_clock::now();
            for (const std::string& digest : candidates) {
                if (matcher.is_regex &&
                    std::chrono::steady_clock::now() - scan_started > kRegexScanBudget) {
                    break; // the query's time budget is spent: what matched so far is answered
                }
                const auto blob = state.blobs.find(digest);
                if (blob == state.blobs.end()) {
                    continue;
                }
                const bool matched =
                    matcher.is_regex ? bounded_regex_search(blob->second.content, *matcher.expression)
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

    // One repository of a workspace, refreshed in its index: branch tips, artifacts and (with
    // `tags`) release snapshots. The caller holds the repository's lend and the state's mutex.
    [[nodiscard]] static Result<ReindexResult> refresh_repository(IndexState& state,
                                                                  const WorkspaceIndex& index,
                                                                  liquid::repository& opened,
                                                                  const std::string& repository,
                                                                  bool tags,
                                                                  bool* write_failed = nullptr) {
        auto next_active = state.data.active;
        if (const auto published = publish_refs(opened, state.data, repository, next_active); !published) {
            return std::unexpected(published.error());
        }
        if (tags) {
            if (const auto tagged = publish_tags(opened, state.data, index, repository, next_active);
                !tagged) {
                return std::unexpected(tagged.error());
            }
        }
        auto next_artifacts = state.data.artifacts;
        auto reindexed = reindex_into(opened, state.data, repository, next_artifacts);
        if (!reindexed) {
            return std::unexpected(reindexed.error());
        }
        ObjectStorage storage(index.objects);
        const auto budget = index.write_budget ? index.write_budget() : std::nullopt;
        std::uint64_t added = 0;
        const auto committed = commit(storage, state.data, std::move(next_active),
                                      std::move(next_artifacts), true, false, budget, &added);
        if (index.spent && added > 0) {
            index.spent(added);
        }
        if (!committed) {
            // What is in memory may be ahead of the objects: read them again on next use.
            state.loaded = false;
            state.data = CustomerState{};
            if (write_failed != nullptr) {
                *write_failed = true;
            }
            return std::unexpected(committed.error());
        }
        return *reindexed;
    }
};

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
    // A server configuring search starts it again after an earlier server's shutdown().
    impl_->stopping.store(false);
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

void Service::schedule_rebuild(const std::string& customer, const std::string& workspace) {
    {
        const auto state = impl_->workspace_state(workspace);
        std::lock_guard held(state->mutex);
        state->building = true;
        state->loaded = false;
        state->data = Impl::CustomerState{};
        state->retry_after = {};
    }
    impl_->schedule(customer, workspace, *this);
}

void Service::set_pending_retry(std::chrono::milliseconds first) {
    std::lock_guard lock(impl_->mutex);
    impl_->pending_retry_first = std::max(first, std::chrono::milliseconds(1));
}

void Service::shutdown() {
    impl_->stopping.store(true);
}

std::vector<std::string> Service::unsettled_workspaces() const {
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
        if (state->building || !state->pending.empty() || !state->removed.empty()) {
            output.push_back(workspace);
        }
    }
    return output;
}

std::vector<std::string> Service::tracked_workspaces() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<std::string> output;
    for (const auto& [workspace, state] : impl_->workspaces) {
        output.push_back(workspace);
    }
    return output;
}

Result<void> Service::rebuild_workspace(const std::string& customer, const std::string& workspace) {
    if (impl_->stopping.load()) {
        return std::unexpected(Error{"store_unavailable", "Lake is stopping"});
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->dropped.contains(workspace)) {
            return {};
        }
    }
    const auto state = impl_->workspace_state(workspace);
    bool ready = false;
    bool unloaded = false;
    {
        std::lock_guard held(state->mutex);
        ready = state->loaded && !state->building;
        // Not loaded and not being built: evicted, or a write failed and left repositories pending
        // (lake#540). Its stored index is loaded rather than rebuilt when it reads back.
        unloaded = !ready && !state->building && (!state->pending.empty() || !state->removed.empty());
        if (!ready && !unloaded) {
            state->building = true;
            state->loaded = false;
            state->data = Impl::CustomerState{};
        }
    }
    if (unloaded) {
        const WorkspaceAccess config = impl_->workspace_config();
        Result<Impl::Readiness> loaded = Impl::Readiness::Schedule;
        {
            auto lent = config.lend ? config.lend(customer, workspace, true)
                                    : Result<BorrowedWorkspace>(
                                          std::unexpected(Error{"unsupported", "no workspace access"}));
            if (!lent) {
                std::chrono::milliseconds delay{0};
                {
                    std::lock_guard held(state->mutex);
                    delay = impl_->next_pending_retry(*state);
                }
                impl_->schedule(customer, workspace, *this, delay);
                return std::unexpected(lent.error());
            }
            std::lock_guard held(state->mutex);
            if (state->loaded && !state->building) {
                loaded = Impl::Readiness::Ready;
            } else if (state->building) {
                loaded = Impl::Readiness::Building;
            } else {
                loaded = Impl::ensure_workspace(*state, lent->index);
            }
            if (loaded && *loaded == Impl::Readiness::Schedule) {
                // ensure_workspace marked it as being built: rebuilt below.
                state->loaded = false;
                state->data = Impl::CustomerState{};
            }
        }
        if (!loaded) {
            std::chrono::milliseconds delay{0};
            {
                std::lock_guard held(state->mutex);
                delay = impl_->next_pending_retry(*state);
            }
            impl_->schedule(customer, workspace, *this, delay);
            return std::unexpected(loaded.error());
        }
        if (*loaded == Impl::Readiness::Ready) {
            impl_->trim();
            return apply_pending(customer, workspace);
        }
        if (*loaded == Impl::Readiness::Building) {
            // A rebuild that failed waits out its delay; the next use schedules it again.
            return {};
        }
    }
    if (ready) {
        return apply_pending(customer, workspace);
    }
    const auto failed = [&](Error error) -> Result<void> {
        std::lock_guard held(state->mutex);
        state->building = false;
        state->loaded = false;
        state->data = Impl::CustomerState{};
        state->backoff = std::min<std::chrono::seconds>(
            std::chrono::minutes(10), std::max<std::chrono::seconds>(std::chrono::seconds(15), state->backoff * 2));
        state->retry_after = std::chrono::steady_clock::now() + state->backoff;
        state->last_error = error.message;
        return std::unexpected(std::move(error));
    };
    try {
        const WorkspaceAccess config = impl_->workspace_config();
        if (!config.lend) {
            return failed(Error{"unsupported", "workspace indexes are not configured"});
        }
        // The lend is held only to take the index's objects and the store's fence. A rebuild is
        // lake's own work: it waits for the lane rather than being refused admission.
        WorkspaceIndex index;
        {
            auto lent = config.lend(customer, workspace, true);
            if (!lent) {
                return failed(lent.error());
            }
            index = lent->index;
        }
        const std::string space = index.customer.empty() ? customer : index.customer;
        if (!index.repositories) {
            return failed(Error{"store_unavailable", "the workspace's repositories cannot be listed"});
        }
        const auto repositories = index.repositories();
        if (!repositories) {
            return failed(repositories.error());
        }
        Impl::CustomerState fresh;
        std::map<std::string, Segment> active;
        std::map<std::string, ArtifactSegment> artifacts;
        // Each repository is read under its own store's lane, one at a time, so a push to the
        // workspace waits at most one repository's read.
        for (const std::string& repository : *repositories) {
            if (impl_->stopping.load()) {
                return failed(Error{"store_unavailable", "Lake is stopping"});
            }
            {
                std::lock_guard held(state->mutex);
                if (state->removed.contains(repository)) {
                    continue;
                }
            }
            auto borrowed = impl_->borrow(space, repository, true);
            if (!borrowed) {
                if (borrowed.error().code == "not_found") {
                    continue;
                }
                return failed(Error{borrowed.error().code, "could not rebuild the search index of repository " +
                                                               repository + ": " + borrowed.error().message});
            }
            if (!borrowed->workspace || borrowed->workspace->workspace != workspace) {
                continue;
            }
            if (const auto published =
                    Impl::publish_refs(*borrowed->repository, fresh, repository, active);
                !published) {
                return failed(published.error());
            }
            if (const auto tagged =
                    Impl::publish_tags(*borrowed->repository, fresh, index, repository, active);
                !tagged) {
                return failed(tagged.error());
            }
            if (const auto reindexed =
                    Impl::reindex_into(*borrowed->repository, fresh, repository, artifacts);
                !reindexed) {
                return failed(reindexed.error());
            }
        }
        // Repositories deleted while they were read are left out.
        {
            std::lock_guard held(state->mutex);
            std::erase_if(active, [&](const auto& item) { return state->removed.contains(item.second.repository); });
            std::erase_if(artifacts,
                          [&](const auto& item) { return state->removed.contains(item.second.repository); });
        }
        // The index is written without any lane: its objects carry the fence taken above, and a
        // write to this workspace's index meanwhile is remembered in `pending`.
        ObjectStorage storage(index.objects);
        const auto budget = index.write_budget ? index.write_budget() : std::nullopt;
        std::uint64_t added = 0;
        const auto committed = Impl::commit(storage, fresh, std::move(active), std::move(artifacts), true,
                                            true, budget, &added);
        if (index.spent && added > 0) {
            index.spent(added);
        }
        if (!committed) {
            return failed(committed.error());
        }
        {
            std::lock_guard held(state->mutex);
            state->data = std::move(fresh);
            state->loaded = true;
            state->generation = index.generation;
            state->building = false;
            state->backoff = std::chrono::seconds(0);
            state->retry_after = {};
            state->last_error.clear();
        }
        impl_->trim();
        return apply_pending(customer, workspace);
    } catch (const std::exception& error) {
        return failed(Error{"store_error", error.what()});
    }
}

// Applies to a ready index what was written or deleted while it was built or unreachable. What
// cannot be applied now stays pending and is tried again after a delay.
Result<void> Service::apply_pending(const std::string& customer, const std::string& workspace) {
    const auto state = impl_->workspace_state(workspace);
    std::set<std::string> pending;
    std::set<std::string> removed;
    {
        std::lock_guard held(state->mutex);
        pending.swap(state->pending);
        removed.swap(state->removed);
        for (const std::string& repository : removed) {
            pending.erase(repository);
        }
    }
    if (pending.empty() && removed.empty()) {
        return {};
    }
    const WorkspaceAccess config = impl_->workspace_config();
    bool retry = false;
    Error last{"", ""};
    const auto keep = [&](std::set<std::string>& into, const std::string& repository, Error error) {
        std::lock_guard held(state->mutex);
        into.insert(repository);
        retry = true;
        last = std::move(error);
    };
    if (!removed.empty()) {
        auto lent = config.lend ? config.lend(customer, workspace, true)
                                : Result<BorrowedWorkspace>(std::unexpected(Error{"unsupported", "no workspace access"}));
        if (!lent) {
            for (const std::string& repository : removed) {
                keep(state->removed, repository, lent.error());
            }
        } else {
            std::lock_guard held(state->mutex);
            const auto ready = Impl::ensure_workspace(*state, lent->index);
            if (ready && *ready == Impl::Readiness::Ready) {
                auto next_active = state->data.active;
                std::erase_if(next_active, [&](const auto& item) { return removed.contains(item.second.repository); });
                auto next_artifacts = state->data.artifacts;
                std::erase_if(next_artifacts,
                              [&](const auto& item) { return removed.contains(item.second.repository); });
                if (next_active.size() != state->data.active.size() ||
                    next_artifacts.size() != state->data.artifacts.size()) {
                    ObjectStorage storage(lent->index.objects);
                    if (const auto committed = Impl::commit(storage, state->data, std::move(next_active),
                                                            std::move(next_artifacts), true, false);
                        !committed) {
                        state->loaded = false;
                        state->data = Impl::CustomerState{};
                        state->removed.insert(removed.begin(), removed.end());
                        retry = true;
                        last = committed.error();
                    }
                }
            } else {
                if (ready && *ready == Impl::Readiness::Schedule) {
                    state->building = false;
                }
                state->removed.insert(removed.begin(), removed.end());
                retry = true;
                last = ready ? Error{"store_unavailable", "the index is not ready"} : ready.error();
            }
        }
    }
    for (const std::string& repository : pending) {
        auto borrowed = impl_->borrow(customer, repository, true);
        if (!borrowed) {
            if (borrowed.error().code != "not_found") {
                keep(state->pending, repository, borrowed.error());
            }
            continue;
        }
        if (!borrowed->workspace || borrowed->workspace->workspace != workspace) {
            continue;
        }
        std::lock_guard held(state->mutex);
        const auto ready = Impl::ensure_workspace(*state, *borrowed->workspace);
        if (!ready || *ready != Impl::Readiness::Ready) {
            if (ready && *ready == Impl::Readiness::Schedule) {
                state->building = false;
            }
            state->pending.insert(repository);
            retry = true;
            last = ready ? Error{"store_unavailable", "the index is not ready"} : ready.error();
            continue;
        }
        if (auto refreshed = Impl::refresh_repository(*state, *borrowed->workspace, *borrowed->repository,
                                                      repository, true);
            !refreshed) {
            state->pending.insert(repository);
            retry = true;
            last = refreshed.error();
        }
    }
    impl_->trim();
    std::chrono::milliseconds delay{0};
    {
        std::lock_guard held(state->mutex);
        if (retry) {
            delay = impl_->next_pending_retry(*state);
        } else if (state->pending.empty() && state->removed.empty()) {
            state->pending_backoff = std::chrono::milliseconds(0);
        }
    }
    if (retry) {
        impl_->schedule(customer, workspace, *this, delay);
        return std::unexpected(last);
    }
    return {};
}

Result<void> Service::publish(const Publish& publication) {
    try {
        if (publication.customer.empty() || publication.repository.empty() ||
            publication.ref.empty() || publication.change.empty()) {
            return std::unexpected(
                Error{"invalid_input", "customer, repository, ref and change are required"});
        }
        std::optional<std::string> to_schedule;
        // lake#540: a failed index write leaves the repository pending, retried after this delay.
        std::optional<std::pair<std::string, std::chrono::milliseconds>> to_retry;
        Error write_error{"", ""};
        {
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
                    const auto ready = Impl::ensure_workspace(*state, index);
                    if (!ready) {
                        return std::unexpected(ready.error());
                    }
                    if (*ready != Impl::Readiness::Ready) {
                        // The rebuild reads this repository's releases, or indexes it again
                        // once it is done.
                        state->pending.insert(publication.repository);
                        if (*ready == Impl::Readiness::Schedule) {
                            to_schedule = index.workspace;
                        }
                    } else {
                        auto next_active = state->data.active;
                        const auto changed =
                            Impl::publish_into(*opened, state->data, publication, next_active);
                        if (!changed) {
                            return std::unexpected(changed.error());
                        }
                        if (*changed) {
                            ObjectStorage storage(index.objects);
                            const auto budget =
                                index.write_budget ? index.write_budget() : std::nullopt;
                            if (const auto committed =
                                    Impl::commit(storage, state->data, std::move(next_active),
                                                 state->data.artifacts, false, false, budget);
                                !committed) {
                                state->loaded = false;
                                state->data = Impl::CustomerState{};
                                // Kept pending, so search says the index is partly stale and
                                // the worker refreshes the repository after a growing delay.
                                state->pending.insert(publication.repository);
                                to_retry.emplace(index.workspace, impl_->next_pending_retry(*state));
                                write_error = committed.error();
                            }
                        }
                    }
                }
            } else {
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
            }
        }
        if (to_schedule) {
            impl_->schedule(publication.customer, *to_schedule, *this);
        }
        if (to_retry) {
            impl_->schedule(publication.customer, to_retry->first, *this, to_retry->second);
            return std::unexpected(std::move(write_error));
        }
        impl_->trim();
        // What the space's index held of it from before the switch goes.
        return drop_repository(publication.customer, publication.repository);
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

Result<void> Service::forget_repository(const std::string& customer,
                                        const std::string& workspace,
                                        const std::string& repository) {
    try {
        if (const auto dropped = drop_repository(customer, repository); !dropped) {
            return dropped;
        }
        const WorkspaceAccess config = impl_->workspace_config();
        if (workspace.empty() || !config.lend) {
            return {};
        }
        std::optional<std::string> to_schedule;
        {
            auto lent = config.lend(customer, workspace, false);
            if (!lent) {
                // A workspace that is not switched has no index of its own.
                if (lent.error().code == "invalid_input") {
                    return {};
                }
                // Otherwise the removal is kept and tried again later, so the repository never
                // stays in the index.
                std::chrono::milliseconds delay{0};
                {
                    const auto state = impl_->workspace_state(workspace);
                    std::lock_guard held(state->mutex);
                    state->removed.insert(repository);
                    state->pending.erase(repository);
                    delay = impl_->next_pending_retry(*state);
                }
                impl_->schedule(customer, workspace, *this, delay);
                return std::unexpected(lent.error());
            }
            const auto state = impl_->workspace_state(workspace);
            std::lock_guard held(state->mutex);
            state->pending.erase(repository);
            const auto ready = Impl::ensure_workspace(*state, lent->index);
            if (!ready) {
                state->removed.insert(repository);
                to_schedule = workspace;
            } else if (*ready != Impl::Readiness::Ready) {
                // A build in progress leaves it out and removes it once it is done.
                state->removed.insert(repository);
                if (*ready == Impl::Readiness::Schedule) {
                    to_schedule = workspace;
                }
            } else {
                auto next_active = state->data.active;
                std::erase_if(next_active,
                              [&](const auto& item) { return item.second.repository == repository; });
                auto next_artifacts = state->data.artifacts;
                std::erase_if(next_artifacts,
                              [&](const auto& item) { return item.second.repository == repository; });
                if (next_active.size() != state->data.active.size() ||
                    next_artifacts.size() != state->data.artifacts.size()) {
                    ObjectStorage storage(lent->index.objects);
                    if (const auto committed = Impl::commit(storage, state->data, std::move(next_active),
                                                            std::move(next_artifacts), true, false);
                        !committed) {
                        state->loaded = false;
                        state->data = Impl::CustomerState{};
                        state->removed.insert(repository);
                        to_schedule = workspace;
                    }
                }
            }
        }
        if (to_schedule) {
            impl_->schedule(customer, *to_schedule, *this);
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
                    state->data = Impl::CustomerState{};
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
        std::shared_ptr<Impl::IndexState> state;
        {
            std::lock_guard lock(impl_->mutex);
            impl_->dropped.insert(workspace);
            if (const auto found = impl_->workspaces.find(workspace); found != impl_->workspaces.end()) {
                state = found->second;
                impl_->workspaces.erase(found);
            }
            impl_->last_used.erase(workspace);
        }
        if (state) {
            std::lock_guard held(state->mutex);
            state->loaded = false;
            state->pending.clear();
            state->removed.clear();
            state->data = Impl::CustomerState{};
        }
        const WorkspaceAccess config = impl_->workspace_config();
        if (!config.lend_if_open) {
            return {};
        }
        auto lent = config.lend_if_open(workspace);
        if (!lent) {
            return std::unexpected(lent.error());
        }
        if (!*lent) {
            return {};
        }
        const auto listed = (*lent)->index.objects->list();
        if (!listed) {
            return std::unexpected(listed.error());
        }
        for (const std::string& name : *listed) {
            if (const auto removed = (*lent)->index.objects->remove(name); !removed) {
                return removed;
            }
        }
        return (*lent)->index.objects->sync();
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
        std::optional<std::string> to_schedule;
        std::optional<std::pair<std::string, std::chrono::milliseconds>> to_retry;
        Error write_error{"", ""};
        ReindexResult result;
        {
            auto borrowed = impl_->borrow(customer, repository);
            if (!borrowed) {
                return std::unexpected(borrowed.error());
            }
            liquid::repository* opened = borrowed->repository;
            if (!borrowed->workspace) {
                const auto state = impl_->space_state(customer);
                std::lock_guard held(state->mutex);
                if (const auto ensured = impl_->ensure_space(customer, *state); !ensured) {
                    return std::unexpected(ensured.error());
                }
                // A space's index follows every push and commit too (lake#584): the repository's
                // branch tips are published again as its references name them now, as in a
                // workspace's index below. Before, only a release or an explicit publication
                // reached the space's hot index, so a drive write or a push never did.
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
                DirectoryStorage storage(impl_->index_path(customer));
                if (const auto committed = Impl::commit(storage,
                                                        state->data,
                                                        std::move(next_active),
                                                        std::move(next_artifacts),
                                                        true,
                                                        false);
                    !committed) {
                    return std::unexpected(committed.error());
                }
                return *reindexed;
            }
            // In a workspace's index, reindexing a repository also publishes its branch tips again
            // as its references name them now, so the index follows every push (lake#381).
            const WorkspaceIndex& index = *borrowed->workspace;
            const auto state = impl_->workspace_state(index.workspace);
            std::lock_guard held(state->mutex);
            const auto ready = Impl::ensure_workspace(*state, index);
            if (!ready) {
                return std::unexpected(ready.error());
            }
            if (*ready != Impl::Readiness::Ready) {
                // Never rebuilt inside a request: the rebuild runs off the request path and
                // indexes this repository again once it is done.
                state->pending.insert(repository);
                if (*ready == Impl::Readiness::Schedule) {
                    to_schedule = index.workspace;
                }
            } else {
                bool write_failed = false;
                auto refreshed =
                    Impl::refresh_repository(*state, index, *opened, repository, false, &write_failed);
                if (!refreshed && !write_failed) {
                    return std::unexpected(refreshed.error());
                }
                if (!refreshed) {
                    // lake#540: kept pending, so search says the index is partly stale and the
                    // worker refreshes the repository after a growing delay.
                    state->pending.insert(repository);
                    to_retry.emplace(index.workspace, impl_->next_pending_retry(*state));
                    write_error = refreshed.error();
                } else {
                    result = *refreshed;
                }
            }
        }
        if (to_retry) {
            impl_->schedule(customer, to_retry->first, *this, to_retry->second);
            return std::unexpected(std::move(write_error));
        }
        if (to_schedule) {
            impl_->schedule(customer, *to_schedule, *this);
            return std::unexpected(Error{"store_unavailable",
                                         "the workspace's search index is being built; it will "
                                         "include this repository"});
        }
        impl_->trim();
        if (const auto dropped = drop_repository(customer, repository); !dropped) {
            return std::unexpected(dropped.error());
        }
        return result;
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<std::vector<ArtifactMember>> Service::artifact_members(const std::string& customer,
                                                              const std::string& repository,
                                                              const std::string& version) const {
    try {
        const WorkspaceAccess config = impl_->workspace_config();
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
        std::optional<std::string> route;
        if (config.route && config.lend) {
            const auto routed = config.route(customer, repository);
            if (!routed) {
                return std::unexpected(routed.error());
            }
            route = *routed;
        }
        if (route) {
            // Members of a workspace whose index is being built are not listed yet.
            const auto state = impl_->workspace_state(*route);
            {
                std::lock_guard held(state->mutex);
                if (state->building) {
                    return output;
                }
            }
            bool schedule = false;
            {
                auto lent = config.lend(customer, *route, false);
                if (!lent) {
                    return std::unexpected(lent.error());
                }
                std::lock_guard held(state->mutex);
                const auto ready = Impl::ensure_workspace(*state, lent->index);
                if (!ready) {
                    return std::unexpected(ready.error());
                }
                if (*ready == Impl::Readiness::Ready) {
                    read(state->data);
                }
                schedule = *ready == Impl::Readiness::Schedule;
            }
            if (schedule) {
                impl_->schedule(customer, *route, const_cast<Service&>(*this));
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
            if (const std::string problem = regex_pattern_problem(matcher.needle); !problem.empty()) {
                return std::unexpected(Error{"invalid_input", problem});
            }
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
        const auto permitted = [&](const std::string& space) {
            return !query.spaces || query.spaces->contains(space);
        };
        const Error not_searchable{"invalid_input",
                                   "workspace_id must name a workspace Zetta in this account"};

        // Which indexes the query reads: one workspace's, a repository's own, or a space's (or
        // every permitted space's) index on lake's volume with each of its switched workspaces'.
        std::vector<std::pair<std::string, std::string>> workspaces; // (space, workspace)
        bool read_spaces = true;
        bool named = false; // the workspace was named or follows from the repository
        if (!query.workspace.empty()) {
            if (!config.lend || (query.spaces && !permitted(query.customer))) {
                return std::unexpected(not_searchable);
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
            std::vector<std::string> customers;
            if (!query.customer.empty()) {
                customers.push_back(query.customer);
            } else if (query.spaces) {
                customers.assign(query.spaces->begin(), query.spaces->end());
            } else {
                customers.emplace_back();
            }
            for (const std::string& customer : customers) {
                if (!customer.empty() && !permitted(customer)) {
                    continue;
                }
                const auto listed = config.switched(customer);
                if (!listed) {
                    return std::unexpected(listed.error());
                }
                for (const auto& entry : *listed) {
                    if (permitted(entry.first)) {
                        workspaces.push_back(entry);
                    }
                }
            }
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
                std::vector<std::string> customers;
                if (!query.customer.empty()) {
                    customers.push_back(query.customer);
                } else {
                    for (const std::string& customer : impl_->space_directories()) {
                        if (permitted(customer)) {
                            customers.push_back(customer);
                        }
                    }
                }
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

        std::vector<std::pair<std::string, std::string>> to_schedule;
        for (const auto& [customer, workspace] : workspaces) {
            // An index being built is not waited for, and its store's lane is not taken.
            const auto state = impl_->workspace_state(workspace);
            {
                std::lock_guard held(state->mutex);
                if (state->building || (!state->loaded && std::chrono::steady_clock::now() < state->retry_after)) {
                    output.building_workspaces.push_back(workspace);
                    continue;
                }
            }
            // The workspace's lane is taken before its index's mutex, and no index's mutex is
            // held while waiting on it.
            std::optional<BorrowedRepository> scanned_repository;
            std::optional<BorrowedWorkspace> lent;
            if (matcher.scans_history) {
                auto borrowed = impl_->borrow(query.customer, query.repository);
                if (!borrowed) {
                    return std::unexpected(borrowed.error());
                }
                if (!borrowed->workspace || borrowed->workspace->workspace != workspace) {
                    return std::unexpected(Error{
                        "invalid_input", "the repository is not served by that workspace's store"});
                }
                scanned_repository = std::move(*borrowed);
            } else {
                auto borrowed = config.lend(customer, workspace, false);
                if (!borrowed) {
                    if (named) {
                        return std::unexpected(borrowed.error().code == "invalid_input" ||
                                                       borrowed.error().code == "not_found"
                                                   ? not_searchable
                                                   : borrowed.error());
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
            {
                std::lock_guard held(state->mutex);
                const auto ready = Impl::ensure_workspace(*state, index);
                if (!ready) {
                    if (named) {
                        return std::unexpected(ready.error());
                    }
                    output.skipped_workspaces.push_back(workspace);
                    continue;
                }
                if (*ready != Impl::Readiness::Ready) {
                    output.building_workspaces.push_back(workspace);
                    if (*ready == Impl::Readiness::Schedule) {
                        to_schedule.emplace_back(space, workspace);
                    }
                    continue;
                }
                // lake#540: the index answers, but without what a failed write left pending.
                if (!state->pending.empty() || !state->removed.empty()) {
                    output.stale_workspaces.push_back(workspace);
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
        }
        for (const auto& [customer, workspace] : to_schedule) {
            impl_->schedule(customer, workspace, const_cast<Service&>(*this));
        }
        impl_->trim();

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
