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
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

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

Result<void> write_json_atomically(const std::filesystem::path& path, const json& value) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
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
        output << value.dump();
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

Result<json> read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{"not_found", "index file is absent"});
    }
    json value;
    input >> value;
    return value;
}

} // namespace

std::string_view to_string(QueryMode mode) {
    switch (mode) {
    case QueryMode::Hot:
        return "hot";
    case QueryMode::Snapshot:
        return "snapshot";
    case QueryMode::Scan:
        return "scan";
    }
    return "hot";
}

struct Service::Impl {
    struct CustomerState {
        std::map<std::string, BlobRecord> blobs;
        std::map<std::string, Segment> active;
        std::unordered_map<std::string, std::vector<TrigramPosting>> trigram_index;
        std::vector<std::string> blob_documents;
        oramacore::index::MemoryIndex blob_index{{{"content", {1.0, 0.75}}}};
    };

    std::filesystem::path root;
    mutable std::shared_mutex mutex;
    std::map<std::string, CustomerState> customers;

    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}

    [[nodiscard]] std::filesystem::path index_path(const std::string& customer) const {
        return root / customer / "index";
    }

    void rebuild(CustomerState& state) {
        state.trigram_index.clear();
        state.blob_documents.clear();
        state.blob_index = oramacore::index::MemoryIndex({{"content", {1.0, 0.75}}});
        for (const auto& [digest, blob] : state.blobs) {
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
    }

    [[nodiscard]] Result<Segment> collect_snapshot(liquid::repository& repository,
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
                    state.blobs.emplace(
                        digest,
                        BlobRecord{std::string(reinterpret_cast<const char*>(contents->data()),
                                               contents->size())});
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

    [[nodiscard]] Result<void> save_blobs(const std::string& customer,
                                          const CustomerState& state) const {
        json blobs = json::object();
        for (const auto& [digest, record] : state.blobs) {
            blobs[digest] = {{"content", record.content}};
        }
        return write_json_atomically(index_path(customer) / "blobs.json", blobs);
    }

    struct ScanOutput {
        std::vector<Occurrence> occurrences;
        std::size_t changes = 0;
    };

    [[nodiscard]] Result<ScanOutput> scan(const Query& query, CustomerState& state) {
        const std::filesystem::path repository_path =
            root / query.customer / "repositories" / query.repository;
        auto customer_store =
            liquid::runtime::block_on(liquid::store::open(root / query.customer / "store"));
        if (!customer_store) {
            return std::unexpected(Error{"store_error", customer_store.error().message});
        }
        liquid::repository_options repository_options;
        repository_options.store = &*customer_store;
        auto opened = liquid::runtime::block_on(
            liquid::repository::open(repository_path, repository_options));
        if (!opened) {
            return std::unexpected(Error{"store_error", opened.error().message});
        }
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
        const std::size_t original_blob_count = state.blobs.size();
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
        if (state.blobs.size() != original_blob_count) {
            const auto saved = save_blobs(query.customer, state);
            if (!saved) {
                return std::unexpected(saved.error());
            }
            rebuild(state);
        }
        return output;
    }

    [[nodiscard]] Result<void> load_customer(const std::string& customer) {
        if (customers.contains(customer)) {
            return {};
        }
        CustomerState state;
        const std::filesystem::path directory = index_path(customer);
        std::error_code filesystem_error;
        if (!std::filesystem::exists(directory, filesystem_error)) {
            customers.emplace(customer, std::move(state));
            return {};
        }
        const auto blob_value = read_json(directory / "blobs.json");
        if (blob_value) {
            for (auto item = blob_value->begin(); item != blob_value->end(); ++item) {
                state.blobs.emplace(item.key(),
                                    BlobRecord{item.value().at("content").get<std::string>()});
            }
        }
        const auto manifest = read_json(directory / "manifest.json");
        if (manifest) {
            for (const json& active : manifest->at("active")) {
                const std::filesystem::path segment_path =
                    directory / active.at("segment").get<std::string>();
                const auto segment_value = read_json(segment_path);
                if (!segment_value) {
                    return std::unexpected(segment_value.error());
                }
                Segment segment;
                segment.repository = segment_value->at("repository").get<std::string>();
                segment.ref = segment_value->at("ref").get<std::string>();
                segment.change = segment_value->at("change").get<std::string>();
                segment.kind = publication_kind(*segment_value);
                segment.file = active.at("segment").get<std::string>();
                for (const json& value : segment_value->at("occurrences")) {
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
        rebuild(state);
        customers.emplace(customer, std::move(state));
        return {};
    }

    [[nodiscard]] Result<void> load_all_customers() {
        std::error_code error;
        if (!std::filesystem::exists(root, error)) {
            return {};
        }
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (error) {
                return std::unexpected(
                    Error{"store_error", "could not list customer stores: " + error.message()});
            }
            if (!entry.is_directory()) {
                continue;
            }
            const auto loaded = load_customer(entry.path().filename().string());
            if (!loaded) {
                return loaded;
            }
        }
        return {};
    }
};

Service::Service(std::filesystem::path store_root)
    : impl_(std::make_shared<Impl>(std::move(store_root))) {}

Result<void> Service::publish(const Publish& publication) {
    try {
        if (publication.customer.empty() || publication.repository.empty() ||
            publication.ref.empty() || publication.change.empty()) {
            return std::unexpected(
                Error{"invalid_input", "customer, repository, ref and change are required"});
        }
        std::unique_lock lock(impl_->mutex);
        const auto loaded = impl_->load_customer(publication.customer);
        if (!loaded) {
            return loaded;
        }
        auto& state = impl_->customers.at(publication.customer);
        const std::string key = slot_key(publication.kind, publication.repository, publication.ref);
        const auto existing = state.active.find(key);
        if (publication.kind == PublicationKind::TagSnapshot && existing != state.active.end()) {
            if (existing->second.change == publication.change) {
                return {};
            }
            return std::unexpected(
                Error{"conflict", "a tag snapshot cannot move to another change"});
        }
        const std::filesystem::path repository_path =
            impl_->root / publication.customer / "repositories" / publication.repository;
        auto customer_store = liquid::runtime::block_on(
            liquid::store::open(impl_->root / publication.customer / "store"));
        if (!customer_store) {
            return std::unexpected(Error{"store_error", customer_store.error().message});
        }
        liquid::repository_options repository_options;
        repository_options.store = &*customer_store;
        auto opened = liquid::runtime::block_on(
            liquid::repository::open(repository_path, repository_options));
        if (!opened) {
            return std::unexpected(Error{"store_error", opened.error().message});
        }
        const auto parsed = liquid::parse_digest(publication.change);
        if (!parsed) {
            return std::unexpected(Error{"invalid_input", parsed.error().message});
        }
        auto snapshot = liquid::runtime::block_on(opened->at(liquid::change_id{*parsed}));
        if (!snapshot) {
            return std::unexpected(Error{"not_found", snapshot.error().message});
        }
        auto segment = impl_->collect_snapshot(*opened,
                                               state,
                                               publication.repository,
                                               publication.ref,
                                               publication.change,
                                               publication.kind,
                                               snapshot->root);
        if (!segment) {
            return std::unexpected(segment.error());
        }

        const std::filesystem::path directory = impl_->index_path(publication.customer);
        const auto saved_blobs = impl_->save_blobs(publication.customer, state);
        if (!saved_blobs) {
            return saved_blobs;
        }
        json segment_value{{"version", 2},
                           {"repository", segment->repository},
                           {"ref", segment->ref},
                           {"change", segment->change},
                           {"kind", publication_kind_name(segment->kind)},
                           {"occurrences", json::array()}};
        for (const Occurrence& occurrence : segment->occurrences) {
            segment_value["occurrences"].push_back(
                {{"path", occurrence.path}, {"blob", occurrence.blob}});
        }
        const std::string file_name = segment_name(*segment);
        const auto saved_segment = write_json_atomically(directory / file_name, segment_value);
        if (!saved_segment) {
            return saved_segment;
        }
        segment->file = file_name;
        auto next_active = state.active;
        next_active[key] = std::move(*segment);
        json manifest{{"version", 2}, {"active", json::array()}};
        for (const auto& [slot, active] : next_active) {
            manifest["active"].push_back({{"repository", active.repository},
                                          {"ref", active.ref},
                                          {"change", active.change},
                                          {"kind", publication_kind_name(active.kind)},
                                          {"segment", active.file}});
        }
        const auto published = write_json_atomically(directory / "manifest.json", manifest);
        if (!published) {
            return published;
        }
        state.active = std::move(next_active);
        impl_->rebuild(state);
        return {};
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<QueryResult>
Service::query(const Query& query, const std::string& principal, control::Store& store) const {
    try {
        if (query.text.empty()) {
            return std::unexpected(Error{"invalid_input", "query is required"});
        }
        if (query.history && !query.change.empty()) {
            return std::unexpected(
                Error{"invalid_input", "history and change are mutually exclusive"});
        }
        const bool scans_history = query.history || !query.change.empty();
        if (scans_history &&
            (!query.ref.empty() || query.customer.empty() || query.repository.empty())) {
            return std::unexpected(Error{
                "invalid_input", "scan queries require customer and repository, without ref"});
        }
        std::unique_lock lock(impl_->mutex);
        const auto loaded = query.customer.empty() ? impl_->load_all_customers()
                                                   : impl_->load_customer(query.customer);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        const bool is_regex =
            query.text.size() > 2 && query.text.front() == '/' && query.text.back() == '/';
        const bool is_substring =
            query.text.size() > 1 && query.text.front() == '"' && query.text.back() == '"';
        const std::string needle = is_regex       ? query.text.substr(1, query.text.size() - 2)
                                   : is_substring ? query.text.substr(1, query.text.size() - 2)
                                                  : query.text;
        std::optional<std::regex> expression;
        if (is_regex) {
            try {
                expression.emplace(needle);
            } catch (const std::regex_error& error) {
                return std::unexpected(
                    Error{"invalid_input", "invalid regex: " + std::string(error.what())});
            }
        }
        QueryResult output;
        output.mode = scans_history ? QueryMode::Scan : QueryMode::Hot;
        for (auto& [customer, state] : impl_->customers) {
            if (!query.customer.empty() && query.customer != customer) {
                continue;
            }
            std::vector<Occurrence> selected;
            if (scans_history) {
                const auto scanned = impl_->scan(query, state);
                if (!scanned) {
                    return std::unexpected(scanned.error());
                }
                selected = std::move(scanned->occurrences);
                output.scanned_changes += scanned->changes;
            } else {
                for (const auto& [slot, segment] : state.active) {
                    if (!query.repository.empty() && query.repository != segment.repository) {
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
            std::unordered_map<std::string, double> scores;
            if (!is_regex && !is_substring) {
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
                const std::string prefilter = is_regex ? regex_literal(needle) : needle;
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
                        is_regex ? std::regex_search(blob->second.content, *expression)
                                 : blob->second.content.find(needle) != std::string::npos;
                    if (matched) {
                        scores[digest] = 1.0;
                    }
                }
            }
            struct Candidate {
                const Occurrence* occurrence;
                std::string policy_object_id;
            };
            std::vector<Candidate> candidates;
            std::vector<std::string> policy_object_ids;
            for (const Occurrence& occurrence : selected) {
                const auto score = scores.find(occurrence.blob);
                if (score == scores.end()) {
                    continue;
                }
                std::string policy_object_id = occurrence.repository;
                if (principal != "admin") {
                    const auto policy_object =
                        store.find_policy_object("repository", occurrence.repository);
                    if (!policy_object) {
                        if (policy_object.error().code == "not_found") {
                            continue;
                        }
                        return std::unexpected(
                            Error{policy_object.error().code, policy_object.error().message});
                    }
                    policy_object_id = policy_object->id;
                }
                candidates.push_back(Candidate{&occurrence, policy_object_id});
                policy_object_ids.push_back(policy_object_id);
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
            for (const Candidate& candidate : candidates) {
                const auto decision = decisions->find(candidate.policy_object_id);
                if (decision == decisions->end() || !decision->second.allowed) {
                    continue;
                }
                const Occurrence& occurrence = *candidate.occurrence;
                const auto blob = state.blobs.find(occurrence.blob);
                output.items.push_back(Hit{customer,
                                           occurrence.repository,
                                           occurrence.path,
                                           occurrence.change,
                                           snippet_for(blob->second.content, needle),
                                           scores.at(occurrence.blob)});
            }
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
