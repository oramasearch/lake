#include "lake/transparency/checkpoint.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>

#include <openssl/evp.h>

namespace lake::transparency {
namespace {

using nlohmann::json;

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

bool valid_endpoint(std::string_view value) {
    if (value.starts_with("https://")) {
        return value.size() > std::string_view("https://").size();
    }
    if (!value.starts_with("http://")) {
        return false;
    }
    std::string_view authority = value.substr(std::string_view("http://").size());
    const std::size_t end = authority.find_first_of("/?#");
    if (end != std::string_view::npos) {
        authority = authority.substr(0, end);
    }
    return authority == "localhost" || authority.starts_with("localhost:") ||
           authority == "127.0.0.1" || authority.starts_with("127.0.0.1:") ||
           authority == "[::1]" || authority.starts_with("[::1]:");
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

bool signature_valid(std::string_view public_key_hex,
                     std::string_view body,
                     std::string_view signature_hex) {
    const auto public_key = decode_hex(public_key_hex);
    const auto signature = decode_hex(signature_hex);
    if (!public_key || public_key->size() != 32 || !signature || signature->size() != 64) {
        return false;
    }
    Pkey key(EVP_PKEY_new_raw_public_key(
        EVP_PKEY_ED25519, nullptr, public_key->data(), public_key->size()));
    if (!key) {
        return false;
    }
    Context context(EVP_MD_CTX_new());
    if (!context ||
        EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
        return false;
    }
    return EVP_DigestVerify(context.get(),
                            signature->data(),
                            signature->size(),
                            reinterpret_cast<const unsigned char*>(body.data()),
                            body.size()) == 1;
}

Result<void> write_json(const std::filesystem::path& path, const json& value, bool immutable) {
    if (immutable && std::filesystem::exists(path)) {
        return std::unexpected(Error{"conflict", "checkpoint object already exists"});
    }
    static std::atomic<std::uint64_t> next_temporary{0};
    const std::filesystem::path temporary =
        path.string() + ".tmp-" + std::to_string(++next_temporary);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return std::unexpected(Error{"store_error", "could not create checkpoint object"});
        }
        output << value.dump();
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            return std::unexpected(Error{"store_error", "could not write checkpoint object"});
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return std::unexpected(
            Error{"store_error", "could not publish checkpoint object: " + error.message()});
    }
    return {};
}

Result<json> read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(Error{"not_found", "no checkpoint has been published"});
    }
    json value = json::parse(input, nullptr, false);
    if (value.is_discarded()) {
        return std::unexpected(Error{"store_error", "stored checkpoint is not valid JSON"});
    }
    return value;
}

} // namespace

std::string_view to_string(LogMode mode) {
    switch (mode) {
    case LogMode::AirGapped:
        return "air-gapped";
    case LogMode::Witnessed:
        return "witnessed";
    case LogMode::Anchored:
        return "anchored";
    }
    return "unknown";
}

Result<LogConfiguration>
log_configuration_from_environment(const std::string& mode,
                                   const std::string& witnesses,
                                   std::uint64_t threshold,
                                   const std::string& aggregator_endpoint) {
    LogConfiguration configuration;
    if (mode == "air-gapped") {
        configuration.mode = LogMode::AirGapped;
    } else if (mode == "witnessed") {
        configuration.mode = LogMode::Witnessed;
    } else if (mode == "anchored") {
        configuration.mode = LogMode::Anchored;
    } else {
        return std::unexpected(
            Error{"invalid_input", "LAKE_LOG_MODE must be air-gapped, witnessed, or anchored"});
    }
    const json witness_values = json::parse(witnesses, nullptr, false);
    if (witness_values.is_discarded() || !witness_values.is_array()) {
        return std::unexpected(Error{"invalid_input", "LAKE_LOG_WITNESSES must be a JSON array"});
    }
    for (const json& value : witness_values) {
        if (!value.is_object() || !value.contains("endpoint") ||
            !value.at("endpoint").is_string() || !value.contains("public_key") ||
            !value.at("public_key").is_string()) {
            return std::unexpected(
                Error{"invalid_input",
                      "each LAKE_LOG_WITNESSES item needs endpoint and public_key strings"});
        }
        configuration.witnesses.push_back(
            {value.at("endpoint").get<std::string>(), value.at("public_key").get<std::string>()});
    }
    configuration.witness_threshold = static_cast<std::size_t>(threshold);
    configuration.aggregator_endpoint = aggregator_endpoint;
    const auto valid = validate_log_configuration(configuration);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    return configuration;
}

Result<void> validate_log_configuration(const LogConfiguration& configuration) {
    if (configuration.mode == LogMode::AirGapped) {
        if (!configuration.witnesses.empty() || configuration.witness_threshold != 0 ||
            !configuration.aggregator_endpoint.empty()) {
            return std::unexpected(
                Error{"invalid_input",
                      "air-gapped mode cannot configure witnesses or an anchoring aggregator"});
        }
        return {};
    }
    if (configuration.witness_threshold == 0 ||
        configuration.witness_threshold > configuration.witnesses.size()) {
        return std::unexpected(Error{
            "invalid_input", "witnessed and anchored modes need a quorum-satisfiable witness set"});
    }
    std::set<std::string> endpoints;
    std::set<std::string> public_keys;
    for (const Witness& witness : configuration.witnesses) {
        if (!valid_endpoint(witness.endpoint)) {
            return std::unexpected(
                Error{"invalid_input", "witness endpoint must be HTTPS or loopback HTTP"});
        }
        const auto public_key = decode_hex(witness.public_key);
        if (!public_key || public_key->size() != 32) {
            return std::unexpected(Error{
                "invalid_input", "witness public_key must be a 32-byte Ed25519 hexadecimal key"});
        }
        if (!endpoints.insert(witness.endpoint).second ||
            !public_keys.insert(witness.public_key).second) {
            return std::unexpected(
                Error{"invalid_input", "witness endpoints and public keys must be distinct"});
        }
    }
    if (configuration.mode == LogMode::Anchored &&
        !valid_endpoint(configuration.aggregator_endpoint)) {
        return std::unexpected(Error{
            "invalid_input", "anchored mode requires an HTTPS or loopback aggregation endpoint"});
    }
    return {};
}

json log_configuration_json(const LogConfiguration& configuration) {
    json witnesses = json::array();
    for (const Witness& witness : configuration.witnesses) {
        witnesses.push_back({{"endpoint", witness.endpoint}, {"public_key", witness.public_key}});
    }
    json value{{"mode", to_string(configuration.mode)},
               {"witnesses", std::move(witnesses)},
               {"witness_threshold", configuration.witness_threshold}};
    if (configuration.mode == LogMode::Anchored) {
        value["aggregator_endpoint"] = configuration.aggregator_endpoint;
    }
    return value;
}

struct CheckpointService::Impl {
    std::filesystem::path directory;
    LogConfiguration configuration;
    bool valid_instance = false;
    mutable std::mutex mutex;

    Impl(std::filesystem::path store_root, std::string instance_name, LogConfiguration configured)
        : configuration(std::move(configured)) {
        valid_instance = valid_instance_name(instance_name);
        if (valid_instance) {
            directory = std::move(store_root) / std::move(instance_name) / "log" / "checkpoints";
        }
    }

    Result<std::uint64_t> next_sequence() const {
        std::error_code error;
        std::uint64_t latest = 0;
        if (!std::filesystem::exists(directory, error)) {
            return 1;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) {
                return std::unexpected(
                    Error{"store_error", "could not list checkpoints: " + error.message()});
            }
            const std::string name = entry.path().filename().string();
            constexpr std::string_view suffix = ".checkpoint.json";
            if (!entry.is_regular_file() || !name.ends_with(suffix)) {
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
        return latest + 1;
    }
};

CheckpointService::CheckpointService(std::filesystem::path store_root,
                                     std::string instance_name,
                                     LogConfiguration configuration)
    : impl_(std::make_shared<Impl>(
          std::move(store_root), std::move(instance_name), std::move(configuration))) {}

Result<json> CheckpointService::publish(const json& checkpoint,
                                        const CheckpointSigner& signer,
                                        const TransparencySubmitter& submitter) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "lake instance name is not a safe path component"});
    }
    const auto configured = validate_log_configuration(impl_->configuration);
    if (!configured) {
        return std::unexpected(configured.error());
    }
    if (!checkpoint.is_object() || checkpoint.value("version", "") != "lake_checkpoint/1") {
        return std::unexpected(Error{"invalid_input", "checkpoint has an invalid version"});
    }
    if (!signer) {
        return std::unexpected(Error{"store_error", "no checkpoint signer is configured"});
    }
    const auto signature = signer(checkpoint);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    if (!signature->is_object() || !signature->contains("signer") ||
        !signature->at("signer").is_string() || !signature->contains("signature") ||
        !signature->at("signature").is_string()) {
        return std::unexpected(
            Error{"store_error", "checkpoint signer returned an invalid signature"});
    }
    const std::string encoded = checkpoint.dump();
    const auto checkpoint_hash = sha256(encoded);
    if (!checkpoint_hash) {
        return std::unexpected(checkpoint_hash.error());
    }
    json witness_signatures = json::array();
    if (impl_->configuration.mode != LogMode::AirGapped) {
        if (!submitter) {
            return std::unexpected(
                Error{"store_error", "no transparency outbound client is configured"});
        }
        for (const Witness& witness : impl_->configuration.witnesses) {
            const auto response = submitter(
                witness.endpoint,
                {{"checkpoint", checkpoint}, {"checkpoint_hash_sha256", *checkpoint_hash}});
            if (!response || !response->contains("signature") ||
                !response->at("signature").is_string()) {
                continue;
            }
            const std::string witness_signature = response->at("signature").get<std::string>();
            if (!signature_valid(witness.public_key, encoded, witness_signature)) {
                continue;
            }
            witness_signatures.push_back({{"endpoint", witness.endpoint},
                                          {"public_key", witness.public_key},
                                          {"signature", witness_signature}});
        }
        if (witness_signatures.size() < impl_->configuration.witness_threshold) {
            return std::unexpected(
                Error{"unrecoverable", "witness quorum did not co-sign checkpoint"});
        }
    }
    json anchor;
    if (impl_->configuration.mode == LogMode::Anchored) {
        const auto response = submitter(impl_->configuration.aggregator_endpoint,
                                        {{"hash", *checkpoint_hash}, {"hash_algorithm", "sha256"}});
        if (!response || !response->contains("proof")) {
            return std::unexpected(
                Error{"unrecoverable", "anchoring aggregator did not return a proof"});
        }
        anchor = {{"endpoint", impl_->configuration.aggregator_endpoint},
                  {"proof", response->at("proof")}};
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create checkpoint directory: " + error.message()});
    }
    const auto sequence = impl_->next_sequence();
    if (!sequence) {
        return std::unexpected(sequence.error());
    }
    json record{{"checkpoint", checkpoint},
                {"checkpoint_hash_sha256", *checkpoint_hash},
                {"sequence", *sequence},
                {"signature", *signature},
                {"witness_signatures", std::move(witness_signatures)}};
    if (!anchor.empty()) {
        record["anchor"] = anchor;
    }
    const std::filesystem::path checkpoint_path =
        impl_->directory / (std::to_string(*sequence) + ".checkpoint.json");
    const auto saved = write_json(checkpoint_path, record, true);
    if (!saved) {
        return std::unexpected(saved.error());
    }
    if (!anchor.empty()) {
        const auto proof_saved =
            write_json(impl_->directory / (std::to_string(*sequence) + ".ots.json"), anchor, true);
        if (!proof_saved) {
            return std::unexpected(proof_saved.error());
        }
    }
    const auto current = write_json(impl_->directory / "current.json", record, false);
    if (!current) {
        return std::unexpected(current.error());
    }
    return record;
}

Result<json> CheckpointService::adopt(const json& record) {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "lake instance name is not a safe path component"});
    }
    if (!record.is_object() || !record.contains("checkpoint") ||
        !record.at("checkpoint").is_object() || !record.contains("checkpoint_hash_sha256") ||
        !record.at("checkpoint_hash_sha256").is_string() || !record.contains("sequence") ||
        !record.at("sequence").is_number_unsigned() || !record.contains("signature") ||
        !record.at("signature").is_object() || !record.contains("witness_signatures") ||
        !record.at("witness_signatures").is_array()) {
        return std::unexpected(Error{"invalid_input", "replicated checkpoint is malformed"});
    }
    const std::uint64_t sequence = record.at("sequence").get<std::uint64_t>();
    if (sequence == 0) {
        return std::unexpected(Error{"invalid_input", "checkpoint sequence must be positive"});
    }
    const std::string encoded = record.at("checkpoint").dump();
    const auto checkpoint_hash = sha256(encoded);
    if (!checkpoint_hash ||
        *checkpoint_hash != record.at("checkpoint_hash_sha256").get<std::string>()) {
        return std::unexpected(
            Error{"unrecoverable", "replicated checkpoint hash does not match its body"});
    }
    std::set<std::string> accepted_witnesses;
    for (const json& signature : record.at("witness_signatures")) {
        if (!signature.is_object() || !signature.contains("endpoint") ||
            !signature.at("endpoint").is_string() || !signature.contains("public_key") ||
            !signature.at("public_key").is_string() || !signature.contains("signature") ||
            !signature.at("signature").is_string()) {
            continue;
        }
        const std::string endpoint = signature.at("endpoint").get<std::string>();
        const std::string public_key = signature.at("public_key").get<std::string>();
        const auto configured =
            std::ranges::find_if(impl_->configuration.witnesses, [&](const Witness& witness) {
                return witness.endpoint == endpoint && witness.public_key == public_key;
            });
        if (configured != impl_->configuration.witnesses.end() &&
            signature_valid(public_key, encoded, signature.at("signature").get<std::string>())) {
            accepted_witnesses.insert(public_key);
        }
    }
    if (accepted_witnesses.size() < impl_->configuration.witness_threshold) {
        return std::unexpected(
            Error{"unrecoverable", "replicated checkpoint lacks the configured witness quorum"});
    }
    if (impl_->configuration.mode == LogMode::Anchored &&
        (!record.contains("anchor") || !record.at("anchor").is_object() ||
         record.at("anchor").value("endpoint", "") != impl_->configuration.aggregator_endpoint ||
         !record.at("anchor").contains("proof"))) {
        return std::unexpected(
            Error{"unrecoverable", "replicated checkpoint lacks its configured anchor proof"});
    }
    std::error_code error;
    std::filesystem::create_directories(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not create checkpoint directory: " + error.message()});
    }
    const std::filesystem::path current_path = impl_->directory / "current.json";
    if (std::filesystem::exists(current_path)) {
        const auto current = read_json(current_path);
        if (!current || !current->contains("sequence") ||
            !current->at("sequence").is_number_unsigned()) {
            return std::unexpected(Error{"store_error", "current checkpoint is malformed"});
        }
        const std::uint64_t current_sequence = current->at("sequence").get<std::uint64_t>();
        if (current_sequence == sequence) {
            if (*current == record) {
                return record;
            }
            return std::unexpected(
                Error{"unrecoverable", "checkpoint sequence has conflicting contents"});
        }
        if (current_sequence > sequence) {
            return std::unexpected(Error{"conflict", "checkpoint replication would roll back"});
        }
    }
    const std::filesystem::path checkpoint_path =
        impl_->directory / (std::to_string(sequence) + ".checkpoint.json");
    if (std::filesystem::exists(checkpoint_path)) {
        const auto existing = read_json(checkpoint_path);
        if (!existing || *existing != record) {
            return std::unexpected(
                Error{"unrecoverable", "checkpoint sequence has conflicting contents"});
        }
    } else {
        const auto saved = write_json(checkpoint_path, record, true);
        if (!saved) {
            return std::unexpected(saved.error());
        }
    }
    if (record.contains("anchor")) {
        const std::filesystem::path anchor_path =
            impl_->directory / (std::to_string(sequence) + ".ots.json");
        if (!std::filesystem::exists(anchor_path)) {
            const auto saved = write_json(anchor_path, record.at("anchor"), true);
            if (!saved) {
                return std::unexpected(saved.error());
            }
        }
    }
    const auto current = write_json(current_path, record, false);
    if (!current) {
        return std::unexpected(current.error());
    }
    return record;
}

Result<json> CheckpointService::current() const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "lake instance name is not a safe path component"});
    }
    return read_json(impl_->directory / "current.json");
}

Result<json> CheckpointService::at_tree_size(std::uint64_t tree_size) const {
    std::unique_lock lock(impl_->mutex);
    if (!impl_->valid_instance) {
        return std::unexpected(
            Error{"invalid_input", "lake instance name is not a safe path component"});
    }
    std::error_code error;
    if (!std::filesystem::exists(impl_->directory, error)) {
        if (error) {
            return std::unexpected(
                Error{"store_error", "could not inspect checkpoint directory: " + error.message()});
        }
        return std::unexpected(
            Error{"not_found", "no checkpoint exists for the requested tree size"});
    }
    std::optional<json> selected;
    std::uint64_t selected_sequence = 0;
    constexpr std::string_view suffix = ".checkpoint.json";
    std::filesystem::directory_iterator entries(impl_->directory, error);
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not list checkpoints: " + error.message()});
    }
    const std::filesystem::directory_iterator end;
    for (; entries != end; entries.increment(error)) {
        const auto& entry = *entries;
        const std::string name = entry.path().filename().string();
        if (!entry.is_regular_file(error)) {
            if (error) {
                return std::unexpected(
                    Error{"store_error", "could not inspect checkpoint: " + error.message()});
            }
            continue;
        }
        if (!name.ends_with(suffix)) {
            continue;
        }
        const auto record = read_json(entry.path());
        if (!record || !record->is_object() || !record->contains("checkpoint") ||
            !record->at("checkpoint").is_object() ||
            !record->at("checkpoint").contains("tree_size") ||
            !record->at("checkpoint").at("tree_size").is_number_unsigned() ||
            !record->contains("sequence") || !record->at("sequence").is_number_unsigned()) {
            return std::unexpected(Error{"store_error", "stored checkpoint is malformed"});
        }
        if (record->at("checkpoint").at("tree_size").get<std::uint64_t>() != tree_size) {
            continue;
        }
        const std::uint64_t sequence = record->at("sequence").get<std::uint64_t>();
        if (!selected || sequence > selected_sequence) {
            selected = *record;
            selected_sequence = sequence;
        }
    }
    if (error) {
        return std::unexpected(
            Error{"store_error", "could not list checkpoints: " + error.message()});
    }
    if (!selected) {
        return std::unexpected(
            Error{"not_found", "no checkpoint exists for the requested tree size"});
    }
    return *selected;
}

json CheckpointService::configuration() const {
    return log_configuration_json(impl_->configuration);
}

const std::filesystem::path& CheckpointService::directory() const {
    return impl_->directory;
}

} // namespace lake::transparency
