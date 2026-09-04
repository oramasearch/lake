#include "lake/search/index.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
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
};

struct Segment {
    std::string repository;
    std::string ref;
    std::string change;
    std::vector<Occurrence> occurrences;
};

struct BlobRecord {
    std::string content;
};

struct TrigramPosting {
    std::string blob;
    std::vector<std::size_t> positions;
};

std::string slot_key(const std::string& repository, const std::string& ref) {
    return repository + "\n" + ref;
}

std::string segment_name(const std::string& repository, const std::string& change) {
    return "segment-" + repository + "-" + change + ".json";
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

struct Service::Impl {
    struct CustomerState {
        std::map<std::string, BlobRecord> blobs;
        std::map<std::string, Segment> active;
        std::vector<Occurrence> occurrences;
        std::unordered_map<std::string, std::vector<TrigramPosting>> trigram_index;
        std::vector<std::string> blob_documents;
        std::vector<Occurrence> path_documents;
        oramacore::index::MemoryIndex blob_index{{{"content", {1.0, 0.75}}}};
        oramacore::index::MemoryIndex path_index{{{"path", {3.0, 0.75}}}};
    };

    std::filesystem::path root;
    mutable std::shared_mutex mutex;
    std::map<std::string, CustomerState> customers;

    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}

    [[nodiscard]] std::filesystem::path index_path(const std::string& customer) const {
        return root / customer / "index";
    }

    void rebuild(CustomerState& state) {
        state.occurrences.clear();
        state.trigram_index.clear();
        state.blob_documents.clear();
        state.path_documents.clear();
        state.blob_index = oramacore::index::MemoryIndex({{"content", {1.0, 0.75}}});
        state.path_index = oramacore::index::MemoryIndex({{"path", {3.0, 0.75}}});
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
        for (const auto& [slot, segment] : state.active) {
            for (const Occurrence& occurrence : segment.occurrences) {
                state.occurrences.push_back(occurrence);
                const auto document = state.path_index.add({{"path", occurrence.path}});
                if (state.path_documents.size() <= document) {
                    state.path_documents.resize(document + 1);
                }
                state.path_documents[document] = occurrence;
            }
        }
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
                for (const json& value : segment_value->at("occurrences")) {
                    segment.occurrences.push_back(Occurrence{segment.repository,
                                                             segment.ref,
                                                             segment.change,
                                                             value.at("path").get<std::string>(),
                                                             value.at("blob").get<std::string>()});
                }
                state.active.emplace(slot_key(segment.repository, segment.ref), std::move(segment));
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
        Segment segment{publication.repository, publication.ref, publication.change, {}};
        const auto walk = [&](const auto& self,
                              const liquid::tree& tree,
                              const std::string& prefix) -> Result<void> {
            for (const auto& entry : tree.entries) {
                const std::string path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
                if (entry.kind == liquid::node_kind::tree) {
                    auto nested = liquid::runtime::block_on(opened->tree(entry.ref));
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
                auto& state = impl_->customers.at(publication.customer);
                if (!state.blobs.contains(digest)) {
                    auto contents = liquid::runtime::block_on(opened->blob(entry.ref));
                    if (!contents) {
                        return std::unexpected(Error{"store_error", contents.error().message});
                    }
                    state.blobs.emplace(
                        digest,
                        BlobRecord{std::string(reinterpret_cast<const char*>(contents->data()),
                                               contents->size())});
                }
                segment.occurrences.push_back(Occurrence{
                    publication.repository, publication.ref, publication.change, path, digest});
            }
            return {};
        };
        const auto walked = walk(walk, snapshot->root, "");
        if (!walked) {
            return walked;
        }

        auto& state = impl_->customers.at(publication.customer);
        json blobs = json::object();
        for (const auto& [digest, record] : state.blobs) {
            blobs[digest] = {{"content", record.content}};
        }
        const std::filesystem::path directory = impl_->index_path(publication.customer);
        const auto saved_blobs = write_json_atomically(directory / "blobs.json", blobs);
        if (!saved_blobs) {
            return saved_blobs;
        }
        json segment_value{{"version", 1},
                           {"repository", segment.repository},
                           {"ref", segment.ref},
                           {"change", segment.change},
                           {"occurrences", json::array()}};
        for (const Occurrence& occurrence : segment.occurrences) {
            segment_value["occurrences"].push_back(
                {{"path", occurrence.path}, {"blob", occurrence.blob}});
        }
        const std::string file_name = segment_name(publication.repository, publication.change);
        const auto saved_segment = write_json_atomically(directory / file_name, segment_value);
        if (!saved_segment) {
            return saved_segment;
        }
        state.active[slot_key(publication.repository, publication.ref)] = std::move(segment);
        json manifest{{"version", 1}, {"active", json::array()}};
        for (const auto& [slot, active] : state.active) {
            manifest["active"].push_back(
                {{"repository", active.repository},
                 {"ref", active.ref},
                 {"change", active.change},
                 {"segment", segment_name(active.repository, active.change)}});
        }
        const auto published = write_json_atomically(directory / "manifest.json", manifest);
        if (!published) {
            return published;
        }
        impl_->rebuild(state);
        return {};
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

Result<std::vector<Hit>>
Service::query(const Query& query, const std::string& principal, control::Store& store) const {
    try {
        if (query.text.empty()) {
            return std::unexpected(Error{"invalid_input", "query is required"});
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
        std::vector<Hit> output;
        for (const auto& [customer, state] : impl_->customers) {
            if (!query.customer.empty() && query.customer != customer) {
                continue;
            }
            std::unordered_map<std::string, double> scores;
            if (!is_regex && !is_substring) {
                for (const auto& result : state.blob_index.search(needle, 0).hits) {
                    if (result.doc_id < state.blob_documents.size()) {
                        scores[state.blob_documents[result.doc_id]] += result.score;
                    }
                }
                for (const auto& result : state.path_index.search(needle, 0).hits) {
                    if (result.doc_id < state.path_documents.size()) {
                        scores[state.path_documents[result.doc_id].blob] += result.score;
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
            for (const Occurrence& occurrence : state.occurrences) {
                if (!query.repository.empty() && query.repository != occurrence.repository) {
                    continue;
                }
                if (!query.ref.empty() && query.ref != occurrence.ref) {
                    continue;
                }
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
                output.push_back(Hit{customer,
                                     occurrence.repository,
                                     occurrence.path,
                                     occurrence.change,
                                     snippet_for(blob->second.content, needle),
                                     scores.at(occurrence.blob)});
            }
        }
        std::sort(output.begin(), output.end(), [](const Hit& left, const Hit& right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.repository != right.repository) {
                return left.repository < right.repository;
            }
            return left.path < right.path;
        });
        const int limit = query.limit <= 0 ? 20 : std::min(query.limit, 1000);
        if (output.size() > static_cast<std::size_t>(limit)) {
            output.resize(static_cast<std::size_t>(limit));
        }
        return output;
    } catch (const std::exception& error) {
        return std::unexpected(Error{"store_error", error.what()});
    }
}

} // namespace lake::search
