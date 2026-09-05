#include "lake/transparency/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
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

namespace lake::transparency {
namespace {

using Bytes = std::vector<std::byte>;
using Hash = std::array<std::byte, 32>;
using nlohmann::json;

constexpr std::string_view tile_header = "LAKE-TILE-1\n";

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

Result<void> add_leaf(std::vector<std::optional<Hash>>& frontier, const Hash& leaf) {
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

Result<VerifiedAttestation> verify_attestation(std::span<const std::byte> encoded) {
    const auto statement = liquid::decode_attestation(encoded);
    if (!statement) {
        return std::unexpected(
            Error{"invalid_input",
                  "entry is not a canonical TAAP attestation: " + statement.error().message});
    }
    liquid::attestation_verification_options options{.now = statement->at};
    const liquid::attestation_profile* profile =
        liquid::attestation_profile_registry::global().find(statement->profile);
    if (profile && profile->status_policy.kind == liquid::attestation_status_kind::revocable) {
        options.status = liquid::attestation_status{.revoked = false, .checked_at = statement->at};
    }
    const auto verified = liquid::verify_attestation(*statement, options);
    if (!verified) {
        return std::unexpected(Error{
            "invalid_input", "could not verify TAAP attestation: " + verified.error().message});
    }
    if (!verified->verified) {
        return std::unexpected(
            Error{"invalid_input", "TAAP attestation is invalid: " + verified->reason});
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
            const auto verified = verify_attestation(*bytes);
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
};

TiledLog::TiledLog(std::filesystem::path store_root, std::string instance_name)
    : impl_(std::make_shared<Impl>(std::move(store_root), std::move(instance_name))) {}

Result<AppendResult> TiledLog::append(const std::vector<std::string>& attestations) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "lake instance name is not a safe path component"});
    }
    if (attestations.empty()) {
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
    std::filesystem::create_directories(impl_->directory / "tiles" / "entries", error);
    if (error) {
        return std::unexpected(Error{
            "store_error", "could not create transparency tile directory: " + error.message()});
    }
    const auto append_lock = AppendLock::acquire(impl_->directory / "append.lock");
    if (!append_lock) {
        return std::unexpected(append_lock.error());
    }
    auto current = stored_state(impl_->state_path());
    if (!current) {
        return std::unexpected(current.error());
    }
    const std::uint64_t first_index = current->tree_size;
    std::uint64_t tile_index = current->tree_size / transparency_tile_width;
    const std::size_t partial_width = current->tree_size % transparency_tile_width;
    std::vector<Bytes> tile_entries;
    if (partial_width != 0) {
        const auto partial = impl_->load_tile(tile_index, partial_width);
        if (!partial) {
            return std::unexpected(partial.error());
        }
        tile_entries = *partial;
    }
    std::size_t tiles_published = 0;
    for (const Bytes& entry : *decoded) {
        const auto digest = leaf_hash(entry);
        if (!digest) {
            return std::unexpected(digest.error());
        }
        const auto added = add_leaf(current->frontier, *digest);
        if (!added) {
            return std::unexpected(added.error());
        }
        ++current->tree_size;
        tile_entries.push_back(entry);
        if (tile_entries.size() == transparency_tile_width) {
            const auto written = impl_->publish_tile(tile_index, tile_entries);
            if (!written) {
                return std::unexpected(written.error());
            }
            ++tiles_published;
            ++tile_index;
            tile_entries.clear();
        }
    }
    if (!tile_entries.empty()) {
        const auto written = impl_->publish_tile(tile_index, tile_entries);
        if (!written) {
            return std::unexpected(written.error());
        }
        ++tiles_published;
    }
    const auto root = frontier_root(current->frontier);
    if (!root) {
        return std::unexpected(root.error());
    }
    current->root = *root;
    const auto state_written = write_json(impl_->state_path(), state_json(*current));
    if (!state_written) {
        return std::unexpected(state_written.error());
    }
    return AppendResult{first_index, current->tree_size, hex(current->root), tiles_published};
}

Result<Tile> TiledLog::tile(std::uint64_t index, std::size_t width) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "lake instance name is not a safe path component"});
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

Result<LogState> TiledLog::state() const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "lake instance name is not a safe path component"});
    }
    const auto current = stored_state(impl_->state_path());
    if (!current) {
        return std::unexpected(current.error());
    }
    return LogState{current->tree_size, hex(current->root)};
}

Result<IdentityScan> TiledLog::scan_identity(const std::string& identity,
                                             std::uint64_t tree_size,
                                             const std::string& root_hash) const {
    std::unique_lock lock(impl_->mutex);
    const auto started = std::chrono::steady_clock::now();
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "lake instance name is not a safe path component"});
    }
    if (!identity.starts_with("taap:1:")) {
        return std::unexpected(Error{"invalid_input", "identity must be a TAAP identifier"});
    }
    const auto expected_root = decode_hash(root_hash);
    if (!expected_root) {
        return std::unexpected(Error{"invalid_input", "checkpoint root hash is invalid"});
    }
    IdentityScan scan{.identity = identity, .tree_size = tree_size, .root_hash = root_hash};
    std::vector<std::optional<Hash>> frontier;
    std::unordered_map<std::string, VerifiedAttestation> verification_cache;
    std::uint64_t entry_index = 0;
    const std::uint64_t tile_count =
        tree_size == 0 ? 0 : (tree_size + transparency_tile_width - 1) / transparency_tile_width;
    for (std::uint64_t tile_index = 0; tile_index < tile_count; ++tile_index) {
        const std::uint64_t remaining = tree_size - entry_index;
        const std::size_t width =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, transparency_tile_width));
        const auto entries = impl_->load_tile(tile_index, width);
        if (!entries) {
            return std::unexpected(entries.error());
        }
        ++scan.tiles_scanned;
        for (const Bytes& entry : *entries) {
            const auto leaf = leaf_hash(entry);
            if (!leaf) {
                return std::unexpected(leaf.error());
            }
            const auto added = add_leaf(frontier, *leaf);
            if (!added) {
                return std::unexpected(added.error());
            }
            const auto digest = sha256(entry);
            if (!digest) {
                return std::unexpected(digest.error());
            }
            const std::string key = hex(*digest);
            auto verified = verification_cache.find(key);
            if (verified == verification_cache.end()) {
                const auto value = verify_attestation(entry);
                if (!value) {
                    return std::unexpected(Error{"unrecoverable",
                                                 "transparency tile contains an "
                                                 "invalid TAAP attestation: " +
                                                     value.error().message});
                }
                verified = verification_cache.emplace(key, *value).first;
            }
            if (std::ranges::find(verified->second.identities, identity) !=
                verified->second.identities.end()) {
                const auto encoded = base64url_encode(entry);
                if (!encoded) {
                    return std::unexpected(encoded.error());
                }
                scan.matches.push_back({entry_index,
                                        verified->second.profile,
                                        verified->second.issuer,
                                        verified->second.subject,
                                        *encoded});
            }
            ++entry_index;
        }
    }
    scan.entries_scanned = entry_index;
    const auto actual_root = frontier_root(frontier);
    if (!actual_root) {
        return std::unexpected(actual_root.error());
    }
    if (*actual_root != *expected_root) {
        return std::unexpected(
            Error{"unrecoverable", "transparency tile Merkle root does not match the checkpoint"});
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
