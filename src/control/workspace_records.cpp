#include "lake/control/workspace_records.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <map>
#include <tuple>

#include "lake/control/store.hpp"
#include "secrets_crypto.hpp"

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> records_failure(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

// The labels both keys are derived under. `v1` is the key version every row records (`kv`), so a
// later key can be introduced beside it without rewriting the rows this one sealed.
constexpr int key_version = 1;
constexpr std::string_view seal_label = "lake/workspace-records/v1/seal/";
constexpr std::string_view index_label = "lake/workspace-records/v1/index/";

bool uuid_text(std::string_view value) {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? value[index] != '-' : std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
            return false;
        }
    }
    return true;
}

bool collection_name(std::string_view name) {
    return !name.empty() && name.size() <= 48 && std::isalpha(static_cast<unsigned char>(name[0])) &&
           std::ranges::all_of(name, [](unsigned char c) {
               return std::islower(c) != 0 || std::isdigit(c) != 0 || c == '_';
           });
}

// Only lake-made values reach a statement: UUIDs, lowercase hex, base64url and integers. None of
// them contains a quote, so the literal needs no escaping; anything else is refused.
bool literal_safe(std::string_view value) {
    return std::ranges::all_of(value, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_';
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

bool missing(const ZettaControlError& error) {
    return error.message.find("does not exist") != std::string::npos;
}

bool already_exists(const ZettaControlError& error) {
    return error.message.find("already exists") != std::string::npos;
}

std::int64_t integer(const nlohmann::json& row, const char* key) {
    const auto found = row.find(key);
    if (found == row.end()) {
        return -1;
    }
    if (found->is_number_integer()) {
        return found->get<std::int64_t>();
    }
    if (found->is_number()) {
        return static_cast<std::int64_t>(found->get<double>());
    }
    return -1;
}

std::string text(const nlohmann::json& row, const char* key) {
    const auto found = row.find(key);
    return found != row.end() && found->is_string() ? found->get<std::string>() : std::string();
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

bool names_workspace_records(std::string_view text) {
    const std::string_view name = workspace_records_database;
    if (text.size() < name.size()) {
        return false;
    }
    for (std::size_t start = 0; start + name.size() <= text.size(); ++start) {
        bool equal = true;
        for (std::size_t index = 0; index < name.size() && equal; ++index) {
            equal = std::tolower(static_cast<unsigned char>(text[start + index])) == name[index];
        }
        if (equal) {
            return true;
        }
    }
    return false;
}

std::string record_instant(std::int64_t at_us) {
    const auto at = std::chrono::sys_time<std::chrono::seconds>(
        std::chrono::seconds(at_us / 1'000'000));
    return std::format("{:%FT%TZ}", at);
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
            "store_unavailable", "workspace records need LAKE_INSTANCE_KEK: " + root.error().message);
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

std::string WorkspaceRecords::index_of(RecordCollection collection, std::string_view lookup) const {
    std::string message(collection.name);
    message += '\0';
    message += lookup;
    auto digest = secrets_crypto::hmac(keys_->index, message);
    if (!digest) {
        return {};
    }
    // HMAC-SHA-256 as base64url is 43 characters of the literal-safe alphabet.
    return *digest;
}

liquid::runtime::task<ZettaControlResult<nlohmann::json>>
WorkspaceRecords::sql(std::string statement) {
    // Zetta classifies `sql` as a read even though INSERT and DDL change the database, so the call
    // is a GET with the statement in the query string (docs/architecture/workspace-records.md).
    co_return settled(co_await call_("sql", true, {{"statement", std::move(statement)}}));
}

liquid::runtime::task<ZettaControlResult<void>>
WorkspaceRecords::create_table(RecordCollection collection) {
    auto database = settled(co_await sql("CREATE DATABASE " + std::string(workspace_records_database)));
    if (!database && !already_exists(database.error())) {
        co_return ZettaControlResult<void>(std::unexpected(database.error()));
    }
    auto table = settled(co_await sql("CREATE TABLE " + std::string(workspace_records_database) + "." +
                              std::string(collection.name) +
                              " (rid text, rev int, part int, parts int, at bigint, sv int, kv int, "
                              "k text, body text)"));
    if (!table && !already_exists(table.error())) {
        co_return ZettaControlResult<void>(std::unexpected(table.error()));
    }
    co_return ZettaControlResult<void>{};
}

liquid::runtime::task<ZettaControlResult<StoredRecord>>
WorkspaceRecords::append(RecordCollection collection, std::string lookup, nlohmann::json body,
                         RecordWrite write) {
    if (!collection_name(collection.name)) {
        co_return records_failure<StoredRecord>("store_error", "the collection name is invalid");
    }
    StoredRecord record;
    record.id = write.id.empty() ? new_uuid_v4() : write.id;
    record.at_us = write.at_us > 0 ? write.at_us : now_us();
    record.schema_version = collection.schema_version;
    record.body = std::move(body);
    if (!uuid_text(record.id)) {
        co_return records_failure<StoredRecord>("invalid_input", "a record id must be a UUID");
    }
    const std::string key = index_of(collection, lookup);
    if (key.empty()) {
        co_return records_failure<StoredRecord>("store_error", "could not index the record");
    }
    // The associated data binds everything a row says about the record besides its part layout:
    // a row moved to another workspace, collection, record, lookup, instant or schema version does
    // not open.
    const std::string associated =
        std::format("lake-records/v{}|{}|{}|{}|1|{}|{}|{}", key_version, workspace_id_,
                    collection.name, record.id, key, record.at_us, record.schema_version);
    auto sealed = secrets_crypto::seal(keys_->seal, record.body.dump(), associated);
    if (!sealed) {
        co_return records_failure<StoredRecord>("store_error", sealed.error().message);
    }
    if (!literal_safe(*sealed) || !literal_safe(key)) {
        co_return records_failure<StoredRecord>("store_error", "a sealed record is not literal-safe");
    }
    const std::size_t parts =
        std::max<std::size_t>(1, (sealed->size() + part_characters - 1) / part_characters);
    const std::string table =
        std::string(workspace_records_database) + "." + std::string(collection.name);
    // Every part names the count, so a reader assembles the record only once all are present: the
    // record is not visible while a write is in progress or after one failed partway.
    for (std::size_t part = 0; part < parts; ++part) {
        const std::string statement = std::format(
            "INSERT INTO {} (rid, rev, part, parts, at, sv, kv, k, body) VALUES ('{}', 1, {}, {}, "
            "{}, {}, {}, '{}', '{}')",
            table, record.id, part, parts, record.at_us, collection.schema_version, key_version,
            key, sealed->substr(part * part_characters, part_characters));
        auto inserted = settled(co_await sql(statement));
        if (!inserted && missing(inserted.error())) {
            if (auto created = settled(co_await create_table(collection)); !created) {
                co_return ZettaControlResult<StoredRecord>(std::unexpected(created.error()));
            }
            inserted = settled(co_await sql(statement));
        }
        if (!inserted) {
            co_return ZettaControlResult<StoredRecord>(std::unexpected(inserted.error()));
        }
    }
    co_return ZettaControlResult<StoredRecord>(std::move(record));
}

liquid::runtime::task<ZettaControlResult<std::vector<StoredRecord>>>
WorkspaceRecords::list(RecordCollection collection, std::string lookup, std::size_t newest) {
    if (!collection_name(collection.name)) {
        co_return records_failure<std::vector<StoredRecord>>("store_error",
                                                             "the collection name is invalid");
    }
    const std::string key = index_of(collection, lookup);
    if (key.empty() || !literal_safe(key)) {
        co_return records_failure<std::vector<StoredRecord>>("store_error",
                                                             "could not index the lookup");
    }
    auto selected = settled(co_await sql(std::format("SELECT * FROM {}.{} WHERE k = '{}'",
                                             workspace_records_database, collection.name, key)));
    if (!selected) {
        if (missing(selected.error())) {
            co_return ZettaControlResult<std::vector<StoredRecord>>(std::vector<StoredRecord>{});
        }
        co_return ZettaControlResult<std::vector<StoredRecord>>(std::unexpected(selected.error()));
    }
    // Rows by record and part. A part written twice (a rerun copy, a retried insert) is the same
    // ciphertext, so the first one is kept.
    struct Parts {
        std::int64_t at = 0, sv = 1, kv = 0, count = 0;
        std::map<std::int64_t, std::string> bodies;
    };
    std::map<std::string, Parts> records;
    for (const nlohmann::json& row : selected->value("rows", nlohmann::json::array())) {
        if (!row.is_object() || text(row, "k") != key) {
            continue;
        }
        const std::string id = text(row, "rid");
        const std::int64_t part = integer(row, "part");
        const std::int64_t count = integer(row, "parts");
        if (!uuid_text(id) || integer(row, "rev") != 1 || part < 0 || count < 1 || part >= count) {
            continue;
        }
        Parts& parts = records[id];
        if (parts.count == 0) {
            parts.count = count;
            parts.at = integer(row, "at");
            parts.sv = integer(row, "sv");
            parts.kv = integer(row, "kv");
        }
        if (parts.count == count) {
            parts.bodies.try_emplace(part, text(row, "body"));
        }
    }
    std::vector<StoredRecord> opened;
    for (auto& [id, parts] : records) {
        if (parts.kv != key_version || static_cast<std::int64_t>(parts.bodies.size()) != parts.count) {
            continue;
        }
        std::string sealed;
        for (const auto& [part, body] : parts.bodies) {
            sealed += body;
        }
        const std::string associated =
            std::format("lake-records/v{}|{}|{}|{}|1|{}|{}|{}", key_version, workspace_id_,
                        collection.name, id, key, parts.at, parts.sv);
        auto plain = secrets_crypto::open(keys_->seal, sealed, associated);
        if (!plain) {
            continue; // not sealed for this workspace, collection, record and lookup
        }
        nlohmann::json body = nlohmann::json::parse(*plain, nullptr, false);
        if (body.is_discarded()) {
            continue;
        }
        opened.push_back({id, parts.at, static_cast<int>(parts.sv), std::move(body)});
    }
    std::ranges::sort(opened, [](const StoredRecord& left, const StoredRecord& right) {
        return std::tie(left.at_us, left.id) < std::tie(right.at_us, right.id);
    });
    if (newest > 0 && opened.size() > newest) {
        opened.erase(opened.begin(), opened.end() - static_cast<std::ptrdiff_t>(newest));
    }
    co_return ZettaControlResult<std::vector<StoredRecord>>(std::move(opened));
}

} // namespace lake::control
