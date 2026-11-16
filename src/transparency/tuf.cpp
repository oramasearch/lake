// SPDX-License-Identifier: MIT

#include "lake/transparency/tuf.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <vector>

#include <openssl/evp.h>

namespace lake::transparency {
namespace {

using nlohmann::json;

constexpr std::array<std::string_view, 4> roles = {"root", "targets", "snapshot", "timestamp"};

struct PkeyDeleter {
    void operator()(EVP_PKEY* key) const {
        EVP_PKEY_free(key);
    }
};

struct ContextDeleter {
    void operator()(EVP_MD_CTX* context) const {
        EVP_MD_CTX_free(context);
    }
};

using Pkey = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using Context = std::unique_ptr<EVP_MD_CTX, ContextDeleter>;

bool valid_instance_name(std::string_view value) {
    return !value.empty() && value != "." && value != ".." &&
           value.find('/') == std::string_view::npos && value.find('\\') == std::string_view::npos;
}

bool valid_role(std::string_view role) {
    return std::ranges::find(roles, role) != roles.end();
}

Result<std::vector<unsigned char>> decode_hex(std::string_view value) {
    if (value.empty() || value.size() % 2 != 0) {
        return std::unexpected(Error{"invalid_input", "hex value has an invalid length"});
    }
    std::vector<unsigned char> output(value.size() / 2);
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
            return std::unexpected(Error{"invalid_input", "hex value contains a non-hex digit"});
        }
        output[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return output;
}

Result<std::string> sha256(std::string_view value) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    if (EVP_Digest(value.data(), value.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1) {
        return std::unexpected(Error{"store_error", "could not calculate SHA-256"});
    }
    std::string output;
    output.reserve(size * 2);
    for (unsigned int index = 0; index < size; ++index) {
        output += std::format("{:02x}", digest[index]);
    }
    return output;
}

Result<std::uint64_t> version_of(const json& document, std::string_view role) {
    if (!document.is_object() || !document.contains("signed") ||
        !document.at("signed").is_object() || !document.contains("signatures") ||
        !document.at("signatures").is_array()) {
        return std::unexpected(
            Error{"invalid_input", std::string(role) + " metadata has an invalid envelope"});
    }
    const json& body = document.at("signed");
    if (body.value("_type", "") != role) {
        return std::unexpected(
            Error{"invalid_input", std::string(role) + " metadata has the wrong _type"});
    }
    const std::string specification = body.value("spec_version", "");
    if (specification != "1.0" && !specification.starts_with("1.0.")) {
        return std::unexpected(
            Error{"invalid_input", std::string(role) + " metadata must use TUF 1.0"});
    }
    if (!body.contains("version") || !body.at("version").is_number_unsigned() ||
        body.at("version").get<std::uint64_t>() == 0) {
        return std::unexpected(
            Error{"invalid_input", std::string(role) + " metadata version must be positive"});
    }
    return body.at("version").get<std::uint64_t>();
}

bool valid_utc_timestamp(std::string_view value) {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7 || index == 10 || index == 13 || index == 16 || index == 19) {
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    return true;
}

Result<void> verify_not_expired(const json& document, std::string_view role) {
    const std::string expires = document.at("signed").value("expires", "");
    if (!valid_utc_timestamp(expires)) {
        return std::unexpected(
            Error{"invalid_input", std::string(role) + " metadata expiry must be UTC RFC 3339"});
    }
    const std::string now = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    if (expires <= now) {
        return std::unexpected(Error{"invalid_input", std::string(role) + " metadata is expired"});
    }
    return {};
}

Result<void> verify_key(const std::string& key_id, const json& key) {
    if (!key.is_object() || key.value("keytype", "") != "ed25519" ||
        key.value("scheme", "") != "ed25519" || !key.contains("keyval") ||
        !key.at("keyval").is_object() || !key.at("keyval").contains("public") ||
        !key.at("keyval").at("public").is_string()) {
        return std::unexpected(Error{"invalid_input", "TUF keys must be Ed25519 public keys"});
    }
    // A key object carries only the TUF members, so one public key cannot be given a second id by
    // adding a member (lake#1005).
    for (const auto& [member, value] : key.items()) {
        if (member != "keytype" && member != "scheme" && member != "keyval" &&
            member != "keyid_hash_algorithms") {
            return std::unexpected(Error{"invalid_input", "TUF key has an unknown member: " + member});
        }
    }
    if (key.at("keyval").size() != 1) {
        return std::unexpected(Error{"invalid_input", "TUF keyval holds only the public key"});
    }
    const auto expected_id = sha256(key.dump());
    if (!expected_id) {
        return std::unexpected(expected_id.error());
    }
    if (*expected_id != key_id) {
        return std::unexpected(Error{"invalid_input", "TUF key id does not match its key object"});
    }
    const auto public_key = decode_hex(key.at("keyval").at("public").get<std::string>());
    if (!public_key || public_key->size() != 32) {
        return std::unexpected(Error{"invalid_input", "TUF Ed25519 public key must be 32 bytes"});
    }
    return {};
}

bool signature_valid(const json& key, std::string_view body, std::string_view encoded_signature) {
    const auto public_key = decode_hex(key.at("keyval").at("public").get<std::string>());
    const auto signature = decode_hex(encoded_signature);
    if (!public_key || public_key->size() != 32 || !signature || signature->size() != 64) {
        return false;
    }
    Pkey signing_key(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key->data(), public_key->size()));
    if (!signing_key) {
        return false;
    }
    Context context(EVP_MD_CTX_new());
    if (!context ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, signing_key.get()) != 1) {
        return false;
    }
    return EVP_DigestVerify(context.get(),
                            signature->data(),
                            signature->size(),
                            reinterpret_cast<const unsigned char*>(body.data()),
                            body.size()) == 1;
}

Result<void>
verify_threshold(const json& document, const json& trusted_root, std::string_view role) {
    const json& root = trusted_root.at("signed");
    if (!root.contains("keys") || !root.at("keys").is_object() || !root.contains("roles") ||
        !root.at("roles").is_object() || !root.at("roles").contains(role) ||
        !root.at("roles").at(role).is_object()) {
        return std::unexpected(Error{"invalid_input", "trusted root is missing role metadata"});
    }
    const json& role_description = root.at("roles").at(role);
    if (!role_description.contains("keyids") || !role_description.at("keyids").is_array() ||
        !role_description.contains("threshold") ||
        !role_description.at("threshold").is_number_unsigned()) {
        return std::unexpected(Error{"invalid_input", "trusted root role is malformed"});
    }
    const std::uint64_t threshold = role_description.at("threshold").get<std::uint64_t>();
    const std::vector<std::string> allowed =
        role_description.at("keyids").get<std::vector<std::string>>();
    if (threshold == 0 || threshold > allowed.size()) {
        return std::unexpected(Error{"invalid_input", "trusted root role threshold is invalid"});
    }
    // Counted by distinct public key, not by key id: one key listed under two ids signs once
    // (lake#1005).
    std::set<std::string> counted;
    std::set<std::vector<unsigned char>> counted_keys;
    const std::string body = document.at("signed").dump();
    for (const json& signature : document.at("signatures")) {
        if (!signature.is_object() || !signature.contains("keyid") ||
            !signature.at("keyid").is_string() || !signature.contains("sig") ||
            !signature.at("sig").is_string()) {
            continue;
        }
        const std::string key_id = signature.at("keyid").get<std::string>();
        if (counted.contains(key_id) || std::ranges::find(allowed, key_id) == allowed.end() ||
            !root.at("keys").contains(key_id)) {
            continue;
        }
        const json& key = root.at("keys").at(key_id);
        if (!verify_key(key_id, key)) {
            continue;
        }
        if (signature_valid(key, body, signature.at("sig").get<std::string>())) {
            counted.insert(key_id);
            if (const auto public_key = decode_hex(key.at("keyval").at("public").get<std::string>())) {
                counted_keys.insert(*public_key);
            }
        }
    }
    if (counted_keys.size() < threshold) {
        return std::unexpected(
            Error{"invalid_input",
                  std::string(role) + " metadata does not meet its signature threshold"});
    }
    return {};
}

Result<void> verify_root_shape(const json& document) {
    const json& root = document.at("signed");
    if (!root.contains("consistent_snapshot") || !root.at("consistent_snapshot").is_boolean() ||
        !root.contains("keys") || !root.at("keys").is_object() || !root.contains("roles") ||
        !root.at("roles").is_object()) {
        return std::unexpected(Error{"invalid_input", "root metadata is missing required fields"});
    }
    for (const std::string_view role : roles) {
        if (!root.at("roles").contains(role) || !root.at("roles").at(role).is_object()) {
            return std::unexpected(
                Error{"invalid_input", "root metadata does not define every top-level role"});
        }
        const json& role_description = root.at("roles").at(role);
        if (!role_description.contains("keyids") || !role_description.at("keyids").is_array() ||
            !role_description.contains("threshold") ||
            !role_description.at("threshold").is_number_unsigned()) {
            return std::unexpected(Error{"invalid_input", "root role definition is malformed"});
        }
        std::set<std::string> distinct_key_ids;
        for (const json& key_id_value : role_description.at("keyids")) {
            if (!key_id_value.is_string()) {
                return std::unexpected(Error{"invalid_input", "root role key id must be a string"});
            }
            const std::string key_id = key_id_value.get<std::string>();
            if (!distinct_key_ids.insert(key_id).second) {
                return std::unexpected(
                    Error{"invalid_input", "root role key ids must be distinct"});
            }
            if (!root.at("keys").contains(key_id)) {
                return std::unexpected(
                    Error{"invalid_input", "root role references an unknown key"});
            }
            const auto verified = verify_key(key_id, root.at("keys").at(key_id));
            if (!verified) {
                return verified;
            }
        }
        const std::uint64_t threshold = role_description.at("threshold").get<std::uint64_t>();
        if (threshold == 0 || threshold > distinct_key_ids.size()) {
            return std::unexpected(Error{"invalid_input", "root role threshold is invalid"});
        }
    }
    return {};
}

Result<void> verify_metadata_shape(const json& document, std::string_view role) {
    const json& body = document.at("signed");
    if (role == "targets" && (!body.contains("targets") || !body.at("targets").is_object())) {
        return std::unexpected(Error{"invalid_input", "targets metadata is missing targets"});
    }
    if ((role == "snapshot" || role == "timestamp") &&
        (!body.contains("meta") || !body.at("meta").is_object())) {
        return std::unexpected(
            Error{"invalid_input", std::string(role) + " metadata is missing meta"});
    }
    return {};
}

Result<void>
verify_meta_binding(const json& parent, std::string_view child_name, const json& child) {
    const json& meta = parent.at("signed").at("meta");
    if (!meta.contains(child_name) || !meta.at(child_name).is_object()) {
        return std::unexpected(
            Error{"invalid_input", std::string(child_name) + " is absent from parent metadata"});
    }
    const json& binding = meta.at(child_name);
    const std::uint64_t child_version = child.at("signed").at("version").get<std::uint64_t>();
    const std::string encoded = child.dump();
    const auto digest = sha256(encoded);
    if (!digest) {
        return std::unexpected(digest.error());
    }
    if (!binding.contains("version") || !binding.at("version").is_number_unsigned() ||
        binding.at("version").get<std::uint64_t>() != child_version ||
        !binding.contains("length") || !binding.at("length").is_number_unsigned() ||
        binding.at("length").get<std::uint64_t>() != encoded.size() ||
        !binding.contains("hashes") || !binding.at("hashes").is_object() ||
        binding.at("hashes").value("sha256", "") != *digest) {
        return std::unexpected(Error{
            "invalid_input", std::string(child_name) + " version, length, or hash does not match"});
    }
    return {};
}

Result<json> read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{"not_found", "TUF metadata version does not exist"});
    }
    json value = json::parse(input, nullptr, false);
    if (value.is_discarded()) {
        return std::unexpected(Error{"store_error", "stored TUF metadata is not valid JSON"});
    }
    return value;
}

Result<void> append_json(const std::filesystem::path& path, const json& document) {
    if (std::filesystem::exists(path)) {
        return std::unexpected(Error{"conflict", "TUF metadata version already exists"});
    }
    static std::atomic<std::uint64_t> next_temporary{0};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++next_temporary);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(Error{"store_error", "could not create TUF metadata"});
        }
        output << document.dump();
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return std::unexpected(Error{"store_error", "could not write TUF metadata"});
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return std::unexpected(
            Error{"store_error", "could not publish TUF metadata: " + error.message()});
    }
    return {};
}

} // namespace

struct Service::Impl {
    std::filesystem::path directory;
    bool valid_instance = false;
    mutable std::mutex mutex;

    Impl(std::filesystem::path store_root, std::string instance_name) {
        valid_instance = valid_instance_name(instance_name);
        if (valid_instance) {
            directory = std::move(store_root) / std::move(instance_name) / "log" / "tuf";
        }
    }

    std::filesystem::path path(std::string_view role, std::uint64_t version) const {
        return directory / (std::to_string(version) + "." + std::string(role) + ".json");
    }

    Result<std::uint64_t> latest_version(std::string_view role) const {
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) {
            return std::unexpected(Error{"not_found", "TUF metadata is not initialized"});
        }
        std::uint64_t latest = 0;
        const std::string suffix = "." + std::string(role) + ".json";
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) {
                return std::unexpected(
                    Error{"store_error", "could not list TUF metadata: " + error.message()});
            }
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (!name.ends_with(suffix)) {
                continue;
            }
            const std::string number = name.substr(0, name.size() - suffix.size());
            if (number.empty() || !std::ranges::all_of(number, [](unsigned char value) {
                    return std::isdigit(value);
                })) {
                continue;
            }
            try {
                latest = std::max(latest, static_cast<std::uint64_t>(std::stoull(number)));
            } catch (const std::exception&) {
                continue;
            }
        }
        if (latest == 0) {
            return std::unexpected(Error{"not_found", "TUF metadata role does not exist"});
        }
        return latest;
    }

    Result<Metadata> load(std::string_view role, std::optional<std::uint64_t> version) const {
        if (!valid_role(role)) {
            return std::unexpected(Error{"invalid_input", "unknown TUF metadata role"});
        }
        std::uint64_t selected = version.value_or(0);
        if (selected == 0) {
            const auto latest = latest_version(role);
            if (!latest) {
                return std::unexpected(latest.error());
            }
            selected = *latest;
        }
        const auto document = read_json(path(role, selected));
        if (!document) {
            return std::unexpected(document.error());
        }
        return Metadata{std::string(role), selected, document->dump(), *document};
    }
};

Service::Service(std::filesystem::path store_root, std::string instance_name)
    : impl_(std::make_shared<Impl>(std::move(store_root), std::move(instance_name))) {}

Result<Metadata> Service::initialize(const json& root) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    const auto existing = impl_->latest_version("root");
    if (existing) {
        return std::unexpected(Error{"conflict", "TUF metadata is already initialized"});
    }
    if (existing.error().code != "not_found") {
        return std::unexpected(existing.error());
    }
    const auto version = version_of(root, "root");
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != 1) {
        return std::unexpected(Error{"invalid_input", "initial root metadata must be version 1"});
    }
    const auto expiry = verify_not_expired(root, "root");
    if (!expiry) {
        return std::unexpected(expiry.error());
    }
    const auto shape = verify_root_shape(root);
    if (!shape) {
        return std::unexpected(shape.error());
    }
    const auto threshold = verify_threshold(root, root, "root");
    if (!threshold) {
        return std::unexpected(threshold.error());
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create transparency log: " + error.message()});
    }
    const auto appended = append_json(impl_->path("root", *version), root);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    return Metadata{"root", *version, root.dump(), root};
}

Result<Metadata> Service::rotate_root(const json& root) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    const auto current_version = impl_->latest_version("root");
    if (!current_version) {
        return std::unexpected(current_version.error());
    }
    const auto current = impl_->load("root", *current_version);
    if (!current) {
        return std::unexpected(current.error());
    }
    const auto version = version_of(root, "root");
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != *current_version + 1) {
        return std::unexpected(
            Error{"conflict", "root metadata must advance by exactly one version"});
    }
    const auto expiry = verify_not_expired(root, "root");
    if (!expiry) {
        return std::unexpected(expiry.error());
    }
    const auto shape = verify_root_shape(root);
    if (!shape) {
        return std::unexpected(shape.error());
    }
    const auto old_threshold = verify_threshold(root, current->document, "root");
    if (!old_threshold) {
        return std::unexpected(
            Error{"invalid_input", "new root does not meet the trusted root threshold"});
    }
    const auto new_threshold = verify_threshold(root, root, "root");
    if (!new_threshold) {
        return std::unexpected(
            Error{"invalid_input", "new root does not meet its own root threshold"});
    }
    const auto appended = append_json(impl_->path("root", *version), root);
    if (!appended) {
        return std::unexpected(appended.error());
    }
    return Metadata{"root", *version, root.dump(), root};
}

Result<json> Service::publish(const json& targets, const json& snapshot, const json& timestamp) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    const auto root = impl_->load("root", std::nullopt);
    if (!root) {
        return std::unexpected(root.error());
    }
    const auto targets_version = version_of(targets, "targets");
    const auto snapshot_version = version_of(snapshot, "snapshot");
    const auto timestamp_version = version_of(timestamp, "timestamp");
    if (!targets_version) {
        return std::unexpected(targets_version.error());
    }
    if (!snapshot_version) {
        return std::unexpected(snapshot_version.error());
    }
    if (!timestamp_version) {
        return std::unexpected(timestamp_version.error());
    }
    for (const auto& [role, document] : std::array<std::pair<std::string_view, const json*>, 3>{
             {{"targets", &targets}, {"snapshot", &snapshot}, {"timestamp", &timestamp}}}) {
        const auto expiry = verify_not_expired(*document, role);
        if (!expiry) {
            return std::unexpected(expiry.error());
        }
        const auto shape = verify_metadata_shape(*document, role);
        if (!shape) {
            return std::unexpected(shape.error());
        }
        const auto threshold = verify_threshold(*document, root->document, role);
        if (!threshold) {
            return std::unexpected(threshold.error());
        }
    }
    const auto targets_binding = verify_meta_binding(snapshot, "targets.json", targets);
    if (!targets_binding) {
        return std::unexpected(targets_binding.error());
    }
    const auto snapshot_binding = verify_meta_binding(timestamp, "snapshot.json", snapshot);
    if (!snapshot_binding) {
        return std::unexpected(snapshot_binding.error());
    }
    for (const auto& [role, version] : std::array<std::pair<std::string_view, std::uint64_t>, 3>{
             {{"targets", *targets_version},
              {"snapshot", *snapshot_version},
              {"timestamp", *timestamp_version}}}) {
        const auto current = impl_->latest_version(role);
        if (current && version <= *current) {
            return std::unexpected(
                Error{"conflict", std::string(role) + " metadata would roll back its version"});
        }
        if (!current && current.error().code != "not_found") {
            return std::unexpected(current.error());
        }
    }
    const auto targets_appended = append_json(impl_->path("targets", *targets_version), targets);
    if (!targets_appended) {
        return std::unexpected(targets_appended.error());
    }
    const auto snapshot_appended =
        append_json(impl_->path("snapshot", *snapshot_version), snapshot);
    if (!snapshot_appended) {
        return std::unexpected(snapshot_appended.error());
    }
    const auto timestamp_appended =
        append_json(impl_->path("timestamp", *timestamp_version), timestamp);
    if (!timestamp_appended) {
        return std::unexpected(timestamp_appended.error());
    }
    return json{{"root_version", root->version},
                {"targets_version", *targets_version},
                {"snapshot_version", *snapshot_version},
                {"timestamp_version", *timestamp_version}};
}

Result<Metadata> Service::metadata(const std::string& role,
                                   std::optional<std::uint64_t> version) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "Lake instance name is not a safe path component"});
    }
    return impl_->load(role, version);
}

const std::filesystem::path& Service::directory() const {
    return impl_->directory;
}

} // namespace lake::transparency
