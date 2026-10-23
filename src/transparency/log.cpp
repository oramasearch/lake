#include "lake/transparency/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

#include <fcntl.h>
#include <openssl/evp.h>
#include <sys/file.h>
#include <unistd.h>

#include <liquid/attestation.hpp>
#include <liquid/identity.hpp>

#include "lake/audit/audit.hpp"
#include "objects/records.hpp"

namespace lake::transparency {
namespace {

using Bytes = std::vector<std::byte>;
using Hash = std::array<std::byte, 32>;
using nlohmann::json;

constexpr std::string_view tile_header = "LAKE-TILE-1\n";
constexpr std::string_view hash_tile_header = "LAKE-HASH-TILE-1\n";
constexpr std::string_view audit_entry_header = "LAKE-AUDIT-EVENT-1\n";
constexpr std::string_view audit_commitment_header = "LAKE-AUDIT-COMMITMENT-1\n";

class AppendLock {
public:
    explicit AppendLock(int descriptor) : descriptor_(descriptor) {}

    ~AppendLock() {
        if (descriptor_ >= 0) {
            static_cast<void>(flock(descriptor_, LOCK_UN));
            static_cast<void>(close(descriptor_));
        }
    }

    AppendLock(AppendLock&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    AppendLock& operator=(AppendLock&&) = delete;
    AppendLock(const AppendLock&) = delete;
    AppendLock& operator=(const AppendLock&) = delete;

    static Result<AppendLock> acquire(const std::filesystem::path& path) {
        const int descriptor = open(path.c_str(), O_CREAT | O_RDWR, 0600);
        if (descriptor < 0) {
            return std::unexpected(Error{"store_error",
                                         "could not open transparency append lock: " +
                                             std::string(std::strerror(errno))});
        }
        if (flock(descriptor, LOCK_EX) != 0) {
            const std::string message = std::strerror(errno);
            static_cast<void>(close(descriptor));
            return std::unexpected(
                Error{"store_error", "could not acquire transparency append lock: " + message});
        }
        return AppendLock(descriptor);
    }

private:
    int descriptor_ = -1;
};

bool valid_instance_name(std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find('/') == std::string_view::npos && value.find('\\') == std::string_view::npos;
}

std::string hex(const Hash& value) {
    std::string output;
    output.reserve(value.size() * 2);
    for (const std::byte byte : value) {
        output += std::format("{:02x}", std::to_integer<unsigned int>(byte));
    }
    return output;
}

Result<Hash> decode_hash(std::string_view value) {
    if (value.size() != 64) {
        return std::unexpected(Error{"store_error", "stored transparency hash has invalid length"});
    }
    Hash output{};
    const auto nibble = [](char digit) -> int {
        if (digit >= '0' && digit <= '9') {
            return digit - '0';
        }
        if (digit >= 'a' && digit <= 'f') {
            return digit - 'a' + 10;
        }
        if (digit >= 'A' && digit <= 'F') {
            return digit - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0; index < output.size(); ++index) {
        const int high = nibble(value[index * 2]);
        const int low = nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(
                Error{"store_error", "stored transparency hash is not hexadecimal"});
        }
        output[index] = std::byte{static_cast<unsigned char>((high << 4) | low)};
    }
    return output;
}

Result<Hash> sha256(std::span<const std::byte> value) {
    Hash output{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(),
                   value.size(),
                   reinterpret_cast<unsigned char*>(output.data()),
                   &size,
                   EVP_sha256(),
                   nullptr) != 1 ||
        size != output.size()) {
        return std::unexpected(Error{"store_error", "could not calculate transparency SHA-256"});
    }
    return output;
}

Result<Hash> sha512_256(std::span<const std::byte> value) {
    Hash output{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(),
                   value.size(),
                   reinterpret_cast<unsigned char*>(output.data()),
                   &size,
                   EVP_sha512_256(),
                   nullptr) != 1 ||
        size != output.size()) {
        return std::unexpected(Error{"store_error", "could not calculate audit SHA-512/256"});
    }
    return output;
}

Result<Hash> leaf_hash(std::span<const std::byte> value) {
    Bytes input;
    input.reserve(value.size() + 1);
    input.push_back(std::byte{0});
    input.insert(input.end(), value.begin(), value.end());
    return sha256(input);
}

Result<Hash> node_hash(const Hash& left, const Hash& right) {
    Bytes input;
    input.reserve(1 + left.size() + right.size());
    input.push_back(std::byte{1});
    input.insert(input.end(), left.begin(), left.end());
    input.insert(input.end(), right.begin(), right.end());
    return sha256(input);
}

Result<Hash> empty_root() {
    return sha256({});
}

Result<std::string> base64url_encode(std::span<const std::byte> value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(Error{"invalid_input", "attestation is too large"});
    }
    std::string output(((value.size() + 2) / 3) * 4, '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                     reinterpret_cast<const unsigned char*>(value.data()),
                                     static_cast<int>(value.size()));
    if (size < 0) {
        return std::unexpected(Error{"store_error", "could not encode transparency bytes"});
    }
    output.resize(static_cast<std::size_t>(size));
    std::ranges::replace(output, '+', '-');
    std::ranges::replace(output, '/', '_');
    while (output.ends_with('=')) {
        output.pop_back();
    }
    return output;
}

Result<Bytes> base64url_decode(std::string value) {
    if (value.empty()) {
        return std::unexpected(Error{"invalid_input", "attestation must not be empty"});
    }
    std::ranges::replace(value, '-', '+');
    std::ranges::replace(value, '_', '/');
    const std::size_t remainder = value.size() % 4;
    if (remainder == 1) {
        return std::unexpected(Error{"invalid_input", "attestation is not valid base64url"});
    }
    if (remainder != 0) {
        value.append(4 - remainder, '=');
    }
    Bytes output((value.size() / 4) * 3);
    const int decoded = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(output.data()),
                                        reinterpret_cast<const unsigned char*>(value.data()),
                                        static_cast<int>(value.size()));
    if (decoded < 0) {
        return std::unexpected(Error{"invalid_input", "attestation is not valid base64url"});
    }
    std::size_t size = static_cast<std::size_t>(decoded);
    if (value.ends_with("==")) {
        size -= 2;
    } else if (value.ends_with('=')) {
        --size;
    }
    output.resize(size);
    return output;
}

void append_u32(Bytes& output, std::uint32_t value) {
    output.push_back(std::byte{static_cast<unsigned char>((value >> 24) & 0xff)});
    output.push_back(std::byte{static_cast<unsigned char>((value >> 16) & 0xff)});
    output.push_back(std::byte{static_cast<unsigned char>((value >> 8) & 0xff)});
    output.push_back(std::byte{static_cast<unsigned char>(value & 0xff)});
}

Result<Bytes> encode_tile(const std::vector<Bytes>& entries) {
    Bytes output;
    output.reserve(tile_header.size());
    for (const char value : tile_header) {
        output.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    for (const Bytes& entry : entries) {
        if (entry.size() > std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(Error{"invalid_input", "attestation is too large"});
        }
        append_u32(output, static_cast<std::uint32_t>(entry.size()));
        output.insert(output.end(), entry.begin(), entry.end());
    }
    return output;
}

Result<std::vector<Bytes>> decode_tile(std::span<const std::byte> encoded,
                                       std::size_t expected_entries) {
    if (encoded.size() < tile_header.size()) {
        return std::unexpected(Error{"unrecoverable", "transparency tile header is truncated"});
    }
    for (std::size_t index = 0; index < tile_header.size(); ++index) {
        if (encoded[index] != std::byte{static_cast<unsigned char>(tile_header[index])}) {
            return std::unexpected(Error{"unrecoverable", "transparency tile header is invalid"});
        }
    }
    std::vector<Bytes> entries;
    std::size_t offset = tile_header.size();
    while (offset < encoded.size()) {
        if (encoded.size() - offset < 4) {
            return std::unexpected(Error{"unrecoverable", "transparency tile record is truncated"});
        }
        const auto at = [&encoded, &offset](std::size_t distance) {
            return std::to_integer<std::uint32_t>(encoded[offset + distance]);
        };
        const std::uint32_t length = (at(0) << 24) | (at(1) << 16) | (at(2) << 8) | at(3);
        offset += 4;
        if (length == 0 || length > encoded.size() - offset) {
            return std::unexpected(Error{"unrecoverable", "transparency tile record is invalid"});
        }
        entries.emplace_back(encoded.begin() + static_cast<std::ptrdiff_t>(offset),
                             encoded.begin() + static_cast<std::ptrdiff_t>(offset + length));
        offset += length;
    }
    if (entries.size() != expected_entries) {
        return std::unexpected(
            Error{"unrecoverable", "transparency tile contains an unexpected entry count"});
    }
    return entries;
}

Result<Bytes> encode_hash_tile(const std::vector<Hash>& hashes) {
    Bytes output;
    output.reserve(hash_tile_header.size() + hashes.size() * Hash{}.size());
    for (const char value : hash_tile_header) {
        output.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    for (const Hash& hash : hashes) {
        output.insert(output.end(), hash.begin(), hash.end());
    }
    return output;
}

Result<std::vector<Hash>> decode_hash_tile(std::span<const std::byte> encoded,
                                           std::size_t expected_hashes) {
    const std::size_t expected_size = hash_tile_header.size() + expected_hashes * Hash{}.size();
    if (encoded.size() != expected_size) {
        return std::unexpected(
            Error{"unrecoverable", "transparency hash tile has an invalid size"});
    }
    for (std::size_t index = 0; index < hash_tile_header.size(); ++index) {
        if (encoded[index] != std::byte{static_cast<unsigned char>(hash_tile_header[index])}) {
            return std::unexpected(
                Error{"unrecoverable", "transparency hash tile header is invalid"});
        }
    }
    std::vector<Hash> hashes(expected_hashes);
    std::size_t offset = hash_tile_header.size();
    for (Hash& hash : hashes) {
        std::ranges::copy_n(
            encoded.begin() + static_cast<std::ptrdiff_t>(offset), hash.size(), hash.begin());
        offset += hash.size();
    }
    return hashes;
}

Result<Bytes> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{"not_found", "transparency tile does not exist"});
    }
    input.seekg(0, std::ios::end);
    const std::streamoff end = input.tellg();
    if (end < 0) {
        return std::unexpected(Error{"store_error", "could not size transparency tile"});
    }
    input.seekg(0, std::ios::beg);
    Bytes output(static_cast<std::size_t>(end));
    input.read(reinterpret_cast<char*>(output.data()), end);
    if (!input && end != 0) {
        return std::unexpected(Error{"store_error", "could not read transparency tile"});
    }
    return output;
}

Result<void> write_bytes(const std::filesystem::path& path, std::span<const std::byte> value) {
    if (std::filesystem::exists(path)) {
        const auto existing = read_bytes(path);
        if (existing && std::ranges::equal(*existing, value)) {
            return {};
        }
        return std::unexpected(Error{"conflict", "immutable transparency tile already exists"});
    }
    static std::atomic<std::uint64_t> next_temporary{0};
    const std::filesystem::path temporary = path.string() + ".tmp-" +
                                            std::to_string(static_cast<std::uint64_t>(getpid())) +
                                            "-" + std::to_string(++next_temporary);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(Error{"store_error", "could not create transparency tile"});
        }
        output.write(reinterpret_cast<const char*>(value.data()),
                     static_cast<std::streamsize>(value.size()));
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return std::unexpected(Error{"store_error", "could not write transparency tile"});
        }
    }
    std::error_code error;
    std::filesystem::create_hard_link(temporary, path, error);
    std::filesystem::remove(temporary);
    if (error) {
        const auto existing = read_bytes(path);
        if (existing && std::ranges::equal(*existing, value)) {
            return {};
        }
        return std::unexpected(
            Error{"store_error", "could not publish transparency tile: " + error.message()});
    }
    return {};
}

Result<json> read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{"not_found", "transparency log has no entries"});
    }
    json value = json::parse(input, nullptr, false);
    if (value.is_discarded()) {
        return std::unexpected(Error{"store_error", "transparency tree state is not valid JSON"});
    }
    return value;
}

Result<void> write_json(const std::filesystem::path& path, const json& value) {
    static std::atomic<std::uint64_t> next_temporary{0};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++next_temporary);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(
                Error{"store_error", "could not create transparency tree state"});
        }
        output << value.dump();
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return std::unexpected(Error{"store_error", "could not write transparency tree state"});
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return std::unexpected(
            Error{"store_error", "could not publish transparency tree state: " + error.message()});
    }
    return {};
}

struct StoredState {
    std::uint64_t tree_size = 0;
    Hash root{};
    std::vector<std::optional<Hash>> frontier;
};

using ParentLevels = std::vector<std::vector<Hash>>;

Result<Hash> frontier_root(const std::vector<std::optional<Hash>>& frontier) {
    std::optional<Hash> root;
    for (const std::optional<Hash>& peak : frontier) {
        if (!peak) {
            continue;
        }
        if (!root) {
            root = *peak;
            continue;
        }
        const auto combined = node_hash(*peak, *root);
        if (!combined) {
            return std::unexpected(combined.error());
        }
        root = *combined;
    }
    if (root) {
        return *root;
    }
    return empty_root();
}

Result<void> add_leaf(std::vector<std::optional<Hash>>& frontier,
                      const Hash& leaf,
                      ParentLevels* parent_levels = nullptr) {
    Hash carried = leaf;
    std::size_t level = 0;
    while (true) {
        if (level == frontier.size()) {
            frontier.push_back(carried);
            return {};
        }
        if (!frontier[level]) {
            frontier[level] = carried;
            return {};
        }
        const auto parent = node_hash(*frontier[level], carried);
        if (!parent) {
            return std::unexpected(parent.error());
        }
        frontier[level].reset();
        carried = *parent;
        ++level;
        if (parent_levels != nullptr) {
            if (parent_levels->size() <= level) {
                parent_levels->resize(level + 1);
            }
            parent_levels->at(level).push_back(carried);
        }
    }
}

Result<StoredState> stored_state(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        const auto root = empty_root();
        if (!root) {
            return std::unexpected(root.error());
        }
        return StoredState{.root = *root};
    }
    const auto document = read_json(path);
    if (!document || !document->is_object() || document->value("version", "") != "lake_tree/1" ||
        !document->contains("tree_size") || !document->at("tree_size").is_number_unsigned() ||
        !document->contains("root_hash") || !document->at("root_hash").is_string() ||
        !document->contains("frontier") || !document->at("frontier").is_array()) {
        return std::unexpected(Error{"store_error", "transparency tree state is malformed"});
    }
    StoredState state;
    state.tree_size = document->at("tree_size").get<std::uint64_t>();
    const auto root = decode_hash(document->at("root_hash").get<std::string>());
    if (!root) {
        return std::unexpected(root.error());
    }
    state.root = *root;
    for (const json& value : document->at("frontier")) {
        if (value.is_null()) {
            state.frontier.emplace_back();
            continue;
        }
        if (!value.is_string()) {
            return std::unexpected(Error{"store_error", "transparency frontier is malformed"});
        }
        const auto decoded = decode_hash(value.get<std::string>());
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        state.frontier.push_back(*decoded);
    }
    const auto recomputed = frontier_root(state.frontier);
    if (!recomputed || *recomputed != state.root) {
        return std::unexpected(
            Error{"unrecoverable", "transparency tree state root is inconsistent"});
    }
    return state;
}

json state_json(const StoredState& state) {
    json frontier = json::array();
    for (const std::optional<Hash>& value : state.frontier) {
        if (value) {
            frontier.push_back(hex(*value));
        } else {
            frontier.push_back(nullptr);
        }
    }
    return {{"frontier", std::move(frontier)},
            {"root_hash", hex(state.root)},
            {"tile_width", transparency_tile_width},
            {"tree_size", state.tree_size},
            {"version", "lake_tree/1"}};
}

struct VerifiedAttestation {
    std::string profile;
    std::string issuer;
    std::string subject;
    std::vector<std::string> identities;
};

void add_identity(std::vector<std::string>& identities, const std::string& value) {
    if (!value.starts_with("taap:1:") || std::ranges::find(identities, value) != identities.end()) {
        return;
    }
    identities.push_back(value);
}

// Historical full audit entries remain readable, but new admission permits only commitments.
Result<VerifiedAttestation> verify_attestation(std::span<const std::byte> encoded,
                                               bool allow_legacy_audit = false) {
    const auto has_header = [encoded](std::string_view header) {
        return encoded.size() >= header.size() &&
               std::ranges::equal(
                   encoded.first(header.size()), header, [](std::byte left, char right) {
                       return left == std::byte{static_cast<unsigned char>(right)};
                   });
    };
    if (has_header(audit_commitment_header)) {
        const auto body = encoded.subspan(audit_commitment_header.size());
        const json commitment = json::from_cbor(body, true, false);
        if (!commitment.is_object() || commitment.size() != 1 || !commitment.contains("digest") ||
            !commitment.at("digest").is_string()) {
            return std::unexpected(Error{"invalid_input", "audit commitment is malformed"});
        }
        const std::string digest = commitment.at("digest");
        if (digest.size() != 64 || !std::ranges::all_of(digest, [](char digit) {
                return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
            })) {
            return std::unexpected(Error{"invalid_input", "audit commitment digest is malformed"});
        }
        // Re-encoding rejects duplicate keys and other encodings that hide unparsed bytes.
        const auto canonical = json::to_cbor(commitment);
        if (!std::ranges::equal(body, canonical, [](std::byte left, std::uint8_t right) {
                return std::to_integer<std::uint8_t>(left) == right;
            })) {
            return std::unexpected(Error{"invalid_input", "audit commitment is not canonical"});
        }
        return VerifiedAttestation{.profile = "audit_commitment/1"};
    }
    if (has_header(audit_entry_header)) {
        if (!allow_legacy_audit) {
            return std::unexpected(
                Error{"invalid_input", "private audit events cannot be appended to a public log"});
        }
        const auto body = encoded.subspan(audit_entry_header.size());
        const json event = json::from_cbor(body, true, false);
        if (!event.is_object() || event.value("version", "") != "audit_event/1" ||
            !event.contains("digest") || !event.at("digest").is_string() ||
            !event.contains("signer") || !event.at("signer").is_string() ||
            !event.contains("principal") || !event.at("principal").is_string() ||
            !event.contains("object") || !event.at("object").is_string()) {
            return std::unexpected(Error{"invalid_input", "audit event entry is malformed"});
        }
        json stored_event = event;
        const std::string expected_digest = stored_event.at("digest");
        stored_event.erase("digest");
        const std::vector<std::uint8_t> stored_bytes = json::to_cbor(stored_event);
        const auto digest = sha512_256(std::as_bytes(std::span(stored_bytes)));
        if (!digest || hex(*digest) != expected_digest) {
            return std::unexpected(
                Error{"invalid_input", "audit event entry digest does not verify"});
        }
        VerifiedAttestation output{.profile = "audit_event/1",
                                   .issuer = event.at("signer"),
                                   .subject = event.at("object")};
        add_identity(output.identities, output.issuer);
        add_identity(output.identities, event.at("principal"));
        return output;
    }
    const auto statement = liquid::decode_attestation(encoded);
    if (!statement) {
        return std::unexpected(
            Error{"invalid_input",
                  "entry is not a canonical TAAP attestation: " + statement.error().message});
    }
    // The log records statements; it does not attribute them. An entry must be a registered,
    // well-formed attestation whose signature verifies under the key it carries, the same
    // integrity bar audit entries meet. Whether that key belongs to the named issuer is decided
    // by whoever reads the entry, against the issuer's identity chain (liquid#383): Lake holds no
    // chain for most issuers, and an identity scan must still list statements made in a name so
    // that its owner can see forged ones. Entries accepted before liquid#383 met the same bar.
    if (const auto valid = liquid::attestation_profile_registry::global().validate(*statement);
        !valid) {
        return std::unexpected(
            Error{"invalid_input", "TAAP attestation is invalid: " + valid.error().message});
    }
    const auto signature = liquid::decode_signature(statement->signature);
    const auto body = liquid::objects::encode_attestation_body_unchecked(*statement);
    if (!signature || !body) {
        return std::unexpected(
            Error{"invalid_input",
                  "TAAP attestation is invalid: " +
                      (signature ? body.error().message : signature.error().message)});
    }
    const auto verified =
        liquid::verify_key_signature(statement->issuer, "attestation", *body, *signature);
    if (!verified || !*verified) {
        return std::unexpected(Error{"invalid_input",
                                     "TAAP attestation is invalid: " +
                                         (verified ? std::string{"invalid attestation signature"}
                                                   : verified.error().message)});
    }
    VerifiedAttestation output{
        .profile = statement->profile,
        .issuer = statement->issuer.taap_id,
        .subject = liquid::to_string(statement->subject),
    };
    add_identity(output.identities, statement->issuer.taap_id);
    for (const liquid::attestation_attribute& attribute : statement->attributes) {
        if (const auto* text = std::get_if<std::string>(&attribute.value)) {
            add_identity(output.identities, *text);
        } else if (const auto* list = std::get_if<std::vector<std::string>>(&attribute.value)) {
            for (const std::string& item : *list) {
                add_identity(output.identities, item);
            }
        }
    }
    return output;
}

// The root of one complete tile's entries, kept so that a scan can check a tile it reads again.
Result<Hash> tile_root(const std::vector<Hash>& leaves) {
    std::vector<std::optional<Hash>> frontier;
    for (const Hash& leaf : leaves) {
        const auto added = add_leaf(frontier, leaf);
        if (!added) {
            return std::unexpected(added.error());
        }
    }
    return frontier_root(frontier);
}

// What identity scans have verified so far, kept across calls (lake#941): the first `tree_size`
// entries, the Merkle frontier after them, the root of each complete tile, the leaf hashes of the
// incomplete last tile, and the entries naming each identity. A scan verifies only entries past
// `tree_size` and checks any tile it reads again against what was verified.
struct IdentityIndex {
    std::uint64_t tree_size = 0;
    std::vector<std::optional<Hash>> frontier;
    std::vector<Hash> tile_roots;
    std::vector<Hash> tail_leaves;
    std::unordered_map<std::string, std::vector<std::uint64_t>> entries;
};

Result<std::vector<Bytes>> validated_entries(const std::vector<std::string>& attestations) {
    std::vector<Bytes> decoded;
    decoded.reserve(attestations.size());
    std::unordered_map<std::string, VerifiedAttestation> verification_cache;
    for (const std::string& encoded : attestations) {
        const auto bytes = base64url_decode(encoded);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        if (bytes->size() > 1024 * 1024) {
            return std::unexpected(Error{"invalid_input", "one attestation may not exceed 1 MiB"});
        }
        const auto digest = sha256(*bytes);
        if (!digest) {
            return std::unexpected(digest.error());
        }
        const std::string key = hex(*digest);
        if (!verification_cache.contains(key)) {
            const auto verified = verify_attestation(*bytes, false);
            if (!verified) {
                return std::unexpected(verified.error());
            }
            verification_cache.emplace(key, *verified);
        }
        decoded.push_back(*bytes);
    }
    return decoded;
}

} // namespace

struct TiledLog::Impl {
    std::filesystem::path directory;
    bool valid_instance = false;
    mutable std::mutex mutex;
    // Guards `identity_index` only; a scan never takes `mutex`, so it never holds up an append.
    mutable std::mutex index_mutex;
    mutable IdentityIndex identity_index;

    Impl(std::filesystem::path store_root, std::string instance_name) {
        valid_instance = valid_instance_name(instance_name);
        if (valid_instance) {
            directory = std::move(store_root) / std::move(instance_name) / "log";
        }
    }

    std::filesystem::path state_path() const {
        return directory / "tree.json";
    }

    std::filesystem::path tile_path(std::uint64_t index, std::size_t width) const {
        return directory / "tiles" / "entries" / std::to_string(index) /
               (std::to_string(width) + ".tile");
    }

    std::filesystem::path
    hash_tile_path(std::uint32_t level, std::uint64_t index, std::size_t width) const {
        return directory / "tiles" / "hashes" / std::to_string(level) / std::to_string(index) /
               (std::to_string(width) + ".tile");
    }

    Result<std::vector<Bytes>> load_tile(std::uint64_t index, std::size_t width) const {
        const auto bytes = read_bytes(tile_path(index, width));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return decode_tile(*bytes, width);
    }

    Result<void> publish_tile(std::uint64_t index, const std::vector<Bytes>& entries) const {
        const auto encoded = encode_tile(entries);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        std::error_code error;
        const std::filesystem::path path = tile_path(index, entries.size());
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return std::unexpected(
                Error{"store_error", "could not create transparency tile: " + error.message()});
        }
        return write_bytes(path, *encoded);
    }

    Result<std::vector<Hash>>
    load_hash_tile(std::uint32_t level, std::uint64_t index, std::size_t width) const {
        const auto bytes = read_bytes(hash_tile_path(level, index, width));
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        return decode_hash_tile(*bytes, width);
    }

    Result<std::vector<Hash>> load_or_materialize_hash_tile(std::uint32_t level,
                                                            std::uint64_t index,
                                                            std::size_t width) const {
        const auto exact = load_hash_tile(level, index, width);
        if (exact || exact.error().code != "not_found") {
            return exact;
        }
        const std::filesystem::path directory_path =
            hash_tile_path(level, index, width).parent_path();
        std::error_code error;
        std::filesystem::directory_iterator entries(directory_path, error);
        if (error) {
            return std::unexpected(exact.error());
        }
        std::optional<std::size_t> source_width;
        const std::filesystem::directory_iterator end;
        for (; entries != end; entries.increment(error)) {
            const auto& entry = *entries;
            if (!entry.is_regular_file(error)) {
                if (error) {
                    return std::unexpected(
                        Error{"store_error",
                              "could not inspect transparency hash tile: " + error.message()});
                }
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (!name.ends_with(".tile")) {
                continue;
            }
            const std::string_view encoded_width(name.data(), name.size() - 5);
            std::size_t candidate = 0;
            const auto parsed =
                std::from_chars(encoded_width.begin(), encoded_width.end(), candidate);
            if (parsed.ec != std::errc{} || parsed.ptr != encoded_width.end() ||
                candidate <= width || candidate > transparency_tile_width) {
                continue;
            }
            if (!source_width || candidate < *source_width) {
                source_width = candidate;
            }
        }
        if (error) {
            return std::unexpected(
                Error{"store_error", "could not list transparency hash tiles: " + error.message()});
        }
        if (!source_width) {
            return std::unexpected(exact.error());
        }
        auto source = load_hash_tile(level, index, *source_width);
        if (!source) {
            return std::unexpected(source.error());
        }
        source->resize(width);
        const auto published = publish_hash_tile(level, index, *source);
        if (!published) {
            return std::unexpected(published.error());
        }
        return *source;
    }

    Result<void> publish_hash_tile(std::uint32_t level,
                                   std::uint64_t index,
                                   const std::vector<Hash>& hashes) const {
        const auto encoded = encode_hash_tile(hashes);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        std::error_code error;
        const std::filesystem::path path = hash_tile_path(level, index, hashes.size());
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return std::unexpected(Error{
                "store_error", "could not create transparency hash tile: " + error.message()});
        }
        return write_bytes(path, *encoded);
    }

    Result<std::pair<StoredState, std::vector<std::string>>>
    replay(std::uint64_t end,
           std::uint64_t source_tree_size,
           std::optional<std::uint64_t> collect_from = std::nullopt,
           ParentLevels* parent_levels = nullptr) const {
        StoredState replayed;
        const auto initial_root = empty_root();
        if (!initial_root) {
            return std::unexpected(initial_root.error());
        }
        replayed.root = *initial_root;
        std::vector<std::string> collected;
        const std::uint64_t tile_count =
            end == 0 ? 0 : (end + transparency_tile_width - 1) / transparency_tile_width;
        for (std::uint64_t tile_index = 0; tile_index < tile_count; ++tile_index) {
            const std::uint64_t tile_start = tile_index * transparency_tile_width;
            const std::size_t stored_width = static_cast<std::size_t>(
                std::min<std::uint64_t>(source_tree_size - tile_start, transparency_tile_width));
            const auto entries = load_tile(tile_index, stored_width);
            if (!entries) {
                return std::unexpected(entries.error());
            }
            const std::size_t used = static_cast<std::size_t>(
                std::min<std::uint64_t>(end - tile_start, entries->size()));
            for (std::size_t offset = 0; offset < used; ++offset) {
                const auto digest = leaf_hash(entries->at(offset));
                if (!digest) {
                    return std::unexpected(digest.error());
                }
                const auto added = add_leaf(replayed.frontier, *digest, parent_levels);
                if (!added) {
                    return std::unexpected(added.error());
                }
                const std::uint64_t entry_index = tile_start + offset;
                if (collect_from && entry_index >= *collect_from) {
                    const auto encoded = base64url_encode(entries->at(offset));
                    if (!encoded) {
                        return std::unexpected(encoded.error());
                    }
                    collected.push_back(*encoded);
                }
                ++replayed.tree_size;
            }
        }
        const auto root = frontier_root(replayed.frontier);
        if (!root) {
            return std::unexpected(root.error());
        }
        replayed.root = *root;
        return std::pair{std::move(replayed), std::move(collected)};
    }

    static std::size_t tile_width_at(std::uint64_t value_count, std::uint64_t tile_index) {
        if (value_count == 0) {
            return 0;
        }
        const std::uint64_t tile_count = (value_count - 1) / transparency_tile_width + 1;
        if (tile_index >= tile_count) {
            return 0;
        }
        if (tile_index + 1 < tile_count) {
            return transparency_tile_width;
        }
        return static_cast<std::size_t>(value_count - tile_index * transparency_tile_width);
    }

    Result<std::size_t> ensure_hash_tiles(const StoredState& state) const {
        bool missing = false;
        for (std::uint32_t level = 1; level < 64 && (state.tree_size >> level) != 0; ++level) {
            const std::uint64_t hash_count = state.tree_size >> level;
            const std::uint64_t tile_count = (hash_count - 1) / transparency_tile_width + 1;
            for (std::uint64_t tile_index = 0; tile_index < tile_count; ++tile_index) {
                const std::size_t width = tile_width_at(hash_count, tile_index);
                std::error_code error;
                const bool exists =
                    std::filesystem::exists(hash_tile_path(level, tile_index, width), error);
                if (error) {
                    return std::unexpected(
                        Error{"store_error",
                              "could not inspect transparency hash tile: " + error.message()});
                }
                missing = missing || !exists;
            }
        }
        if (!missing) {
            return 0;
        }
        ParentLevels parent_levels;
        const auto replayed =
            replay(state.tree_size, state.tree_size, std::nullopt, &parent_levels);
        if (!replayed) {
            return std::unexpected(replayed.error());
        }
        if (replayed->first.root != state.root) {
            return std::unexpected(
                Error{"unrecoverable", "transparency entry tiles do not match the stored root"});
        }
        std::size_t published = 0;
        for (std::uint32_t level = 1; level < parent_levels.size(); ++level) {
            const std::vector<Hash>& hashes = parent_levels[level];
            for (std::size_t first = 0; first < hashes.size(); first += transparency_tile_width) {
                const std::size_t width = std::min(transparency_tile_width, hashes.size() - first);
                std::vector<Hash> tile_hashes(hashes.begin() + static_cast<std::ptrdiff_t>(first),
                                              hashes.begin() +
                                                  static_cast<std::ptrdiff_t>(first + width));
                const auto written =
                    publish_hash_tile(level, first / transparency_tile_width, tile_hashes);
                if (!written) {
                    return std::unexpected(written.error());
                }
                ++published;
            }
        }
        return published;
    }

    Result<std::size_t> publish_parent_hashes(std::uint64_t tree_size,
                                              const ParentLevels& parent_levels) const {
        std::size_t published = 0;
        for (std::uint32_t level = 1; level < parent_levels.size(); ++level) {
            const std::vector<Hash>& added_hashes = parent_levels[level];
            if (added_hashes.empty()) {
                continue;
            }
            const std::uint64_t existing_count = tree_size >> level;
            std::uint64_t tile_index = existing_count / transparency_tile_width;
            const std::size_t partial_width = existing_count % transparency_tile_width;
            std::vector<Hash> tile_hashes;
            if (partial_width != 0) {
                const auto partial = load_hash_tile(level, tile_index, partial_width);
                if (!partial) {
                    return std::unexpected(partial.error());
                }
                tile_hashes = *partial;
            }
            for (const Hash& hash : added_hashes) {
                tile_hashes.push_back(hash);
                if (tile_hashes.size() == transparency_tile_width) {
                    const auto written = publish_hash_tile(level, tile_index, tile_hashes);
                    if (!written) {
                        return std::unexpected(written.error());
                    }
                    ++published;
                    ++tile_index;
                    tile_hashes.clear();
                }
            }
            if (!tile_hashes.empty()) {
                const auto written = publish_hash_tile(level, tile_index, tile_hashes);
                if (!written) {
                    return std::unexpected(written.error());
                }
                ++published;
            }
        }
        return published;
    }

    Result<Hash>
    hash_at(std::uint32_t level, std::uint64_t node_index, std::uint64_t tree_size) const {
        const std::uint64_t value_count = tree_size >> level;
        if (node_index >= value_count) {
            return std::unexpected(
                Error{"invalid_input", "requested Merkle node is outside the tree"});
        }
        const std::uint64_t tile_index = node_index / transparency_tile_width;
        const std::size_t width = tile_width_at(value_count, tile_index);
        const std::size_t offset = node_index % transparency_tile_width;
        if (level == 0) {
            const auto entries = load_tile(tile_index, width);
            if (!entries) {
                return std::unexpected(entries.error());
            }
            return leaf_hash(entries->at(offset));
        }
        const auto hashes = load_or_materialize_hash_tile(level, tile_index, width);
        if (!hashes) {
            return std::unexpected(hashes.error());
        }
        return hashes->at(offset);
    }

    Result<Hash>
    range_hash(std::uint64_t first, std::uint64_t count, std::uint64_t tree_size) const {
        if (count == 0 || first > tree_size || count > tree_size - first) {
            return std::unexpected(Error{"invalid_input", "Merkle range is outside the tree"});
        }
        if (std::has_single_bit(count) && first % count == 0) {
            const std::uint32_t level = static_cast<std::uint32_t>(std::countr_zero(count));
            return hash_at(level, first / count, tree_size);
        }
        const std::uint64_t split = std::uint64_t{1} << (std::bit_width(count - 1) - 1);
        const auto left = range_hash(first, split, tree_size);
        if (!left) {
            return std::unexpected(left.error());
        }
        const auto right = range_hash(first + split, count - split, tree_size);
        if (!right) {
            return std::unexpected(right.error());
        }
        return node_hash(*left, *right);
    }

    Result<void> append_inclusion_path(std::uint64_t leaf_index,
                                       std::uint64_t first,
                                       std::uint64_t count,
                                       std::uint64_t tree_size,
                                       std::vector<std::string>& path) const {
        if (count == 1) {
            return {};
        }
        const std::uint64_t split = std::uint64_t{1} << (std::bit_width(count - 1) - 1);
        if (leaf_index < first + split) {
            const auto child = append_inclusion_path(leaf_index, first, split, tree_size, path);
            if (!child) {
                return std::unexpected(child.error());
            }
            const auto sibling = range_hash(first + split, count - split, tree_size);
            if (!sibling) {
                return std::unexpected(sibling.error());
            }
            path.push_back(hex(*sibling));
            return {};
        }
        const auto child =
            append_inclusion_path(leaf_index, first + split, count - split, tree_size, path);
        if (!child) {
            return std::unexpected(child.error());
        }
        const auto sibling = range_hash(first, split, tree_size);
        if (!sibling) {
            return std::unexpected(sibling.error());
        }
        path.push_back(hex(*sibling));
        return {};
    }

    Result<void> append_consistency_path(std::uint64_t old_size,
                                         std::uint64_t new_size,
                                         std::uint64_t first,
                                         bool complete,
                                         std::uint64_t tree_size,
                                         std::vector<std::string>& path) const {
        if (old_size == new_size) {
            if (!complete) {
                const auto root = range_hash(first, new_size, tree_size);
                if (!root) {
                    return std::unexpected(root.error());
                }
                path.push_back(hex(*root));
            }
            return {};
        }
        const std::uint64_t split = std::uint64_t{1} << (std::bit_width(new_size - 1) - 1);
        if (old_size <= split) {
            const auto child =
                append_consistency_path(old_size, split, first, complete, tree_size, path);
            if (!child) {
                return std::unexpected(child.error());
            }
            const auto sibling = range_hash(first + split, new_size - split, tree_size);
            if (!sibling) {
                return std::unexpected(sibling.error());
            }
            path.push_back(hex(*sibling));
            return {};
        }
        const auto child = append_consistency_path(
            old_size - split, new_size - split, first + split, false, tree_size, path);
        if (!child) {
            return std::unexpected(child.error());
        }
        const auto sibling = range_hash(first, split, tree_size);
        if (!sibling) {
            return std::unexpected(sibling.error());
        }
        path.push_back(hex(*sibling));
        return {};
    }

    Result<AppendResult> append_entries(const std::vector<std::string>& attestations,
                                        const std::optional<LogState>& expected_base,
                                        const std::optional<LogState>& expected_head) const {
        if (attestations.empty() && !expected_head) {
            return std::unexpected(Error{"invalid_input", "at least one attestation is required"});
        }
        if (attestations.size() > 100000) {
            return std::unexpected(
                Error{"invalid_input", "one append may contain at most 100000 attestations"});
        }
        const auto decoded = validated_entries(attestations);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        std::error_code error;
        std::filesystem::create_directories(directory / "tiles" / "entries", error);
        if (error) {
            return std::unexpected(Error{
                "store_error", "could not create transparency tile directory: " + error.message()});
        }
        const auto append_lock = AppendLock::acquire(directory / "append.lock");
        if (!append_lock) {
            return std::unexpected(append_lock.error());
        }
        auto current = stored_state(state_path());
        if (!current) {
            return std::unexpected(current.error());
        }
        if (expected_head && current->tree_size == expected_head->tree_size &&
            hex(current->root) == expected_head->root_hash) {
            return AppendResult{current->tree_size, current->tree_size, hex(current->root), 0, 0};
        }
        if (expected_base && (current->tree_size != expected_base->tree_size ||
                              hex(current->root) != expected_base->root_hash)) {
            return std::unexpected(
                Error{"unrecoverable", "replication base does not match the local log prefix"});
        }
        const auto backfilled_hash_tiles = ensure_hash_tiles(*current);
        if (!backfilled_hash_tiles) {
            return std::unexpected(backfilled_hash_tiles.error());
        }
        StoredState candidate = *current;
        ParentLevels parent_levels;
        for (const Bytes& entry : *decoded) {
            const auto digest = leaf_hash(entry);
            if (!digest) {
                return std::unexpected(digest.error());
            }
            const auto added = add_leaf(candidate.frontier, *digest, &parent_levels);
            if (!added) {
                return std::unexpected(added.error());
            }
            ++candidate.tree_size;
        }
        const auto candidate_root = frontier_root(candidate.frontier);
        if (!candidate_root) {
            return std::unexpected(candidate_root.error());
        }
        candidate.root = *candidate_root;
        if (expected_head && (candidate.tree_size != expected_head->tree_size ||
                              hex(candidate.root) != expected_head->root_hash)) {
            return std::unexpected(
                Error{"unrecoverable", "replication entries do not produce the declared log head"});
        }
        const std::uint64_t first_index = current->tree_size;
        if (decoded->empty()) {
            return AppendResult{
                first_index, current->tree_size, hex(current->root), 0, *backfilled_hash_tiles};
        }
        std::uint64_t tile_index = current->tree_size / transparency_tile_width;
        const std::size_t partial_width = current->tree_size % transparency_tile_width;
        std::vector<Bytes> tile_entries;
        if (partial_width != 0) {
            const auto partial = load_tile(tile_index, partial_width);
            if (!partial) {
                return std::unexpected(partial.error());
            }
            tile_entries = *partial;
        }
        std::size_t tiles_published = 0;
        for (const Bytes& entry : *decoded) {
            tile_entries.push_back(entry);
            if (tile_entries.size() == transparency_tile_width) {
                const auto written = publish_tile(tile_index, tile_entries);
                if (!written) {
                    return std::unexpected(written.error());
                }
                ++tiles_published;
                ++tile_index;
                tile_entries.clear();
            }
        }
        if (!tile_entries.empty()) {
            const auto written = publish_tile(tile_index, tile_entries);
            if (!written) {
                return std::unexpected(written.error());
            }
            ++tiles_published;
        }
        const auto hash_tiles_published = publish_parent_hashes(current->tree_size, parent_levels);
        if (!hash_tiles_published) {
            return std::unexpected(hash_tiles_published.error());
        }
        const auto state_written = write_json(state_path(), state_json(candidate));
        if (!state_written) {
            return std::unexpected(state_written.error());
        }
        return AppendResult{first_index,
                            candidate.tree_size,
                            hex(candidate.root),
                            tiles_published,
                            *backfilled_hash_tiles + *hash_tiles_published};
    }
};

TiledLog::TiledLog(std::filesystem::path store_root, std::string instance_name)
    : impl_(std::make_shared<Impl>(std::move(store_root), std::move(instance_name))) {}

Result<AppendResult> TiledLog::append(const std::vector<std::string>& attestations) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    return impl_->append_entries(attestations, std::nullopt, std::nullopt);
}

Result<AppendResult> TiledLog::append_audit_event(const audit::Event& event) {
    if (event.draft.outcome != "success") {
        return std::unexpected(
            Error{"invalid_input", "only successful audit events may publish a commitment"});
    }
    json recorded = audit::to_json(event);
    recorded.erase("digest");
    const auto recorded_bytes = json::to_cbor(recorded);
    const auto digest = sha512_256(std::as_bytes(std::span(recorded_bytes)));
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (hex(*digest) != event.digest) {
        return std::unexpected(Error{"invalid_input", "audit event digest does not verify"});
    }
    const auto body = json::to_cbor(json{{"digest", event.digest}});
    Bytes entry;
    entry.reserve(audit_commitment_header.size() + body.size());
    for (const char value : audit_commitment_header) {
        entry.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    entry.insert(entry.end(),
                 reinterpret_cast<const std::byte*>(body.data()),
                 reinterpret_cast<const std::byte*>(body.data() + body.size()));
    const auto encoded = base64url_encode(entry);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return append({*encoded});
}

Result<Tile> TiledLog::tile(std::uint64_t index, std::size_t width) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    if (width == 0 || width > transparency_tile_width) {
        return std::unexpected(Error{"invalid_input", "tile width must be between 1 and 1024"});
    }
    const auto bytes = read_bytes(impl_->tile_path(index, width));
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const auto entries = decode_tile(*bytes, width);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    const auto digest = sha256(*bytes);
    const auto encoded = base64url_encode(*bytes);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return Tile{index, width, *encoded, hex(*digest)};
}

Result<HashTile>
TiledLog::hash_tile(std::uint32_t level, std::uint64_t index, std::size_t width) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    if (level == 0 || level >= 64) {
        return std::unexpected(Error{"invalid_input", "hash tile level must be between 1 and 63"});
    }
    if (width == 0 || width > transparency_tile_width) {
        return std::unexpected(Error{"invalid_input", "tile width must be between 1 and 1024"});
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create transparency directory: " + error.message()});
    }
    const auto append_lock = AppendLock::acquire(impl_->directory / "append.lock");
    if (!append_lock) {
        return std::unexpected(append_lock.error());
    }
    const auto current = stored_state(impl_->state_path());
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto ensured = impl_->ensure_hash_tiles(*current);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }
    const auto hashes = impl_->load_or_materialize_hash_tile(level, index, width);
    if (!hashes) {
        return std::unexpected(hashes.error());
    }
    const auto encoded_bytes = encode_hash_tile(*hashes);
    if (!encoded_bytes) {
        return std::unexpected(encoded_bytes.error());
    }
    const auto digest = sha256(*encoded_bytes);
    const auto encoded = base64url_encode(*encoded_bytes);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return HashTile{level, index, width, *encoded, hex(*digest)};
}

Result<InclusionProof> TiledLog::inclusion_proof(std::uint64_t leaf_index,
                                                 std::uint64_t tree_size) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    if (tree_size == 0 || leaf_index >= tree_size) {
        return std::unexpected(
            Error{"invalid_input", "leaf index must be within a non-empty tree"});
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create transparency directory: " + error.message()});
    }
    const auto append_lock = AppendLock::acquire(impl_->directory / "append.lock");
    if (!append_lock) {
        return std::unexpected(append_lock.error());
    }
    const auto current = stored_state(impl_->state_path());
    if (!current) {
        return std::unexpected(current.error());
    }
    if (tree_size > current->tree_size) {
        return std::unexpected(
            Error{"invalid_input", "requested tree size is beyond the current log"});
    }
    const auto ensured = impl_->ensure_hash_tiles(*current);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }
    InclusionProof proof{.leaf_index = leaf_index, .tree_size = tree_size};
    const auto appended =
        impl_->append_inclusion_path(leaf_index, 0, tree_size, tree_size, proof.audit_path);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    const auto root = impl_->range_hash(0, tree_size, tree_size);
    if (!root) {
        return std::unexpected(root.error());
    }
    proof.root_hash = hex(*root);
    return proof;
}

Result<ConsistencyProof> TiledLog::consistency_proof(std::uint64_t old_tree_size,
                                                     std::uint64_t new_tree_size) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    if (old_tree_size > new_tree_size) {
        return std::unexpected(
            Error{"invalid_input", "old tree size must not exceed new tree size"});
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create transparency directory: " + error.message()});
    }
    const auto append_lock = AppendLock::acquire(impl_->directory / "append.lock");
    if (!append_lock) {
        return std::unexpected(append_lock.error());
    }
    const auto current = stored_state(impl_->state_path());
    if (!current) {
        return std::unexpected(current.error());
    }
    if (new_tree_size > current->tree_size) {
        return std::unexpected(
            Error{"invalid_input", "requested tree size is beyond the current log"});
    }
    const auto ensured = impl_->ensure_hash_tiles(*current);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }
    ConsistencyProof proof{
        .old_tree_size = old_tree_size,
        .new_tree_size = new_tree_size,
    };
    const auto old_root =
        old_tree_size == 0 ? empty_root() : impl_->range_hash(0, old_tree_size, new_tree_size);
    if (!old_root) {
        return std::unexpected(old_root.error());
    }
    const auto new_root =
        new_tree_size == 0 ? empty_root() : impl_->range_hash(0, new_tree_size, new_tree_size);
    if (!new_root) {
        return std::unexpected(new_root.error());
    }
    proof.old_root_hash = hex(*old_root);
    proof.new_root_hash = hex(*new_root);
    if (old_tree_size != 0 && old_tree_size != new_tree_size) {
        const auto appended = impl_->append_consistency_path(
            old_tree_size, new_tree_size, 0, true, new_tree_size, proof.audit_path);
        if (!appended) {
            return std::unexpected(appended.error());
        }
    }
    return proof;
}

Result<LogState> TiledLog::state() const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    const auto current = stored_state(impl_->state_path());
    if (!current) {
        return std::unexpected(current.error());
    }
    return LogState{current->tree_size, hex(current->root)};
}

Result<LogState> TiledLog::state_at(std::uint64_t tree_size) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create transparency directory: " + error.message()});
    }
    const auto append_lock = AppendLock::acquire(impl_->directory / "append.lock");
    if (!append_lock) {
        return std::unexpected(append_lock.error());
    }
    const auto current = stored_state(impl_->state_path());
    if (!current) {
        return std::unexpected(current.error());
    }
    if (tree_size > current->tree_size) {
        return std::unexpected(
            Error{"invalid_input", "requested tree size is beyond the current log"});
    }
    const auto replayed = impl_->replay(tree_size, current->tree_size);
    if (!replayed) {
        return std::unexpected(replayed.error());
    }
    return LogState{replayed->first.tree_size, hex(replayed->first.root)};
}

Result<LogReplicationBatch> TiledLog::replication_batch(std::uint64_t after_tree_size) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create transparency directory: " + error.message()});
    }
    const auto append_lock = AppendLock::acquire(impl_->directory / "append.lock");
    if (!append_lock) {
        return std::unexpected(append_lock.error());
    }
    const auto current = stored_state(impl_->state_path());
    if (!current) {
        return std::unexpected(current.error());
    }
    if (after_tree_size > current->tree_size) {
        return std::unexpected(
            Error{"invalid_input", "replication cursor is beyond the current log"});
    }
    constexpr std::uint64_t batch_limit = 100000;
    const std::uint64_t available = current->tree_size - after_tree_size;
    const std::uint64_t head_tree_size =
        after_tree_size + std::min<std::uint64_t>(available, batch_limit);
    const auto base = impl_->replay(after_tree_size, current->tree_size);
    if (!base) {
        return std::unexpected(base.error());
    }
    const auto head = impl_->replay(head_tree_size, current->tree_size, after_tree_size);
    if (!head) {
        return std::unexpected(head.error());
    }
    if (head_tree_size == current->tree_size && head->first.root != current->root) {
        return std::unexpected(
            Error{"unrecoverable", "transparency tiles do not match the stored log root"});
    }
    return LogReplicationBatch{
        .base = {base->first.tree_size, hex(base->first.root)},
        .head = {head->first.tree_size, hex(head->first.root)},
        .source_tree_size = current->tree_size,
        .attestations = std::move(head->second),
    };
}

Result<AppendResult> TiledLog::apply_replication_batch(const LogReplicationBatch& batch) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    if (batch.source_tree_size < batch.head.tree_size ||
        batch.head.tree_size < batch.base.tree_size ||
        batch.head.tree_size - batch.base.tree_size != batch.attestations.size()) {
        return std::unexpected(Error{"invalid_input", "replication batch bounds are invalid"});
    }
    return impl_->append_entries(batch.attestations, batch.base, batch.head);
}

Result<IdentityScan> TiledLog::scan_identity(const std::string& identity,
                                             std::uint64_t tree_size,
                                             const std::string& root_hash,
                                             std::uint64_t start_index,
                                             std::size_t limit) const {
    const auto started = std::chrono::steady_clock::now();
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    if (!identity.starts_with("taap:1:")) {
        return std::unexpected(Error{"invalid_input", "identity must be a TAAP identifier"});
    }
    const auto expected_root = decode_hash(root_hash);
    if (!expected_root) {
        return std::unexpected(Error{"invalid_input", "checkpoint root hash is invalid"});
    }
    // Tiles are immutable and `tree_size` is a published checkpoint's, so the scan reads them
    // without the append lock. Concurrent scans would only repeat each other's work.
    std::unique_lock index_lock(impl_->index_mutex, std::try_to_lock);
    if (!index_lock.owns_lock()) {
        return std::unexpected(
            Error{"busy", "another identity scan is verifying the transparency log; retry"});
    }
    IdentityIndex& index = impl_->identity_index;
    if (tree_size < index.tree_size) {
        // A checkpoint older than what was verified: verify again from the first entry.
        index = IdentityIndex{};
    }
    IdentityScan scan{.identity = identity, .tree_size = tree_size, .root_hash = root_hash};
    const auto indexed_matches = [&]() -> std::span<const std::uint64_t> {
        const auto found = index.entries.find(identity);
        if (found == index.entries.end()) {
            return {};
        }
        const auto first = std::ranges::lower_bound(found->second, start_index);
        return {first, found->second.end()};
    };
    const std::uint64_t verified_size = index.tree_size;
    const std::size_t earlier = std::min(indexed_matches().size(), limit);
    const std::size_t keep = limit - earlier;

    // Verify the entries appended since the last scan. Nothing is kept unless they reach the
    // checkpoint's root; entries naming the identity are kept for this page as they are read.
    std::vector<IdentityMatch> fresh;
    std::vector<std::optional<Hash>> frontier = index.frontier;
    std::vector<Hash> tail = index.tail_leaves;
    std::vector<Hash> tile_roots;
    std::unordered_map<std::string, std::vector<std::uint64_t>> named;
    std::uint64_t entry_index = verified_size;
    while (entry_index < tree_size) {
        const std::uint64_t tile_index = entry_index / transparency_tile_width;
        const std::uint64_t tile_start = tile_index * transparency_tile_width;
        const std::size_t width = static_cast<std::size_t>(
            std::min<std::uint64_t>(tree_size - tile_start, transparency_tile_width));
        const auto entries = impl_->load_tile(tile_index, width);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        ++scan.tiles_scanned;
        const std::size_t offset = static_cast<std::size_t>(entry_index - tile_start);
        // Repeated entries in one tile verify once; the cache never outgrows a tile.
        std::unordered_map<std::string, VerifiedAttestation> verification_cache;
        for (std::size_t position = 0; position < width; ++position) {
            const Bytes& entry = (*entries)[position];
            const auto leaf = leaf_hash(entry);
            if (!leaf) {
                return std::unexpected(leaf.error());
            }
            if (position < offset) {
                if (position >= tail.size() || tail[position] != *leaf) {
                    return std::unexpected(
                        Error{"unrecoverable",
                              "transparency tile does not match the entries already verified"});
                }
                continue;
            }
            const auto added = add_leaf(frontier, *leaf);
            if (!added) {
                return std::unexpected(added.error());
            }
            tail.push_back(*leaf);
            const auto digest = sha256(entry);
            if (!digest) {
                return std::unexpected(digest.error());
            }
            const std::string key = hex(*digest);
            auto verified = verification_cache.find(key);
            if (verified == verification_cache.end()) {
                const auto value = verify_attestation(entry, true);
                if (!value) {
                    return std::unexpected(Error{"unrecoverable",
                                                 "transparency tile contains an "
                                                 "invalid TAAP attestation: " +
                                                     value.error().message});
                }
                verified = verification_cache.emplace(key, *value).first;
            }
            for (const std::string& named_identity : verified->second.identities) {
                named[named_identity].push_back(entry_index);
            }
            if (fresh.size() < keep && entry_index >= start_index &&
                std::ranges::find(verified->second.identities, identity) !=
                    verified->second.identities.end()) {
                const auto encoded = base64url_encode(entry);
                if (!encoded) {
                    return std::unexpected(encoded.error());
                }
                fresh.push_back({entry_index,
                                 verified->second.profile,
                                 verified->second.issuer,
                                 verified->second.subject,
                                 *encoded});
            }
            ++entry_index;
            ++scan.entries_scanned;
        }
        if (width == transparency_tile_width) {
            const auto root = tile_root(tail);
            if (!root) {
                return std::unexpected(root.error());
            }
            tile_roots.push_back(*root);
            tail.clear();
        }
    }
    const auto actual_root = frontier_root(frontier);
    if (!actual_root) {
        return std::unexpected(actual_root.error());
    }
    if (*actual_root != *expected_root) {
        return std::unexpected(
            Error{"unrecoverable", "transparency tile Merkle root does not match the checkpoint"});
    }
    index.tree_size = tree_size;
    index.frontier = std::move(frontier);
    index.tail_leaves = std::move(tail);
    index.tile_roots.insert(index.tile_roots.end(), tile_roots.begin(), tile_roots.end());
    for (auto& [named_identity, indexes] : named) {
        std::vector<std::uint64_t>& kept = index.entries[named_identity];
        kept.insert(kept.end(), indexes.begin(), indexes.end());
    }

    // Read this page's entries that an earlier scan verified again, checking each tile against
    // the root or leaf hashes kept for it.
    const std::span<const std::uint64_t> matches = indexed_matches();
    std::optional<std::uint64_t> loaded_tile;
    std::vector<Bytes> loaded;
    for (const std::uint64_t match : matches.first(earlier)) {
        const std::uint64_t tile_index = match / transparency_tile_width;
        const std::uint64_t tile_start = tile_index * transparency_tile_width;
        if (loaded_tile != tile_index) {
            const std::size_t width = static_cast<std::size_t>(
                std::min<std::uint64_t>(tree_size - tile_start, transparency_tile_width));
            auto entries = impl_->load_tile(tile_index, width);
            if (!entries) {
                return std::unexpected(entries.error());
            }
            ++scan.tiles_scanned;
            std::vector<Hash> leaves;
            leaves.reserve(entries->size());
            for (const Bytes& entry : *entries) {
                const auto leaf = leaf_hash(entry);
                if (!leaf) {
                    return std::unexpected(leaf.error());
                }
                leaves.push_back(*leaf);
            }
            bool intact = false;
            if (width == transparency_tile_width) {
                const auto root = tile_root(leaves);
                if (!root) {
                    return std::unexpected(root.error());
                }
                intact =
                    tile_index < index.tile_roots.size() && index.tile_roots[tile_index] == *root;
            } else {
                intact = leaves == index.tail_leaves;
            }
            if (!intact) {
                return std::unexpected(
                    Error{"unrecoverable",
                          "transparency tile does not match the entries already verified"});
            }
            loaded = std::move(*entries);
            loaded_tile = tile_index;
        }
        const Bytes& entry = loaded[static_cast<std::size_t>(match - tile_start)];
        const auto verified = verify_attestation(entry, true);
        if (!verified) {
            return std::unexpected(Error{"unrecoverable",
                                         "transparency tile contains an invalid TAAP "
                                         "attestation: " +
                                             verified.error().message});
        }
        const auto encoded = base64url_encode(entry);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        scan.matches.push_back(
            {match, verified->profile, verified->issuer, verified->subject, *encoded});
    }
    scan.matches.insert(scan.matches.end(),
                        std::make_move_iterator(fresh.begin()),
                        std::make_move_iterator(fresh.end()));
    if (matches.size() > scan.matches.size()) {
        scan.next_index = matches[scan.matches.size()];
    }
    scan.duration_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    return scan;
}

const std::filesystem::path& TiledLog::directory() const {
    return impl_->directory;
}

} // namespace lake::transparency
