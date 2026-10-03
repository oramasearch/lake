#include "lake/control/workspace_records.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <tuple>

#include <openssl/evp.h>

#include "lake/control/store.hpp"
#include "secrets_crypto.hpp"

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> records_failure(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

// The labels both keys are derived under. `kv` in every envelope names the key version, so a later
// key can be introduced beside this one without rewriting the records it sealed.
constexpr int key_version = 1;
constexpr int envelope_version = 1;
constexpr std::string_view seal_label = "lake/workspace-records/v1/seal/";
constexpr std::string_view index_label = "lake/workspace-records/v1/index/";
constexpr std::string_view lookup_index = "lookup";
constexpr int busy_attempts = 4;

bool uuid_text(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? value[index] != '-'
                 : std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    return true;
}

bool collection_name(std::string_view name) {
    return !name.empty() && name.size() <= 48 &&
           std::isalpha(static_cast<unsigned char>(name[0])) &&
           std::ranges::all_of(name, [](unsigned char c) {
               return std::islower(c) != 0 || std::isdigit(c) != 0 || c == '_';
           });
}

// Record keys are lake-made ids (UUIDs); anything that could carry a secret is refused here and
// goes in the sealed body instead.
bool key_safe(std::string_view key) {
    return !key.empty() && key.size() <= 128 && std::ranges::all_of(key, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == ':';
    });
}

std::int64_t now_us() {
    // Strictly increasing within the process, so two messages a request appends keep their order.
    static std::atomic<std::int64_t> last{0};
    const std::int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    std::int64_t previous = last.load(std::memory_order_relaxed);
    std::int64_t next = std::max(now, previous + 1);
    while (!last.compare_exchange_weak(previous, next, std::memory_order_relaxed)) {
        next = std::max(now, previous + 1);
    }
    return next;
}

// The order within the lookup index: the instant, zero-padded so the engine's string order is
// time order.
std::string order_of(std::int64_t at_us) {
    return std::format("{:020}", std::max<std::int64_t>(at_us, 0));
}

std::string base64(std::string_view bytes) {
    std::string out(4 * ((bytes.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                     reinterpret_cast<const unsigned char*>(bytes.data()),
                                     static_cast<int>(bytes.size()));
    out.resize(size < 0 ? 0 : static_cast<std::size_t>(size));
    return out;
}

std::optional<std::string> unbase64(std::string_view text) {
    if (text.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out(3 * (text.size() / 4), '\0');
    const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                                     reinterpret_cast<const unsigned char*>(text.data()),
                                     static_cast<int>(text.size()));
    if (size < 0) {
        return std::nullopt;
    }
    std::size_t length = static_cast<std::size_t>(size);
    for (std::size_t pad = 0; pad < 2 && pad < text.size() && text[text.size() - 1 - pad] == '=';
         ++pad) {
        --length;
    }
    out.resize(length);
    return out;
}

bool missing_collection(const ZettaControlError& error) {
    return error.code == "not_found" && error.path == "/collection";
}

std::int64_t integer(const nlohmann::json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_number()) {
        return -1;
    }
    return found->is_number_integer() ? found->get<std::int64_t>()
                                      : static_cast<std::int64_t>(found->get<double>());
}

} // namespace

struct WorkspaceRecords::Keys {
    secrets_crypto::Key seal{};
    secrets_crypto::Key index{};
    ~Keys() {
        secrets_crypto::cleanse(seal);
        secrets_crypto::cleanse(index);
    }
};

std::string record_instant(std::int64_t at_us) {
    const auto at =
        std::chrono::sys_time<std::chrono::seconds>(std::chrono::seconds(at_us / 1'000'000));
    return std::format("{:%FT%TZ}", at);
}

std::string WorkspaceRecords::zetta_collection(RecordCollection collection) {
    return std::string(workspace_records_prefix) + std::string(collection.name);
}

WorkspaceRecords::WorkspaceRecords(Call call, std::string workspace_id,
                                   std::shared_ptr<const Keys> keys)
    : call_(std::move(call)), workspace_id_(std::move(workspace_id)), keys_(std::move(keys)) {}

ZettaControlResult<WorkspaceRecords>
WorkspaceRecords::open(Call call, std::string workspace_id, std::string_view instance_kek) {
    if (!call) {
        return records_failure<WorkspaceRecords>("workspace_not_ready",
                                                 "a workspace Zetta is required");
    }
    if (!uuid_text(workspace_id)) {
        return records_failure<WorkspaceRecords>("invalid_input",
                                                 "workspace_id must name a workspace Zetta");
    }
    std::ranges::transform(workspace_id, workspace_id.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto root = secrets_crypto::instance_key(instance_kek);
    if (!root) {
        return records_failure<WorkspaceRecords>(
            "store_unavailable",
            "workspace records need LAKE_INSTANCE_KEK: " + root.error().message);
    }
    auto keys = std::make_shared<Keys>();
    auto seal = secrets_crypto::derive_hkdf(*root, std::string(seal_label) + workspace_id);
    auto index = secrets_crypto::derive_hkdf(*root, std::string(index_label) + workspace_id);
    secrets_crypto::cleanse(*root);
    if (!seal || !index) {
        return records_failure<WorkspaceRecords>("store_error",
                                                 "could not derive the workspace record keys");
    }
    keys->seal = *seal;
    keys->index = *index;
    secrets_crypto::cleanse(*seal);
    secrets_crypto::cleanse(*index);
    return WorkspaceRecords(std::move(call), std::move(workspace_id), std::move(keys));
}

std::string WorkspaceRecords::index_of(RecordCollection collection,
                                       std::string_view lookup) const {
    std::string message(collection.name);
    message += '\0';
    message += lookup;
    auto digest = secrets_crypto::hmac(keys_->index, message);
    // HMAC-SHA-256 as base64url: 43 characters.
    return digest ? *digest : std::string();
}

std::string WorkspaceRecords::associated(RecordCollection collection, std::string_view id,
                                         std::string_view key, std::int64_t at_us,
                                         int schema_version, std::int64_t lake_revision) const {
    // Everything the record says about itself besides its body: a record moved to another
    // workspace, collection, key, lookup, instant, schema version or revision does not open.
    return std::format("lake-records/v{}|{}|{}|{}|{}|{}|{}|{}", key_version, workspace_id_,
                       collection.name, id, key, at_us, schema_version, lake_revision);
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>>
WorkspaceRecords::invoke(std::string capability, bool read, nlohmann::json input) {
    ZettaControlResult<nlohmann::json> answered =
        records_failure<nlohmann::json>("busy", "not attempted");
    for (int attempt = 0; attempt < busy_attempts; ++attempt) {
        if (attempt > 0) {
            co_await liquid::runtime::delay(std::chrono::milliseconds(25 << attempt));
        }
        answered = settled(co_await call_(capability, read, input));
        if (answered || answered.error().code != "busy") {
            break;
        }
    }
    co_return answered;
}

liquid::runtime::task<ZettaControlResult<void>>
WorkspaceRecords::create_collection(RecordCollection collection) {
    nlohmann::json input{{"collection", zetta_collection(collection)},
                         {"indexes", nlohmann::json::array({lookup_index})}};
    if (collection.retention_ms > 0) {
        input["retention_ms"] = collection.retention_ms;
    }
    auto created = settled(co_await invoke("create_collection", false, std::move(input)));
    if (!created) {
        co_return ZettaControlResult<void>(std::unexpected(created.error()));
    }
    co_return ZettaControlResult<void>{};
}

liquid::runtime::task<ZettaControlResult<StoredRecord>>
WorkspaceRecords::write(RecordCollection collection, StoredRecord record, std::string lookup,
                        RecordCondition condition) {
    if (!collection_name(collection.name)) {
        co_return records_failure<StoredRecord>("store_error", "the collection name is invalid");
    }
    if (!key_safe(record.id)) {
        co_return records_failure<StoredRecord>("invalid_input", "a record id must be a lake id");
    }
    const std::string key = index_of(collection, lookup);
    if (key.empty()) {
        co_return records_failure<StoredRecord>("store_error", "could not index the record");
    }
    auto sealed = secrets_crypto::seal(
        keys_->seal, record.body.dump(),
        associated(collection, record.id, key, record.at_us, record.schema_version,
                   record.lake_revision));
    if (!sealed) {
        co_return records_failure<StoredRecord>("store_error", sealed.error().message);
    }
    const nlohmann::json envelope{{"v", envelope_version}, {"kv", key_version},
                                  {"sv", record.schema_version}, {"rev", record.lake_revision},
                                  {"at", record.at_us}, {"s", *sealed}};
    nlohmann::json input{{"collection", zetta_collection(collection)},
                         {"key", record.id},
                         {"value", base64(envelope.dump())},
                         {"index", {{lookup_index, key}}},
                         {"order", order_of(record.at_us)}};
    if (condition.if_absent) {
        input["if_absent"] = true;
    } else if (condition.if_revision) {
        input["if_revision"] = *condition.if_revision;
    }
    auto put = settled(co_await invoke("put_record", false, input));
    if (!put && missing_collection(put.error())) {
        if (auto created = settled(co_await create_collection(collection)); !created) {
            co_return ZettaControlResult<StoredRecord>(std::unexpected(created.error()));
        }
        put = settled(co_await invoke("put_record", false, std::move(input)));
    }
    if (!put) {
        co_return ZettaControlResult<StoredRecord>(std::unexpected(put.error()));
    }
    record.revision = integer(*put, "revision");
    co_return ZettaControlResult<StoredRecord>(std::move(record));
}

liquid::runtime::task<ZettaControlResult<StoredRecord>>
WorkspaceRecords::append(RecordCollection collection, std::string lookup, nlohmann::json body,
                         RecordWrite write) {
    StoredRecord record;
    record.id = write.id.empty() ? new_uuid_v4() : write.id;
    record.at_us = write.at_us > 0 ? write.at_us : now_us();
    record.schema_version = collection.schema_version;
    record.body = std::move(body);
    co_return settled(co_await this->write(collection, std::move(record), std::move(lookup),
                                           RecordCondition{.if_absent = true}));
}

liquid::runtime::task<ZettaControlResult<StoredRecord>>
WorkspaceRecords::put(RecordCollection collection, std::string id, std::string lookup,
                      nlohmann::json body, RecordCondition condition, std::int64_t lake_revision,
                      std::int64_t at_us) {
    StoredRecord record;
    record.id = std::move(id);
    record.at_us = at_us > 0 ? at_us : now_us();
    record.schema_version = collection.schema_version;
    record.lake_revision = std::max<std::int64_t>(lake_revision, 1);
    record.body = std::move(body);
    co_return settled(
        co_await write(collection, std::move(record), std::move(lookup), condition));
}

std::optional<StoredRecord> WorkspaceRecords::opened(RecordCollection collection,
                                                     const nlohmann::json& row) const {
    if (!row.is_object()) {
        return std::nullopt;
    }
    const std::string id = row.value("key", std::string());
    const nlohmann::json index = row.value("index", nlohmann::json::object());
    const std::string key =
        index.is_object() ? index.value(std::string(lookup_index), std::string()) : std::string();
    const auto bytes = unbase64(row.value("value", std::string()));
    if (id.empty() || key.empty() || !bytes) {
        return std::nullopt;
    }
    const nlohmann::json envelope = nlohmann::json::parse(*bytes, nullptr, false);
    if (!envelope.is_object() || integer(envelope, "v") != envelope_version ||
        integer(envelope, "kv") != key_version) {
        return std::nullopt;
    }
    StoredRecord record;
    record.id = id;
    record.at_us = integer(envelope, "at");
    record.schema_version = static_cast<int>(integer(envelope, "sv"));
    record.lake_revision = integer(envelope, "rev");
    record.revision = integer(row, "revision");
    auto plain = secrets_crypto::open(
        keys_->seal, envelope.value("s", std::string()),
        associated(collection, id, key, record.at_us, record.schema_version, record.lake_revision));
    if (!plain) {
        return std::nullopt; // not sealed for this workspace, collection, key and lookup
    }
    record.body = nlohmann::json::parse(*plain, nullptr, false);
    if (record.body.is_discarded()) {
        return std::nullopt;
    }
    return record;
}

liquid::runtime::task<ZettaControlResult<std::vector<StoredRecord>>>
WorkspaceRecords::list(RecordCollection collection, std::string lookup, std::size_t newest,
                       std::int64_t since_us, Keep keep) {
    using Records = std::vector<StoredRecord>;
    if (!collection_name(collection.name)) {
        co_return records_failure<Records>("store_error", "the collection name is invalid");
    }
    const std::string key = index_of(collection, lookup);
    if (key.empty()) {
        co_return records_failure<Records>("store_error", "could not index the lookup");
    }
    Records opened_records;
    std::string after;
    // Newest first, a page at a time, until `newest` records opened or the listing ends.
    for (;;) {
        const std::size_t want =
            newest == 0 || keep ? 1000
                                : std::min<std::size_t>(1000, newest - opened_records.size());
        nlohmann::json input{{"collection", zetta_collection(collection)},
                             {"index", lookup_index},
                             {"value", key},
                             {"direction", "desc"},
                             {"limit", std::max<std::size_t>(want, 1)}};
        if (!after.empty()) {
            input["after"] = after;
        }
        auto page = settled(co_await invoke("list_records", true, std::move(input)));
        if (!page) {
            if (missing_collection(page.error())) {
                break;
            }
            co_return ZettaControlResult<Records>(std::unexpected(page.error()));
        }
        bool older = false;
        for (const nlohmann::json& row : page->value("records", nlohmann::json::array())) {
            // The index is in time order, newest first: the first record older than `since_us`
            // ends the walk, whether or not it opens here.
            const std::string order =
                row.is_object() ? row.value("order", std::string()) : std::string();
            if (since_us > 0 && !order.empty() && order < order_of(since_us)) {
                older = true;
                break;
            }
            if (auto record = opened(collection, row)) {
                if (since_us > 0 && record->at_us < since_us) {
                    older = true;
                    break;
                }
                if (keep && !keep(*record)) {
                    continue;
                }
                opened_records.push_back(std::move(*record));
            }
        }
        after = page->value("next_after", std::string());
        if (older || after.empty() || (newest > 0 && opened_records.size() >= newest)) {
            break;
        }
    }
    std::ranges::sort(opened_records, [](const StoredRecord& left, const StoredRecord& right) {
        return std::tie(left.at_us, left.id) < std::tie(right.at_us, right.id);
    });
    if (newest > 0 && opened_records.size() > newest) {
        opened_records.erase(opened_records.begin(),
                             opened_records.end() - static_cast<std::ptrdiff_t>(newest));
    }
    co_return ZettaControlResult<Records>(std::move(opened_records));
}

liquid::runtime::task<ZettaControlResult<std::optional<StoredRecord>>>
WorkspaceRecords::get(RecordCollection collection, std::string id) {
    using Found = std::optional<StoredRecord>;
    if (!collection_name(collection.name) || !key_safe(id)) {
        co_return records_failure<Found>("invalid_input", "the collection or id is invalid");
    }
    auto got = settled(co_await invoke(
        "get_record", true, {{"collection", zetta_collection(collection)}, {"key", id}}));
    if (!got) {
        if (got.error().code == "not_found") {
            co_return ZettaControlResult<Found>(Found{});
        }
        co_return ZettaControlResult<Found>(std::unexpected(got.error()));
    }
    co_return ZettaControlResult<Found>(opened(collection, got->value("record", nlohmann::json())));
}

liquid::runtime::task<ZettaControlResult<void>>
WorkspaceRecords::remove(RecordCollection collection, std::string id,
                         std::optional<std::int64_t> if_revision) {
    if (!collection_name(collection.name) || !key_safe(id)) {
        co_return records_failure<void>("invalid_input", "the collection or id is invalid");
    }
    nlohmann::json input{{"collection", zetta_collection(collection)}, {"key", id}};
    if (if_revision) {
        input["if_revision"] = *if_revision;
    }
    auto removed = settled(co_await invoke("delete_record", false, std::move(input)));
    if (!removed) {
        if (missing_collection(removed.error())) {
            co_return records_failure<void>("not_found", "no record " + id);
        }
        co_return ZettaControlResult<void>(std::unexpected(removed.error()));
    }
    co_return ZettaControlResult<void>{};
}

liquid::runtime::task<ZettaControlResult<std::optional<StoredRecord>>>
WorkspaceRecords::update(RecordCollection collection, std::string id, std::string lookup,
                         Change change, int attempts) {
    using Found = std::optional<StoredRecord>;
    ZettaControlError last{"conflict", "the record kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto current = settled(co_await get(collection, id));
        if (!current) {
            co_return ZettaControlResult<Found>(std::unexpected(current.error()));
        }
        std::optional<nlohmann::json> next = change(*current);
        if (!next) {
            if (!*current) {
                co_return ZettaControlResult<Found>(Found{});
            }
            auto removed = settled(co_await remove(collection, id, (*current)->revision));
            if (removed || removed.error().code == "not_found") {
                co_return ZettaControlResult<Found>(Found{});
            }
            if (removed.error().code != "conflict") {
                co_return ZettaControlResult<Found>(std::unexpected(removed.error()));
            }
            last = removed.error();
            continue;
        }
        RecordCondition condition;
        std::int64_t lake_revision = 1;
        std::int64_t at_us = 0;
        if (*current) {
            condition.if_revision = (*current)->revision;
            lake_revision = (*current)->lake_revision + 1;
            at_us = (*current)->at_us;
        } else {
            condition.if_absent = true;
        }
        auto written = settled(co_await put(collection, id, lookup, std::move(*next), condition,
                                            lake_revision, at_us));
        if (written) {
            co_return ZettaControlResult<Found>(Found{std::move(*written)});
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<Found>(std::unexpected(written.error()));
        }
        // Someone else wrote first (details.revision is the live revision): read it and retry.
        last = written.error();
    }
    co_return ZettaControlResult<Found>(std::unexpected(last));
}

} // namespace lake::control
