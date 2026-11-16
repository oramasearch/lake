// SPDX-License-Identifier: MIT

// Secrets in their mount's workspace Zetta (docs/api/secrets.md, lake#328-#332): mounts, secret
// entries and versions, named encryption keys and data keys as sealed workspace records, with the
// envelope lake has always used (LAKE_INSTANCE_KEK -> customer KEK -> data key -> ciphertext).

#include "secret_records_api.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#include "../control/secrets_crypto.hpp"
#include "lake/control/handles.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/store.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;
using control::RecordCondition;
using control::StoredRecord;
using control::WorkspaceRecords;
using control::ZettaControlError;
using control::ZettaControlResult;
namespace crypto = control::secrets_crypto;

constexpr int write_attempts = 8;

template <class T>
ZettaControlResult<T> failure(std::string code, std::string message, std::string path = {}) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message), std::move(path)});
}

ZettaControlError from_store(const control::StoreError& error) {
    return ZettaControlError{error.code, error.message};
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string now_text() {
    return control::record_instant(now_us());
}

bool uuid_shaped(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        const unsigned char c = static_cast<unsigned char>(value[index]);
        if (dash ? c != '-' : std::isxdigit(c) == 0) {
            return false;
        }
    }
    return true;
}

// Overwrites a copy of key material when it goes out of scope.
struct Wipe {
    std::string& value;
    ~Wipe() {
        std::ranges::fill(value, '\0');
    }
};

std::string raw_of(const crypto::Key& key) {
    return std::string(reinterpret_cast<const char*>(key.data()), key.size());
}

ZettaControlResult<crypto::Key> key_of(const std::string& raw) {
    if (raw.size() != crypto::Key{}.size()) {
        return failure<crypto::Key>("store_error", "data key has the wrong size");
    }
    crypto::Key key{};
    std::copy(raw.begin(), raw.end(), key.begin());
    return key;
}

std::string secret_associated(const std::string& mount_id, const std::string& name,
                              std::uint64_t version) {
    return "lake secret/" + mount_id + "/" + name + "/" + std::to_string(version);
}

std::string transit_associated(const std::string& key_id, std::uint64_t version) {
    return "lake transit/" + key_id + "/" + std::to_string(version);
}

// Read-modify-write of one record at the engine revision read. `change` answers the next body,
// nullopt to leave the record as it is, or an error to stop; a write that loses to another is
// reread and asked again. Answers the record as it stands afterwards, nullopt when it is absent.
using Change = std::function<std::expected<std::optional<json>, ZettaControlError>(
    const StoredRecord&)>;

SecretTask<std::optional<StoredRecord>> rewrite(WorkspaceRecords& records,
                                                control::RecordCollection collection,
                                                std::string id, std::string lookup,
                                                Change change) {
    using Found = std::optional<StoredRecord>;
    using Result = ZettaControlResult<Found>;
    ZettaControlError last{"conflict", "the record kept changing; try again"};
    for (int attempt = 0; attempt < write_attempts; ++attempt) {
        auto got = control::settled(co_await records.get(collection, id));
        if (!got) {
            co_return Result(std::unexpected(got.error()));
        }
        if (!*got) {
            co_return Result(Found{});
        }
        auto next = change(**got);
        if (!next) {
            co_return Result(std::unexpected(next.error()));
        }
        if (!*next) {
            co_return Result(std::move(*got));
        }
        auto written = control::settled(
            co_await records.put(collection, id, lookup, std::move(**next),
                                 RecordCondition{.if_revision = (*got)->revision},
                                 (*got)->lake_revision + 1, (*got)->at_us));
        if (written) {
            co_return Result(Found(std::move(*written)));
        }
        if (written.error().code != "conflict") {
            co_return Result(std::unexpected(written.error()));
        }
        last = written.error();
    }
    co_return Result(std::unexpected(last));
}

// The key a data key record holds, unwrapped by the KEK version it names.
SecretTask<crypto::Key> read_data_key(control::Store& store, const std::string& instance_kek,
                                      WorkspaceRecords& records, const std::string& customer_id,
                                      std::string_view owner_kind, const std::string& owner_id,
                                      std::uint64_t version) {
    using Result = ZettaControlResult<crypto::Key>;
    auto got = control::settled(co_await records.get(
        control::SecretDataKeyCodec::collection,
        control::SecretDataKeyCodec::key(records, owner_kind, owner_id, version)));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    const auto decoded = *got ? control::SecretDataKeyCodec::decode(**got) : std::nullopt;
    if (!decoded || decoded->customer_id != customer_id || decoded->owner_kind != owner_kind ||
        decoded->owner_id != owner_id || decoded->key_version != version) {
        co_return failure<crypto::Key>("not_found", "no data key version exists");
    }
    auto raw = store.unwrap_secret_data_key(customer_id, owner_id, version, decoded->wrapped,
                                            instance_kek);
    if (!raw) {
        co_return Result(std::unexpected(from_store(raw.error())));
    }
    auto key = key_of(*raw);
    std::ranges::fill(*raw, '\0');
    co_return key;
}

// A new data key record, `if_absent`, wrapped by the current customer KEK. If a rotation made
// another version current meanwhile, the record is rewrapped by it before answering, so a rotation
// that has listed the workspace never leaves a key behind under the KEK it then forgets.
SecretTask<void> write_data_key(control::Store& store, const std::string& instance_kek,
                                WorkspaceRecords& records, const std::string& customer_id,
                                std::string owner_kind, std::string owner_id,
                                std::uint64_t version, const crypto::Key& key) {
    using Result = ZettaControlResult<void>;
    std::string raw = raw_of(key);
    const Wipe wipe{raw};
    auto wrapped = store.wrap_secret_data_key(customer_id, owner_id, version, raw, instance_kek);
    if (!wrapped) {
        co_return Result(std::unexpected(from_store(wrapped.error())));
    }
    control::SecretDataKey record{customer_id, owner_kind, owner_id, version, *wrapped};
    const std::string id = control::SecretDataKeyCodec::key(records, owner_kind, owner_id, version);
    const std::string lookup = control::SecretDataKeyCodec::lookup(customer_id);
    auto written = control::settled(co_await records.put(
        control::SecretDataKeyCodec::collection, id, lookup,
        control::SecretDataKeyCodec::encode(record), RecordCondition{.if_absent = true}));
    if (!written) {
        co_return Result(std::unexpected(written.error()));
    }
    auto state = store.customer_kek_state(customer_id);
    if (!state) {
        co_return Result(std::unexpected(from_store(state.error())));
    }
    if (state->version == wrapped->kek_version) {
        co_return Result{};
    }
    auto settled = control::settled(co_await rewrite(
        records, control::SecretDataKeyCodec::collection, id, lookup,
        [&](const StoredRecord& stored) -> std::expected<std::optional<json>, ZettaControlError> {
            auto current = control::SecretDataKeyCodec::decode(stored);
            if (!current) {
                return std::unexpected(ZettaControlError{"store_error", "data key did not decode"});
            }
            auto now = store.customer_kek_state(customer_id);
            if (!now) {
                return std::unexpected(from_store(now.error()));
            }
            if (current->wrapped.kek_version == now->version) {
                return std::nullopt;
            }
            auto rewrapped =
                store.wrap_secret_data_key(customer_id, owner_id, version, raw, instance_kek);
            if (!rewrapped) {
                return std::unexpected(from_store(rewrapped.error()));
            }
            current->wrapped = *rewrapped;
            return control::SecretDataKeyCodec::encode(*current);
        }));
    if (!settled) {
        co_return Result(std::unexpected(settled.error()));
    }
    co_return Result{};
}

json secret_value(const std::string& plaintext) {
    json value = json::parse(plaintext, nullptr, false);
    return value.is_object() ? value : json();
}

} // namespace

SecretTask<json> MountSecrets::remove_all(bool dry_run) {
    using Result = ZettaControlResult<json>;
    const std::string& mount_id = route_.mount_id;
    // Each record to remove: its collection and key, in the order they go. The mount record
    // goes last, so a deletion stopped part way still finds the mount and is finished by a retry.
    std::vector<std::pair<control::RecordCollection, std::string>> doomed;
    json names = json::array();
    std::map<std::string, std::int64_t> counts{{"secrets", 0},
                                               {"versions", 0},
                                               {"encryption_keys", 0},
                                               {"data_keys", 0},
                                               {"leases", 0},
                                               {"mount", 0}};
    auto entries = control::settled(co_await records_.list(
        control::SecretEntryCodec::collection, control::SecretEntryCodec::lookup(mount_id)));
    if (!entries) {
        co_return Result(std::unexpected(entries.error()));
    }
    for (const StoredRecord& stored : *entries) {
        const auto entry = control::SecretEntryCodec::decode(stored);
        if (!entry || entry->mount_id != mount_id) {
            continue;
        }
        auto versions = control::settled(
            co_await records_.list(control::SecretVersionCodec::collection,
                                   control::SecretVersionCodec::lookup(mount_id, entry->name)));
        if (!versions) {
            co_return Result(std::unexpected(versions.error()));
        }
        for (const StoredRecord& version : *versions) {
            const auto decoded = control::SecretVersionCodec::decode(version);
            if (decoded && decoded->mount_id == mount_id && decoded->name == entry->name) {
                doomed.emplace_back(control::SecretVersionCodec::collection, version.id);
                ++counts["versions"];
            }
        }
        doomed.emplace_back(control::SecretEntryCodec::collection, stored.id);
        ++counts["secrets"];
        names.push_back(entry->name);
    }
    std::set<std::string> key_ids;
    auto keys = control::settled(co_await records_.list(
        control::EncryptionKeyCodec::collection, control::EncryptionKeyCodec::lookup(mount_id)));
    if (!keys) {
        co_return Result(std::unexpected(keys.error()));
    }
    for (const StoredRecord& stored : *keys) {
        const auto decoded = control::EncryptionKeyCodec::decode(stored);
        if (decoded && decoded->key.mount_id == mount_id) {
            key_ids.insert(decoded->key.id);
            doomed.emplace_back(control::EncryptionKeyCodec::collection, stored.id);
            ++counts["encryption_keys"];
        }
    }
    auto leases = control::settled(co_await records_.list(
        control::SecretLeaseCodec::collection, control::SecretLeaseCodec::lookup(mount_id)));
    if (!leases) {
        co_return Result(std::unexpected(leases.error()));
    }
    for (const StoredRecord& stored : *leases) {
        const auto decoded = control::SecretLeaseCodec::decode(stored);
        if (decoded && decoded->mount_id == mount_id) {
            doomed.emplace_back(control::SecretLeaseCodec::collection, stored.id);
            ++counts["leases"];
        }
    }
    // The data keys of the space, of which this mount's and its named keys' go.
    auto data_keys = control::settled(
        co_await records_.list(control::SecretDataKeyCodec::collection,
                               control::SecretDataKeyCodec::lookup(route_.customer_id)));
    if (!data_keys) {
        co_return Result(std::unexpected(data_keys.error()));
    }
    for (const StoredRecord& stored : *data_keys) {
        const auto decoded = control::SecretDataKeyCodec::decode(stored);
        if (decoded &&
            ((decoded->owner_kind == "mount" && decoded->owner_id == mount_id) ||
             (decoded->owner_kind == "encryption" && key_ids.contains(decoded->owner_id)))) {
            doomed.emplace_back(control::SecretDataKeyCodec::collection, stored.id);
            ++counts["data_keys"];
        }
    }
    // The name claim, then the mount itself.
    auto mount = control::settled(co_await this->mount());
    if (mount) {
        doomed.emplace_back(control::SecretMountCodec::collection,
                            control::secret_mount_name_key(records_, *mount));
        doomed.emplace_back(control::SecretMountCodec::collection, mount_id);
        counts["mount"] = 1;
    } else if (mount.error().code != "not_found") {
        co_return Result(std::unexpected(mount.error()));
    }
    if (!dry_run) {
        for (const auto& [collection, key] : doomed) {
            auto removed = control::settled(co_await records_.remove(collection, key));
            if (!removed && removed.error().code != "not_found") {
                co_return Result(std::unexpected(removed.error()));
            }
        }
    }
    json counted = json::object();
    for (const auto& [kind, count] : counts) {
        counted[kind] = count;
    }
    co_return Result(json{{"counts", std::move(counted)}, {"secrets", std::move(names)}});
}

ApiResult secret_records_failure(const ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

std::expected<MountSecrets, ApiError> MountSecrets::open(const ApiContext& context,
                                                         const std::string& mount_id) {
    if (!context.store) {
        return std::unexpected(ApiError{"store_unavailable",
                                        "no control-plane store is configured", {}});
    }
    if (!uuid_shaped(mount_id)) {
        return std::unexpected(ApiError{"not_found", "no secret mount " + mount_id, "/mount_id"});
    }
    auto route = context.store->get_secret_mount_route(mount_id);
    if (!route) {
        return std::unexpected(ApiError{route.error().code, route.error().message, "/mount_id"});
    }
    auto records = workspace_records(context, route->customer_id, route->workspace_id);
    if (!records) {
        return std::unexpected(ApiError{records.error().code, records.error().message, {}});
    }
    return MountSecrets(std::move(*records), std::move(*route), context.store,
                        context.instance_kek);
}

SecretTask<control::SecretMount> MountSecrets::mount() {
    using Result = ZettaControlResult<control::SecretMount>;
    auto got = control::settled(
        co_await records_.get(control::SecretMountCodec::collection, route_.mount_id));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    auto decoded = *got ? control::SecretMountCodec::decode(**got) : std::nullopt;
    if (!decoded || decoded->customer_id != route_.customer_id ||
        decoded->workspace_id != route_.workspace_id) {
        co_return failure<control::SecretMount>("not_found", "no secret mount " + route_.mount_id,
                                                "/mount_id");
    }
    co_return Result(std::move(*decoded));
}

SecretTask<control::SecretVersion> MountSecrets::put(control::PutSecret input) {
    using Result = ZettaControlResult<control::SecretVersion>;
    if (input.name.empty() || input.name.size() > 1024 || !input.value.is_object() ||
        !input.custom_metadata.is_object()) {
        co_return failure<control::SecretVersion>(
            "invalid_input", "secret name, object value, and object metadata are required");
    }
    const std::string& mount_id = route_.mount_id;
    auto key = control::settled(co_await read_data_key(*store_, instance_kek_, records_,
                                                       route_.customer_id, "mount", mount_id, 1));
    if (!key) {
        co_return Result(std::unexpected(key.error()));
    }
    const auto entries = control::SecretEntryCodec::collection;
    const auto versions = control::SecretVersionCodec::collection;
    const std::string entry_key = control::SecretEntryCodec::key(records_, mount_id, input.name);
    const std::string entry_lookup = control::SecretEntryCodec::lookup(mount_id);
    const std::string version_lookup = control::SecretVersionCodec::lookup(mount_id, input.name);
    ZettaControlError last{"conflict", "the secret kept changing; try again"};
    for (int attempt = 0; attempt < write_attempts; ++attempt) {
        auto got = control::settled(co_await records_.get(entries, entry_key));
        if (!got) {
            crypto::cleanse(*key);
            co_return Result(std::unexpected(got.error()));
        }
        std::optional<control::SecretEntry> entry =
            *got ? control::SecretEntryCodec::decode(**got) : std::nullopt;
        if (*got && !entry) {
            crypto::cleanse(*key);
            co_return failure<control::SecretVersion>("store_error", "secret entry did not decode");
        }
        if (!entry) {
            // The first value of this name: its entry, at version 0, written once.
            control::SecretEntry created{mount_id, input.name, 0, input.custom_metadata};
            created.updated_at = now_text();
            auto written = control::settled(
                co_await records_.put(entries, entry_key, entry_lookup,
                                      control::SecretEntryCodec::encode(created),
                                      RecordCondition{.if_absent = true}));
            if (!written && written.error().code == "conflict") {
                continue;
            }
            if (!written) {
                crypto::cleanse(*key);
                co_return Result(std::unexpected(written.error()));
            }
            entry = control::SecretEntryCodec::decode(*written);
        }
        if (input.check_and_set != 0 && input.check_and_set != entry->current_version) {
            crypto::cleanse(*key);
            co_return failure<control::SecretVersion>("conflict", "secret version changed",
                                                      "/check_and_set");
        }
        const std::uint64_t version = entry->current_version + 1;
        const json metadata =
            input.custom_metadata.empty() ? entry->custom_metadata : input.custom_metadata;
        auto ciphertext = crypto::seal(*key, input.value.dump(),
                                       secret_associated(mount_id, input.name, version));
        if (!ciphertext) {
            crypto::cleanse(*key);
            co_return Result(std::unexpected(from_store(ciphertext.error())));
        }
        control::SecretVersionRecord record;
        record.mount_id = mount_id;
        record.name = input.name;
        record.version = version;
        record.ciphertext = *ciphertext;
        record.metadata = metadata;
        auto claimed = control::settled(co_await records_.put(
            versions, control::SecretVersionCodec::key(records_, mount_id, input.name, version),
            version_lookup, control::SecretVersionCodec::encode(record),
            RecordCondition{.if_absent = true}));
        if (!claimed && claimed.error().code != "conflict") {
            crypto::cleanse(*key);
            co_return Result(std::unexpected(claimed.error()));
        }
        if (!claimed) {
            // The number is taken: by a concurrent put, or by one that stopped before moving the
            // entry. Move the entry to the newest version that exists, as that put would have.
            last = claimed.error();
            auto listed = control::settled(co_await records_.list(versions, version_lookup));
            if (!listed) {
                crypto::cleanse(*key);
                co_return Result(std::unexpected(listed.error()));
            }
            std::optional<control::SecretVersionRecord> newest;
            for (const StoredRecord& stored : *listed) {
                auto decoded = control::SecretVersionCodec::decode(stored);
                if (decoded && decoded->mount_id == mount_id && decoded->name == input.name &&
                    (!newest || decoded->version > newest->version)) {
                    newest = std::move(decoded);
                }
            }
            if (newest) {
                auto moved = control::settled(co_await rewrite(
                    records_, entries, entry_key, entry_lookup,
                    [&](const StoredRecord& stored)
                        -> std::expected<std::optional<json>, ZettaControlError> {
                        auto current = control::SecretEntryCodec::decode(stored);
                        if (!current || current->current_version >= newest->version) {
                            return std::nullopt;
                        }
                        current->current_version = newest->version;
                        if (newest->metadata) {
                            current->custom_metadata = *newest->metadata;
                        }
                        current->updated_at = now_text();
                        return control::SecretEntryCodec::encode(*current);
                    }));
                if (!moved) {
                    crypto::cleanse(*key);
                    co_return Result(std::unexpected(moved.error()));
                }
            }
            if (input.check_and_set != 0) {
                crypto::cleanse(*key);
                co_return failure<control::SecretVersion>("conflict", "secret version changed",
                                                          "/check_and_set");
            }
            continue;
        }
        // The version is claimed; the entry moves forward to it unless a later writer already
        // moved it further.
        auto moved = control::settled(co_await rewrite(
            records_, entries, entry_key, entry_lookup,
            [&](const StoredRecord& stored)
                -> std::expected<std::optional<json>, ZettaControlError> {
                auto current = control::SecretEntryCodec::decode(stored);
                if (!current) {
                    return std::unexpected(
                        ZettaControlError{"store_error", "secret entry did not decode"});
                }
                if (current->current_version >= version) {
                    return std::nullopt;
                }
                current->current_version = version;
                current->custom_metadata = metadata;
                current->updated_at = now_text();
                return control::SecretEntryCodec::encode(*current);
            }));
        crypto::cleanse(*key);
        if (!moved) {
            co_return Result(std::unexpected(moved.error()));
        }
        co_return Result(control::SecretVersion{mount_id, input.name, version, input.value,
                                                metadata, control::record_instant(claimed->at_us),
                                                "", ""});
    }
    crypto::cleanse(*key);
    co_return Result(std::unexpected(last));
}

SecretTask<control::SecretVersion> MountSecrets::read(std::string name, std::uint64_t version) {
    using Result = ZettaControlResult<control::SecretVersion>;
    const std::string& mount_id = route_.mount_id;
    auto got = control::settled(co_await records_.get(
        control::SecretEntryCodec::collection,
        control::SecretEntryCodec::key(records_, mount_id, name)));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    const auto entry = *got ? control::SecretEntryCodec::decode(**got) : std::nullopt;
    if (!entry || entry->mount_id != mount_id || entry->name != name) {
        co_return failure<control::SecretVersion>("not_found", "no secret named " + name, "/name");
    }
    const std::uint64_t selected = version == 0 ? entry->current_version : version;
    if (selected == 0) {
        co_return failure<control::SecretVersion>(
            "not_found", "secret " + name + " has no live version", "/name");
    }
    auto stored = control::settled(co_await records_.get(
        control::SecretVersionCodec::collection,
        control::SecretVersionCodec::key(records_, mount_id, name, selected)));
    if (!stored) {
        co_return Result(std::unexpected(stored.error()));
    }
    const auto record = *stored ? control::SecretVersionCodec::decode(**stored) : std::nullopt;
    if (!record || record->mount_id != mount_id || record->name != name ||
        record->version != selected) {
        co_return failure<control::SecretVersion>("not_found",
                                                  "secret " + name + " has no version " +
                                                      std::to_string(selected),
                                                  version == 0 ? "/name" : "/version");
    }
    const std::string at = version == 0 ? "/name" : "/version";
    if (!record->destroyed_at.empty()) {
        co_return failure<control::SecretVersion>("not_found",
                                                  "version " + std::to_string(selected) +
                                                      " of secret " + name + " was destroyed",
                                                  at);
    }
    if (!record->deleted_at.empty()) {
        co_return failure<control::SecretVersion>(
            "not_found",
            "version " + std::to_string(selected) + " of secret " + name +
                " is deleted; undelete it with undelete_secret_versions",
            at);
    }
    auto key = control::settled(co_await read_data_key(*store_, instance_kek_, records_,
                                                       route_.customer_id, "mount", mount_id, 1));
    if (!key) {
        co_return Result(std::unexpected(key.error()));
    }
    auto plaintext =
        crypto::open(*key, *record->ciphertext, secret_associated(mount_id, name, selected));
    crypto::cleanse(*key);
    if (!plaintext) {
        co_return Result(std::unexpected(from_store(plaintext.error())));
    }
    json value = secret_value(*plaintext);
    if (value.is_null()) {
        co_return failure<control::SecretVersion>("store_error", "stored secret is not valid JSON");
    }
    co_return Result(control::SecretVersion{mount_id, std::move(name), selected, std::move(value),
                                            entry->custom_metadata, record->created_at, "", ""});
}

SecretTask<control::SecretMetadata> MountSecrets::metadata(std::string name) {
    using Result = ZettaControlResult<control::SecretMetadata>;
    const std::string& mount_id = route_.mount_id;
    auto got = control::settled(co_await records_.get(
        control::SecretEntryCodec::collection,
        control::SecretEntryCodec::key(records_, mount_id, name)));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    const auto entry = *got ? control::SecretEntryCodec::decode(**got) : std::nullopt;
    if (!entry || entry->mount_id != mount_id || entry->name != name) {
        co_return failure<control::SecretMetadata>("not_found", "no secret named " + name, "/name");
    }
    auto listed = control::settled(co_await records_.list(
        control::SecretVersionCodec::collection,
        control::SecretVersionCodec::lookup(mount_id, name)));
    if (!listed) {
        co_return Result(std::unexpected(listed.error()));
    }
    control::SecretMetadata metadata{mount_id, name, entry->current_version,
                                     entry->custom_metadata, {}, entry->created_at,
                                     entry->updated_at};
    for (const StoredRecord& stored : *listed) {
        auto version = control::SecretVersionCodec::decode(stored);
        if (version && version->mount_id == mount_id && version->name == name) {
            metadata.versions.push_back({version->version, version->created_at,
                                         version->deleted_at, version->destroyed_at});
        }
    }
    std::ranges::sort(metadata.versions, {}, &control::SecretVersionMetadata::version);
    co_return Result(std::move(metadata));
}

SecretTask<SecretPage>
MountSecrets::list(std::string prefix, std::string after, std::size_t limit) {
    using Result = ZettaControlResult<SecretPage>;
    const std::string& mount_id = route_.mount_id;
    auto listed = control::settled(co_await records_.list(
        control::SecretEntryCodec::collection, control::SecretEntryCodec::lookup(mount_id)));
    if (!listed) {
        co_return Result(std::unexpected(listed.error()));
    }
    std::vector<control::SecretEntry> entries;
    for (const StoredRecord& stored : *listed) {
        auto entry = control::SecretEntryCodec::decode(stored);
        if (entry && entry->mount_id == mount_id && entry->name.starts_with(prefix) &&
            (after.empty() || entry->name > after)) {
            entries.push_back(std::move(*entry));
        }
    }
    std::ranges::sort(entries, {}, &control::SecretEntry::name);
    SecretPage page;
    for (const control::SecretEntry& entry : entries) {
        if (page.items.size() == limit) {
            page.next_cursor = page.items.back().name;
            break;
        }
        SecretListing item{
            entry.name, entry.current_version, entry.created_at, entry.updated_at, "", ""};
        if (entry.current_version > 0) {
            auto stored = control::settled(
                co_await records_.get(control::SecretVersionCodec::collection,
                                      control::SecretVersionCodec::key(
                                          records_, mount_id, entry.name, entry.current_version)));
            if (!stored) {
                co_return Result(std::unexpected(stored.error()));
            }
            const auto version =
                *stored ? control::SecretVersionCodec::decode(**stored) : std::nullopt;
            if (version && version->mount_id == mount_id && version->name == entry.name) {
                item.deleted_at = version->deleted_at;
                item.destroyed_at = version->destroyed_at;
            }
        }
        page.items.push_back(std::move(item));
    }
    co_return Result(std::move(page));
}

SecretTask<control::SecretMetadata> MountSecrets::change(std::string name,
                                                         std::vector<std::uint64_t> versions,
                                                         control::SecretVersionAction action) {
    using Result = ZettaControlResult<control::SecretMetadata>;
    if (versions.empty()) {
        co_return failure<control::SecretMetadata>("invalid_input",
                                                   "at least one version is required");
    }
    const std::string& mount_id = route_.mount_id;
    const auto entries = control::SecretEntryCodec::collection;
    const std::string entry_key = control::SecretEntryCodec::key(records_, mount_id, name);
    auto got = control::settled(co_await records_.get(entries, entry_key));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    const auto entry = *got ? control::SecretEntryCodec::decode(**got) : std::nullopt;
    if (!entry || entry->mount_id != mount_id || entry->name != name) {
        co_return failure<control::SecretMetadata>("not_found", "no secret named " + name, "/name");
    }
    const auto collection = control::SecretVersionCodec::collection;
    const std::string lookup = control::SecretVersionCodec::lookup(mount_id, name);
    // Every named version must exist, and none may be restored once destroyed, before any changes.
    for (const std::uint64_t version : versions) {
        auto stored = control::settled(co_await records_.get(
            collection, control::SecretVersionCodec::key(records_, mount_id, name, version)));
        if (!stored) {
            co_return Result(std::unexpected(stored.error()));
        }
        const auto record = *stored ? control::SecretVersionCodec::decode(**stored) : std::nullopt;
        if (!record || record->mount_id != mount_id || record->name != name) {
            co_return failure<control::SecretMetadata>("not_found",
                                                       "secret " + name + " has no version " +
                                                           std::to_string(version),
                                                       "/versions");
        }
        if (action == control::SecretVersionAction::Undelete && !record->destroyed_at.empty()) {
            co_return failure<control::SecretMetadata>(
                "conflict", "a destroyed secret version cannot be restored");
        }
    }
    // Each transition is written at the revision it read, so a concurrent destroy is never undone
    // by a delete or undelete computed from an older version of the record.
    for (const std::uint64_t version : versions) {
        auto changed = control::settled(co_await rewrite(
            records_, collection, control::SecretVersionCodec::key(records_, mount_id, name, version),
            lookup,
            [action](const StoredRecord& stored)
                -> std::expected<std::optional<json>, ZettaControlError> {
                auto record = control::SecretVersionCodec::decode(stored);
                if (!record) {
                    return std::unexpected(
                        ZettaControlError{"store_error", "secret version did not decode"});
                }
                const std::string now = now_text();
                if (action == control::SecretVersionAction::Delete) {
                    if (!record->destroyed_at.empty() || !record->deleted_at.empty()) {
                        return std::nullopt;
                    }
                    record->deleted_at = now;
                } else if (action == control::SecretVersionAction::Undelete) {
                    if (!record->destroyed_at.empty()) {
                        return std::unexpected(ZettaControlError{
                            "conflict", "a destroyed secret version cannot be restored"});
                    }
                    if (record->deleted_at.empty()) {
                        return std::nullopt;
                    }
                    record->deleted_at.clear();
                } else {
                    if (!record->destroyed_at.empty()) {
                        return std::nullopt;
                    }
                    record->ciphertext.reset();
                    if (record->deleted_at.empty()) {
                        record->deleted_at = now;
                    }
                    record->destroyed_at = now;
                }
                return control::SecretVersionCodec::encode(*record);
            }));
        if (!changed) {
            co_return Result(std::unexpected(changed.error()));
        }
        if (!*changed) {
            co_return failure<control::SecretMetadata>("not_found",
                                                       "secret " + name + " has no version " +
                                                           std::to_string(version),
                                                       "/versions");
        }
    }
    auto touched = control::settled(co_await rewrite(
        records_, entries, entry_key, control::SecretEntryCodec::lookup(mount_id),
        [](const StoredRecord& stored) -> std::expected<std::optional<json>, ZettaControlError> {
            auto current = control::SecretEntryCodec::decode(stored);
            if (!current) {
                return std::unexpected(
                    ZettaControlError{"store_error", "secret entry did not decode"});
            }
            current->updated_at = now_text();
            return control::SecretEntryCodec::encode(*current);
        }));
    if (!touched) {
        co_return Result(std::unexpected(touched.error()));
    }
    co_return control::settled(co_await metadata(std::move(name)));
}

SecretTask<control::EncryptionKey> MountSecrets::create_key(std::string name) {
    using Result = ZettaControlResult<control::EncryptionKey>;
    if (!control::valid_handle(name)) {
        co_return failure<control::EncryptionKey>(
            "invalid_input", control::handle_rule("an encryption key's name"), "/name");
    }
    auto mount = control::settled(co_await this->mount());
    if (!mount) {
        co_return Result(std::unexpected(mount.error()));
    }
    control::EncryptionKey key{control::new_uuid_v4(), route_.mount_id, name, "", 1, "", ""};
    auto material = crypto::random_key();
    if (!material) {
        co_return Result(std::unexpected(from_store(material.error())));
    }
    auto stored = control::settled(co_await write_data_key(
        *store_, instance_kek_, records_, route_.customer_id, "encryption", key.id, 1, *material));
    crypto::cleanse(*material);
    if (!stored) {
        co_return Result(std::unexpected(stored.error()));
    }
    key.updated_at = now_text();
    auto written = control::settled(co_await records_.put(
        control::EncryptionKeyCodec::collection,
        control::EncryptionKeyCodec::key(records_, route_.mount_id, name),
        control::EncryptionKeyCodec::lookup(route_.mount_id),
        control::EncryptionKeyCodec::encode(key), RecordCondition{.if_absent = true}));
    if (!written && written.error().code == "conflict") {
        co_return failure<control::EncryptionKey>("conflict", "encryption key already exists",
                                                  "/name");
    }
    if (!written) {
        co_return Result(std::unexpected(written.error()));
    }
    auto decoded = control::EncryptionKeyCodec::decode(*written);
    if (!decoded) {
        co_return failure<control::EncryptionKey>("store_error", "encryption key did not decode");
    }
    co_return Result(std::move(decoded->key));
}

SecretTask<std::vector<control::EncryptionKey>> MountSecrets::list_keys(int limit) {
    using Keys = std::vector<control::EncryptionKey>;
    using Result = ZettaControlResult<Keys>;
    auto listed = control::settled(co_await records_.list(
        control::EncryptionKeyCodec::collection,
        control::EncryptionKeyCodec::lookup(route_.mount_id),
        limit > 0 ? static_cast<std::size_t>(limit) : 0));
    if (!listed) {
        co_return Result(std::unexpected(listed.error()));
    }
    Keys keys;
    for (auto stored = listed->rbegin(); stored != listed->rend(); ++stored) {
        auto decoded = control::EncryptionKeyCodec::decode(*stored);
        if (decoded && decoded->key.mount_id == route_.mount_id) {
            keys.push_back(std::move(decoded->key));
        }
    }
    co_return Result(std::move(keys));
}

SecretTask<control::EncryptionKey> MountSecrets::get_key(std::string name) {
    using Result = ZettaControlResult<control::EncryptionKey>;
    auto got = control::settled(co_await records_.get(
        control::EncryptionKeyCodec::collection,
        control::EncryptionKeyCodec::key(records_, route_.mount_id, name)));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    auto decoded = *got ? control::EncryptionKeyCodec::decode(**got) : std::nullopt;
    if (!decoded || decoded->key.mount_id != route_.mount_id || decoded->key.name != name) {
        co_return failure<control::EncryptionKey>("not_found", "encryption key does not exist");
    }
    co_return Result(std::move(decoded->key));
}

SecretTask<control::EncryptionKey> MountSecrets::rotate_key(std::string name) {
    using Result = ZettaControlResult<control::EncryptionKey>;
    const auto collection = control::EncryptionKeyCodec::collection;
    const std::string id = control::EncryptionKeyCodec::key(records_, route_.mount_id, name);
    const std::string lookup = control::EncryptionKeyCodec::lookup(route_.mount_id);
    ZettaControlError last{"conflict", "the encryption key kept changing; try again"};
    for (int attempt = 0; attempt < write_attempts; ++attempt) {
        auto key = control::settled(co_await get_key(name));
        if (!key) {
            co_return Result(std::unexpected(key.error()));
        }
        const std::uint64_t version = key->current_version + 1;
        auto material = crypto::random_key();
        if (!material) {
            co_return Result(std::unexpected(from_store(material.error())));
        }
        auto claimed = control::settled(
            co_await write_data_key(*store_, instance_kek_, records_, route_.customer_id,
                                    "encryption", key->id, version, *material));
        crypto::cleanse(*material);
        if (!claimed && claimed.error().code != "conflict") {
            co_return Result(std::unexpected(claimed.error()));
        }
        // Taken: by a concurrent rotation, or one that stopped before moving the key. Either way
        // the key moves to it, and this rotation claims the next number.
        auto moved = control::settled(co_await rewrite(
            records_, collection, id, lookup,
            [version](const StoredRecord& stored)
                -> std::expected<std::optional<json>, ZettaControlError> {
                auto current = control::EncryptionKeyCodec::decode(stored);
                if (!current) {
                    return std::unexpected(
                        ZettaControlError{"store_error", "encryption key did not decode"});
                }
                if (current->key.current_version >= version) {
                    return std::nullopt;
                }
                current->key.current_version = version;
                current->key.updated_at = now_text();
                return control::EncryptionKeyCodec::encode(current->key);
            }));
        if (!moved) {
            co_return Result(std::unexpected(moved.error()));
        }
        if (!claimed) {
            last = claimed.error();
            continue;
        }
        auto decoded = *moved ? control::EncryptionKeyCodec::decode(**moved) : std::nullopt;
        if (!decoded) {
            co_return failure<control::EncryptionKey>("not_found",
                                                      "encryption key does not exist");
        }
        decoded->key.current_version = std::max(decoded->key.current_version, version);
        co_return Result(std::move(decoded->key));
    }
    co_return Result(std::unexpected(last));
}

SecretTask<control::TransitValue> MountSecrets::encrypt(std::string name, std::string plaintext) {
    using Result = ZettaControlResult<control::TransitValue>;
    auto key = control::settled(co_await get_key(name));
    if (!key) {
        co_return Result(std::unexpected(key.error()));
    }
    auto decoded = crypto::base64url_decode(plaintext);
    if (!decoded) {
        co_return Result(std::unexpected(from_store(decoded.error())));
    }
    auto material = control::settled(
        co_await read_data_key(*store_, instance_kek_, records_, route_.customer_id, "encryption",
                               key->id, key->current_version));
    if (!material) {
        co_return Result(std::unexpected(material.error()));
    }
    auto encryption_key = crypto::derive(*material, "lake transit encryption v1");
    crypto::cleanse(*material);
    if (!encryption_key) {
        co_return Result(std::unexpected(from_store(encryption_key.error())));
    }
    auto sealed = crypto::seal(*encryption_key, *decoded,
                               transit_associated(key->id, key->current_version));
    crypto::cleanse(*encryption_key);
    if (!sealed) {
        co_return Result(std::unexpected(from_store(sealed.error())));
    }
    co_return Result(control::TransitValue{
        crypto::format_ciphertext({key->id, key->current_version, *sealed}), key->current_version,
        "aes-256-gcm"});
}

SecretTask<control::TransitValue> MountSecrets::decrypt(std::string name, std::string ciphertext) {
    using Result = ZettaControlResult<control::TransitValue>;
    auto key = control::settled(co_await get_key(name));
    if (!key) {
        co_return Result(std::unexpected(key.error()));
    }
    auto parsed = crypto::parse_ciphertext(ciphertext);
    if (!parsed) {
        co_return Result(std::unexpected(from_store(parsed.error())));
    }
    if (parsed->key_id != key->id) {
        co_return failure<control::TransitValue>("invalid_input", "ciphertext names another key");
    }
    auto material = control::settled(
        co_await read_data_key(*store_, instance_kek_, records_, route_.customer_id, "encryption",
                               key->id, parsed->version));
    if (!material) {
        co_return Result(std::unexpected(material.error()));
    }
    auto encryption_key = crypto::derive(*material, "lake transit encryption v1");
    crypto::cleanse(*material);
    if (!encryption_key) {
        co_return Result(std::unexpected(from_store(encryption_key.error())));
    }
    auto plaintext = crypto::open(*encryption_key, parsed->sealed,
                                  transit_associated(key->id, parsed->version));
    crypto::cleanse(*encryption_key);
    if (!plaintext) {
        co_return Result(std::unexpected(from_store(plaintext.error())));
    }
    auto encoded = crypto::base64url_encode(*plaintext);
    if (!encoded) {
        co_return Result(std::unexpected(from_store(encoded.error())));
    }
    co_return Result(control::TransitValue{*encoded, parsed->version, "base64url"});
}

SecretTask<control::TransitValue> MountSecrets::rewrap(std::string name, std::string ciphertext) {
    using Result = ZettaControlResult<control::TransitValue>;
    auto opened = control::settled(co_await decrypt(name, std::move(ciphertext)));
    if (!opened) {
        co_return Result(std::unexpected(opened.error()));
    }
    co_return control::settled(co_await encrypt(std::move(name), std::move(opened->value)));
}

SecretTask<control::TransitValue> MountSecrets::sign(std::string name, std::string message,
                                                     std::uint64_t version) {
    using Result = ZettaControlResult<control::TransitValue>;
    auto key = control::settled(co_await get_key(name));
    if (!key) {
        co_return Result(std::unexpected(key.error()));
    }
    const std::uint64_t selected = version == 0 ? key->current_version : version;
    auto decoded = crypto::base64url_decode(message);
    if (!decoded) {
        co_return Result(std::unexpected(from_store(decoded.error())));
    }
    auto material = control::settled(co_await read_data_key(
        *store_, instance_kek_, records_, route_.customer_id, "encryption", key->id, selected));
    if (!material) {
        co_return Result(std::unexpected(material.error()));
    }
    auto signing_key = crypto::derive(*material, "lake transit signing v1");
    crypto::cleanse(*material);
    if (!signing_key) {
        co_return Result(std::unexpected(from_store(signing_key.error())));
    }
    auto signature = crypto::sign(*signing_key, *decoded);
    crypto::cleanse(*signing_key);
    if (!signature) {
        co_return Result(std::unexpected(from_store(signature.error())));
    }
    co_return Result(control::TransitValue{*signature, selected, "ed25519"});
}

SecretTask<control::TransitVerification> MountSecrets::verify(std::string name,
                                                              std::string message,
                                                              std::string signature,
                                                              std::uint64_t version) {
    using Result = ZettaControlResult<control::TransitVerification>;
    auto key = control::settled(co_await get_key(name));
    if (!key) {
        co_return Result(std::unexpected(key.error()));
    }
    const std::uint64_t selected = version == 0 ? key->current_version : version;
    auto decoded = crypto::base64url_decode(message);
    if (!decoded) {
        co_return Result(std::unexpected(from_store(decoded.error())));
    }
    auto material = control::settled(co_await read_data_key(
        *store_, instance_kek_, records_, route_.customer_id, "encryption", key->id, selected));
    if (!material) {
        co_return Result(std::unexpected(material.error()));
    }
    auto signing_key = crypto::derive(*material, "lake transit signing v1");
    crypto::cleanse(*material);
    if (!signing_key) {
        co_return Result(std::unexpected(from_store(signing_key.error())));
    }
    auto valid = crypto::verify(*signing_key, *decoded, signature);
    crypto::cleanse(*signing_key);
    if (!valid) {
        co_return Result(std::unexpected(from_store(valid.error())));
    }
    co_return Result(control::TransitVerification{*valid, selected, "ed25519"});
}

SecretTask<control::TransitValue> MountSecrets::hmac(std::string name, std::string message,
                                                     std::uint64_t version) {
    using Result = ZettaControlResult<control::TransitValue>;
    auto key = control::settled(co_await get_key(name));
    if (!key) {
        co_return Result(std::unexpected(key.error()));
    }
    const std::uint64_t selected = version == 0 ? key->current_version : version;
    auto decoded = crypto::base64url_decode(message);
    if (!decoded) {
        co_return Result(std::unexpected(from_store(decoded.error())));
    }
    auto material = control::settled(co_await read_data_key(
        *store_, instance_kek_, records_, route_.customer_id, "encryption", key->id, selected));
    if (!material) {
        co_return Result(std::unexpected(material.error()));
    }
    auto hmac_key = crypto::derive(*material, "lake transit hmac v1");
    crypto::cleanse(*material);
    if (!hmac_key) {
        co_return Result(std::unexpected(from_store(hmac_key.error())));
    }
    auto digest = crypto::hmac(*hmac_key, *decoded);
    crypto::cleanse(*hmac_key);
    if (!digest) {
        co_return Result(std::unexpected(from_store(digest.error())));
    }
    co_return Result(control::TransitValue{*digest, selected, "hmac-sha256"});
}

SecretTask<control::DerivedEncryptionKey> MountSecrets::derive(std::string parent_name,
                                                               std::string name,
                                                               std::string context) {
    using Result = ZettaControlResult<control::DerivedEncryptionKey>;
    if (!control::valid_handle(name)) {
        co_return failure<control::DerivedEncryptionKey>(
            "invalid_input", control::handle_rule("a derived key's name"), "/name");
    }
    auto parent = control::settled(co_await get_key(parent_name));
    if (!parent) {
        co_return Result(std::unexpected(parent.error()));
    }
    auto decoded_context = crypto::base64url_decode(context);
    if (!decoded_context) {
        co_return Result(std::unexpected(from_store(decoded_context.error())));
    }
    auto material = control::settled(
        co_await read_data_key(*store_, instance_kek_, records_, route_.customer_id, "encryption",
                               parent->id, parent->current_version));
    if (!material) {
        co_return Result(std::unexpected(material.error()));
    }
    auto derived = crypto::derive(*material, "lake transit child v1/" + *decoded_context);
    crypto::cleanse(*material);
    if (!derived) {
        co_return Result(std::unexpected(from_store(derived.error())));
    }
    control::EncryptionKey child{control::new_uuid_v4(), route_.mount_id, name, parent->id, 1,
                                 "", now_text()};
    auto stored = control::settled(co_await write_data_key(
        *store_, instance_kek_, records_, route_.customer_id, "encryption", child.id, 1, *derived));
    crypto::cleanse(*derived);
    if (!stored) {
        co_return Result(std::unexpected(stored.error()));
    }
    auto written = control::settled(co_await records_.put(
        control::EncryptionKeyCodec::collection,
        control::EncryptionKeyCodec::key(records_, route_.mount_id, name),
        control::EncryptionKeyCodec::lookup(route_.mount_id),
        control::EncryptionKeyCodec::encode(child), RecordCondition{.if_absent = true}));
    if (!written && written.error().code == "conflict") {
        co_return failure<control::DerivedEncryptionKey>("conflict",
                                                         "encryption key already exists", "/name");
    }
    if (!written) {
        co_return Result(std::unexpected(written.error()));
    }
    auto decoded = control::EncryptionKeyCodec::decode(*written);
    if (!decoded) {
        co_return failure<control::DerivedEncryptionKey>("store_error",
                                                         "encryption key did not decode");
    }
    co_return Result(control::DerivedEncryptionKey{std::move(decoded->key),
                                                   parent->current_version});
}

namespace {

ZettaControlError lease_not_found(const std::string& lease_id) {
    return ZettaControlError{"not_found", "dynamic-secret lease " + lease_id + " does not exist",
                             "/lease_id"};
}

// A lease record that is this mount's lease `lease_id`, or `not_found`.
ZettaControlResult<control::SecretLeaseRecord> lease_of(const StoredRecord& stored,
                                                        const std::string& mount_id,
                                                        const std::string& lease_id) {
    auto lease = control::SecretLeaseCodec::decode(stored);
    if (!lease || lease->mount_id != mount_id || lease->id != lease_id) {
        return std::unexpected(lease_not_found(lease_id));
    }
    return std::move(*lease);
}

} // namespace

std::expected<MountSecrets, ApiError> MountSecrets::open_lease(const ApiContext& context,
                                                               const std::string& lease_id) {
    if (!context.store) {
        return std::unexpected(ApiError{"store_unavailable",
                                        "no control-plane store is configured", {}});
    }
    if (!uuid_shaped(lease_id)) {
        return std::unexpected(
            ApiError{"not_found", "dynamic-secret lease " + lease_id + " does not exist",
                     "/lease_id"});
    }
    auto route = context.store->get_secret_lease_route(lease_id);
    if (!route) {
        return std::unexpected(ApiError{route.error().code, route.error().message, "/lease_id"});
    }
    auto mount = open(context, route->mount_id);
    if (mount && mount->route().workspace_id != route->workspace_id) {
        return std::unexpected(ApiError{"not_found",
                                        "dynamic-secret lease " + lease_id + " does not exist",
                                        "/lease_id"});
    }
    return mount;
}

SecretTask<control::IssuedDynamicSecret> MountSecrets::lease(std::string role,
                                                             int lifetime_seconds) {
    using Result = ZettaControlResult<control::IssuedDynamicSecret>;
    if (role.empty() || role.size() > 256 || lifetime_seconds < 1 || lifetime_seconds > 3600) {
        co_return failure<control::IssuedDynamicSecret>("invalid_input",
                                                        "dynamic-secret lease is invalid");
    }
    const std::string suffix = control::identity_crypto::random_secret();
    json credential = {{"username", "lake-" + suffix.substr(0, 12)},
                       {"password", control::identity_crypto::random_secret()}};
    control::SecretLeaseRecord lease;
    lease.id = control::new_uuid_v4();
    lease.mount_id = route_.mount_id;
    lease.role = std::move(role);
    {
        auto key = control::settled(co_await read_data_key(*store_, instance_kek_, records_,
                                                           route_.customer_id, "mount",
                                                           route_.mount_id, 1));
        if (!key) {
            co_return Result(std::unexpected(key.error()));
        }
        auto sealed = crypto::seal(*key, credential.dump(),
                                   "lake dynamic secret/" + route_.mount_id + "/" + lease.id);
        crypto::cleanse(*key);
        if (!sealed) {
            co_return Result(std::unexpected(from_store(sealed.error())));
        }
        lease.ciphertext = std::move(*sealed);
    }
    lease.at_us = now_us();
    lease.expires_us = lease.at_us + std::int64_t{lifetime_seconds} * 1'000'000;
    // The route first: a record never exists without the due index entry that expires it. A
    // route whose record was never written is removed by the expiry loop.
    auto routed = store_->record_secret_lease_route(
        {lease.id, route_.mount_id, route_.customer_id, route_.workspace_id, lease.expires_us});
    if (!routed) {
        co_return Result(std::unexpected(from_store(routed.error())));
    }
    auto written = control::settled(co_await records_.put(
        control::SecretLeaseCodec::collection, lease.id,
        control::SecretLeaseCodec::lookup(route_.mount_id),
        control::SecretLeaseCodec::encode(lease), RecordCondition{.if_absent = true}, 1,
        lease.at_us));
    if (!written) {
        static_cast<void>(store_->delete_secret_lease_route(lease.id));
        co_return Result(std::unexpected(written.error()));
    }
    co_return Result(control::IssuedDynamicSecret{control::to_lease(lease, route_.workspace_id),
                                                  std::move(credential)});
}

SecretTask<control::DynamicSecretLease> MountSecrets::get_lease(std::string lease_id) {
    using Result = ZettaControlResult<control::DynamicSecretLease>;
    auto got = control::settled(
        co_await records_.get(control::SecretLeaseCodec::collection, lease_id));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    if (!*got) {
        co_return Result(std::unexpected(lease_not_found(lease_id)));
    }
    auto lease = lease_of(**got, route_.mount_id, lease_id);
    if (!lease) {
        co_return Result(std::unexpected(lease.error()));
    }
    co_return Result(control::to_lease(*lease, route_.workspace_id));
}

SecretTask<control::DynamicSecretLease> MountSecrets::renew_lease(std::string lease_id,
                                                                  int lifetime_seconds) {
    using Result = ZettaControlResult<control::DynamicSecretLease>;
    if (lifetime_seconds < 1 || lifetime_seconds > 3600) {
        co_return failure<control::DynamicSecretLease>(
            "invalid_input", "lease lifetime must be at most one hour", "/lifetime_seconds");
    }
    std::int64_t expires_us = 0;
    auto written = control::settled(co_await rewrite(
        records_, control::SecretLeaseCodec::collection, lease_id,
        control::SecretLeaseCodec::lookup(route_.mount_id),
        [&](const StoredRecord& stored) -> std::expected<std::optional<json>, ZettaControlError> {
            auto lease = lease_of(stored, route_.mount_id, lease_id);
            if (!lease) {
                return std::unexpected(lease.error());
            }
            const std::int64_t now = now_us();
            if (!lease->active(now)) {
                return std::unexpected(
                    ZettaControlError{"conflict", "dynamic-secret lease is not active"});
            }
            // A renewal extends and never shortens: a lease's expiry only moves forward, so
            // every expiry the due index was ever given is no later than the record's.
            lease->expires_us =
                std::max(lease->expires_us, now + std::int64_t{lifetime_seconds} * 1'000'000);
            ++lease->renewals;
            expires_us = lease->expires_us;
            return control::SecretLeaseCodec::encode(*lease);
        }));
    if (!written) {
        co_return Result(std::unexpected(written.error()));
    }
    if (!*written) {
        co_return Result(std::unexpected(lease_not_found(lease_id)));
    }
    // The due index follows the record. If this write is lost or lands after a later renewal's,
    // the index holds an earlier expiry, never a later one: the loop then finds the lease
    // renewed and moves it to the expiry it reads, so no lease outlives its expiry unsettled.
    static_cast<void>(store_->reschedule_secret_lease(lease_id, expires_us));
    auto lease = lease_of(**written, route_.mount_id, lease_id);
    if (!lease) {
        co_return Result(std::unexpected(lease.error()));
    }
    co_return Result(control::to_lease(*lease, route_.workspace_id));
}

SecretTask<control::DynamicSecretLease> MountSecrets::revoke_lease(std::string lease_id) {
    using Result = ZettaControlResult<control::DynamicSecretLease>;
    auto written = control::settled(co_await rewrite(
        records_, control::SecretLeaseCodec::collection, lease_id,
        control::SecretLeaseCodec::lookup(route_.mount_id),
        [&](const StoredRecord& stored) -> std::expected<std::optional<json>, ZettaControlError> {
            auto lease = lease_of(stored, route_.mount_id, lease_id);
            if (!lease) {
                return std::unexpected(lease.error());
            }
            if (!lease->revoked_at.empty() && !lease->ciphertext) {
                return std::nullopt; // idempotent
            }
            if (lease->revoked_at.empty()) {
                lease->revoked_at = now_text();
            }
            lease->ciphertext.reset();
            return control::SecretLeaseCodec::encode(*lease);
        }));
    if (!written) {
        co_return Result(std::unexpected(written.error()));
    }
    if (!*written) {
        co_return Result(std::unexpected(lease_not_found(lease_id)));
    }
    // Settled: the loop has nothing left to do. If this write is lost, the loop finds the lease
    // revoked at its due instant and settles the route then.
    static_cast<void>(store_->reschedule_secret_lease(lease_id, 0));
    auto lease = lease_of(**written, route_.mount_id, lease_id);
    if (!lease) {
        co_return Result(std::unexpected(lease.error()));
    }
    co_return Result(control::to_lease(*lease, route_.workspace_id));
}

namespace {

// A route whose record is missing this long after the route was written belongs to a lease whose
// write never completed (or a copy that stopped between route and record): it is removed.
constexpr std::int64_t abandoned_route_us = 5LL * 60 * 1'000'000;

bool workspace_gone(control::Store& store, const control::SecretLeaseRoute& route) {
    const auto workspace = store.get_workspace(route.workspace_id);
    if (!workspace) {
        return workspace.error().code == "not_found";
    }
    return workspace->desired == "deleted" || workspace->customer_id != route.customer_id;
}

// One claimed lease, settled in its workspace.
liquid::runtime::task<void> settle_lease(control::Store& store, WorkspaceRecords& records,
                                         const control::SecretLeaseRoute& route,
                                         SecretLeaseSweep& sweep) {
    bool expired_now = false;
    auto written = control::settled(co_await rewrite(
        records, control::SecretLeaseCodec::collection, route.lease_id,
        control::SecretLeaseCodec::lookup(route.mount_id),
        [&](const StoredRecord& stored) -> std::expected<std::optional<json>, ZettaControlError> {
            expired_now = false;
            auto lease = lease_of(stored, route.mount_id, route.lease_id);
            if (!lease) {
                return std::unexpected(lease.error());
            }
            const std::int64_t now = now_us();
            if (lease->active(now)) {
                return std::nullopt; // renewed since it was indexed
            }
            if (lease->settled() && !lease->ciphertext) {
                return std::nullopt;
            }
            if (lease->revoked_at.empty() && lease->expired_at.empty()) {
                lease->expired_at = now_text();
                expired_now = true;
            }
            lease->ciphertext.reset();
            return control::SecretLeaseCodec::encode(*lease);
        }));
    if (!written) {
        ++sweep.failed;
        co_return {};
    }
    if (!*written) {
        if (now_us() - route.created_us >= abandoned_route_us) {
            static_cast<void>(store.delete_secret_lease_route(route.lease_id));
            ++sweep.removed;
        } else {
            ++sweep.failed; // its record may still be on its way; looked at again after the hold
        }
        co_return {};
    }
    auto lease = lease_of(**written, route.mount_id, route.lease_id);
    if (!lease) {
        ++sweep.failed;
        co_return {};
    }
    if (lease->settled()) {
        static_cast<void>(store.reschedule_secret_lease(route.lease_id, 0));
        ++(expired_now ? sweep.expired : sweep.settled);
        co_return {};
    }
    static_cast<void>(store.reschedule_secret_lease(route.lease_id, lease->expires_us));
    ++sweep.rescheduled;
    co_return {};
}

} // namespace

liquid::runtime::task<SecretLeaseSweep> expire_secret_leases(const ApiContext& context,
                                                             int limit) {
    SecretLeaseSweep sweep;
    if (!context.store || !context.workspaces || context.instance_kek.empty()) {
        co_return sweep;
    }
    control::Store& store = *context.store;
    auto claimed = store.claim_due_secret_leases(limit, secret_lease_claim_hold_seconds);
    if (!claimed) {
        ++sweep.failed;
        co_return sweep;
    }
    sweep.claimed = claimed->size();
    std::map<std::string, std::vector<const control::SecretLeaseRoute*>> by_workspace;
    for (const control::SecretLeaseRoute& route : *claimed) {
        by_workspace[route.workspace_id].push_back(&route);
    }
    for (const auto& [workspace_id, routes] : by_workspace) {
        if (workspace_gone(store, *routes.front())) {
            // The workspace's Zetta and its records were deleted with it.
            for (const control::SecretLeaseRoute* route : routes) {
                static_cast<void>(store.reschedule_secret_lease(route->lease_id, 0));
                ++sweep.settled;
            }
            continue;
        }
        auto records = workspace_records(context, routes.front()->customer_id, workspace_id);
        if (!records) {
            sweep.failed += routes.size();
            continue;
        }
        for (const control::SecretLeaseRoute* route : routes) {
            static_cast<void>(co_await settle_lease(store, *records, *route, sweep));
        }
    }
    co_return sweep;
}

SecretTask<control::SecretMount> create_secret_mount(const ApiContext& context,
                                                     control::SecretMount mount) {
    using Result = ZettaControlResult<control::SecretMount>;
    if (!control::valid_handle(mount.name)) {
        co_return failure<control::SecretMount>(
            "invalid_input", control::handle_rule("a mount's name"), "/name");
    }
    if (!context.store) {
        co_return failure<control::SecretMount>("store_unavailable",
                                                "no control-plane store is configured");
    }
    control::Store& store = *context.store;
    // The owner must be inside the space before anything is written to the space's workspace, so
    // a caller who names another space's workspace writes nothing there. Without an organization
    // the owner is the customer itself (an account); with one, the organization must sit in the
    // customer's namespace.
    const auto customer = store.find_policy_object("customer", mount.customer_id);
    if (!customer) {
        co_return failure<control::SecretMount>(
            "invalid_input", "customer_id is unknown", "/customer_id");
    }
    if (!mount.organization_id.empty()) {
        const auto organization = store.find_policy_object("organization", mount.organization_id);
        if (!organization || organization->parent_id != customer->id) {
            co_return failure<control::SecretMount>(
                "invalid_input",
                "organization_id is not inside the customer namespace",
                "/organization_id");
        }
    }
    auto opened = workspace_records(context, mount.customer_id, mount.workspace_id);
    if (!opened) {
        co_return Result(std::unexpected(opened.error()));
    }
    WorkspaceRecords& records = *opened;
    const auto collection = control::SecretMountCodec::collection;
    mount.id = control::new_uuid_v4();
    mount.at_us = now_us();
    mount.created_at = control::record_instant(mount.at_us);
    mount.updated_at = mount.created_at;

    // The name, claimed once in the workspace for its owner. A claim whose mount never got its
    // route (a create that stopped part way) is taken over at the revision read.
    const std::string claim_key = control::secret_mount_name_key(records, mount);
    const std::string claim_lookup = control::secret_mount_name_lookup(mount);
    auto claimed = control::settled(co_await records.put(
        collection, claim_key, claim_lookup, control::secret_mount_name_claim(mount.id),
        RecordCondition{.if_absent = true}));
    if (!claimed && claimed.error().code == "conflict") {
        auto existing = control::settled(co_await records.get(collection, claim_key));
        if (!existing) {
            co_return Result(std::unexpected(existing.error()));
        }
        const auto holder =
            *existing ? control::secret_mount_name_claimed(**existing) : std::nullopt;
        if (!*existing || (holder && store.get_secret_mount_route(*holder))) {
            co_return failure<control::SecretMount>("conflict", "secret mount already exists",
                                                    "/name");
        }
        claimed = control::settled(co_await records.put(
            collection, claim_key, claim_lookup, control::secret_mount_name_claim(mount.id),
            RecordCondition{.if_revision = (*existing)->revision}, (*existing)->lake_revision + 1,
            (*existing)->at_us));
        if (!claimed && claimed.error().code == "conflict") {
            co_return failure<control::SecretMount>("conflict", "secret mount already exists",
                                                    "/name");
        }
    }
    if (!claimed) {
        co_return Result(std::unexpected(claimed.error()));
    }
    auto key = crypto::random_key();
    if (!key) {
        co_return Result(std::unexpected(from_store(key.error())));
    }
    auto stored = control::settled(co_await write_data_key(
        store, context.instance_kek, records, mount.customer_id, "mount", mount.id, 1, *key));
    crypto::cleanse(*key);
    if (!stored) {
        co_return Result(std::unexpected(stored.error()));
    }
    auto written = control::settled(co_await records.put(
        collection, mount.id, control::SecretMountCodec::lookup(mount.customer_id),
        control::SecretMountCodec::encode(mount), RecordCondition{.if_absent = true}, 1,
        mount.at_us));
    if (!written) {
        co_return Result(std::unexpected(written.error()));
    }
    // Last, the route: until it exists, nothing names the records above.
    auto route = store.create_secret_mount_route(
        {mount.id, mount.customer_id, mount.organization_id, mount.workspace_id, ""});
    if (!route) {
        co_return Result(std::unexpected(ZettaControlError{
            route.error().code,
            route.error().message,
            route.error().message.find("organization") != std::string::npos ? "/organization_id"
            : route.error().message.find("customer") != std::string::npos   ? "/customer_id"
                                                                            : "/workspace_id"}));
    }
    co_return Result(std::move(mount));
}

SecretTask<std::vector<control::SecretMount>>
list_secret_mounts(const ApiContext& context, std::string customer_id, std::string workspace_id,
                   std::string organization_id, int limit) {
    using Mounts = std::vector<control::SecretMount>;
    using Result = ZettaControlResult<Mounts>;
    if (!context.store) {
        co_return failure<Mounts>("store_unavailable", "no control-plane store is configured");
    }
    auto routes = context.store->list_secret_mount_routes(customer_id, workspace_id);
    if (!routes) {
        co_return Result(std::unexpected(from_store(routes.error())));
    }
    if (routes->empty()) {
        co_return Result(Mounts{});
    }
    std::set<std::string> routed;
    for (const control::SecretMountRoute& route : *routes) {
        routed.insert(route.mount_id);
    }
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return Result(std::unexpected(records.error()));
    }
    auto listed = control::settled(co_await records->list(
        control::SecretMountCodec::collection, control::SecretMountCodec::lookup(customer_id)));
    if (!listed) {
        co_return Result(std::unexpected(listed.error()));
    }
    Mounts mounts;
    for (auto stored = listed->rbegin(); stored != listed->rend(); ++stored) {
        auto mount = control::SecretMountCodec::decode(*stored);
        if (mount && routed.contains(mount->id) && mount->customer_id == customer_id &&
            mount->workspace_id == workspace_id &&
            (organization_id.empty() || mount->organization_id == organization_id)) {
            mounts.push_back(std::move(*mount));
        }
        if (limit > 0 && mounts.size() >= static_cast<std::size_t>(limit)) {
            break;
        }
    }
    co_return Result(std::move(mounts));
}

control::ZettaControlResult<std::string> unwrap_secret_data_key(const ApiContext& context,
                                                                const control::SecretDataKey& key) {
    if (!context.store) {
        return failure<std::string>("store_unavailable", "no control-plane store is configured");
    }
    auto raw = context.store->unwrap_secret_data_key(key.customer_id, key.owner_id,
                                                     key.key_version, key.wrapped,
                                                     context.instance_kek);
    if (!raw) {
        return std::unexpected(from_store(raw.error()));
    }
    return std::move(*raw);
}

SecretTask<bool> rewrap_secret_data_key(const ApiContext& context, WorkspaceRecords& records,
                                        std::string id) {
    using Result = ZettaControlResult<bool>;
    bool wrote = false;
    auto got = control::settled(
        co_await records.get(control::SecretDataKeyCodec::collection, id));
    if (!got) {
        co_return Result(std::unexpected(got.error()));
    }
    const auto first = *got ? control::SecretDataKeyCodec::decode(**got) : std::nullopt;
    if (!first) {
        co_return Result(false);
    }
    auto rewritten = control::settled(co_await rewrite(
        records, control::SecretDataKeyCodec::collection, std::move(id),
        control::SecretDataKeyCodec::lookup(first->customer_id),
        [&](const StoredRecord& stored) -> std::expected<std::optional<json>, ZettaControlError> {
            auto current = control::SecretDataKeyCodec::decode(stored);
            if (!current) {
                return std::unexpected(ZettaControlError{"store_error", "data key did not decode"});
            }
            auto state = context.store->customer_kek_state(current->customer_id);
            if (!state) {
                return std::unexpected(from_store(state.error()));
            }
            if (current->wrapped.kek_version == state->version) {
                return std::nullopt;
            }
            auto raw = unwrap_secret_data_key(context, *current);
            if (!raw) {
                return std::unexpected(raw.error());
            }
            auto wrapped = context.store->wrap_secret_data_key(
                current->customer_id, current->owner_id, current->key_version, *raw,
                context.instance_kek);
            std::ranges::fill(*raw, '\0');
            if (!wrapped) {
                return std::unexpected(from_store(wrapped.error()));
            }
            current->wrapped = *wrapped;
            wrote = true;
            return control::SecretDataKeyCodec::encode(*current);
        }));
    if (!rewritten) {
        co_return Result(std::unexpected(rewritten.error()));
    }
    co_return Result(wrote);
}

namespace {

// Every data key of the customer, in each of its live workspaces that hold a mount, rewrapped by
// the current KEK. Answers how many were rewritten.
SecretTask<std::uint64_t> rewrap_customer_data_keys(const ApiContext& context,
                                                    const std::string& customer_id) {
    using Result = ZettaControlResult<std::uint64_t>;
    auto routes = context.store->list_secret_mount_routes(customer_id, "");
    if (!routes) {
        co_return Result(std::unexpected(from_store(routes.error())));
    }
    std::set<std::string> workspaces;
    for (const control::SecretMountRoute& route : *routes) {
        workspaces.insert(route.workspace_id);
    }
    std::uint64_t rewrapped = 0;
    for (const std::string& workspace_id : workspaces) {
        const auto workspace = context.store->get_workspace(workspace_id);
        if (workspace && workspace->desired == "deleted") {
            continue; // its records went with it
        }
        auto records = workspace_records(context, customer_id, workspace_id);
        if (!records) {
            co_return Result(std::unexpected(records.error()));
        }
        auto listed = control::settled(
            co_await records->list(control::SecretDataKeyCodec::collection,
                                   control::SecretDataKeyCodec::lookup(customer_id)));
        if (!listed) {
            ZettaControlError error = listed.error();
            error.message = "workspace " + workspace_id + ": " + error.message;
            co_return Result(std::unexpected(std::move(error)));
        }
        for (const StoredRecord& stored : *listed) {
            auto wrote = control::settled(co_await rewrap_secret_data_key(context, *records,
                                                                          stored.id));
            if (!wrote) {
                ZettaControlError error = wrote.error();
                error.message = "workspace " + workspace_id + ": " + error.message;
                co_return Result(std::unexpected(std::move(error)));
            }
            rewrapped += *wrote ? 1 : 0;
        }
    }
    co_return Result(rewrapped);
}

} // namespace

SecretTask<control::CustomerKekRotation> rotate_customer_kek(const ApiContext& context,
                                                             std::string customer_id) {
    using Result = ZettaControlResult<control::CustomerKekRotation>;
    if (!context.store) {
        co_return failure<control::CustomerKekRotation>("store_unavailable",
                                                        "no control-plane store is configured");
    }
    control::Store& store = *context.store;
    auto state = store.customer_kek_state(customer_id);
    if (!state) {
        co_return Result(std::unexpected(from_store(state.error())));
    }
    std::uint64_t finished = 0;
    if (state->previous_version != 0) {
        // An earlier rotation stopped before every workspace was rewrapped: finish it first.
        auto rewrapped = control::settled(co_await rewrap_customer_data_keys(context, customer_id));
        if (!rewrapped) {
            co_return Result(std::unexpected(rewrapped.error()));
        }
        finished = *rewrapped;
        if (auto retired = store.retire_customer_kek(customer_id, state->previous_version);
            !retired) {
            co_return Result(std::unexpected(from_store(retired.error())));
        }
    }
    auto rotated = store.rotate_customer_kek(customer_id, context.instance_kek);
    if (!rotated) {
        co_return Result(std::unexpected(from_store(rotated.error())));
    }
    auto rewrapped = control::settled(co_await rewrap_customer_data_keys(context, customer_id));
    if (!rewrapped) {
        ZettaControlError error = rewrapped.error();
        error.message = "customer KEK version " + std::to_string(rotated->version) +
                        " is current, but some data keys still use version " +
                        std::to_string(rotated->previous_version) + " (" + error.message +
                        "); rotate again once the workspace is ready to finish";
        co_return Result(std::unexpected(std::move(error)));
    }
    if (auto retired = store.retire_customer_kek(customer_id, rotated->previous_version);
        !retired) {
        co_return Result(std::unexpected(from_store(retired.error())));
    }
    rotated->data_keys_rewrapped += finished + *rewrapped;
    rotated->previous_version = 0;
    co_return Result(std::move(*rotated));
}

} // namespace lake::api
