#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/control/zetta_control.hpp"

namespace lake::test {

// The keyed record store of one workspace Zetta (zetta#1231, zetta docs/data/record-store.md) as
// lake's workspace records use it: create_collection, put_record (if_absent / if_revision, 409
// `conflict` with details.revision), get_record, delete_record and list_records over one index
// value ordered by `order` then key, a page at a time. Unknown collections are `not_found` at
// /collection. `busy_next` answers that many calls with 503 `busy` first. Tests that route several
// workspaces keep one instance per workspace endpoint.
class RecordTables {
public:
    using json = nlohmann::json;
    struct Collection {
        json indexes = json::array();
        std::int64_t retention_ms = 0;
        std::map<std::string, json> records; // key -> stored record (with value)
    };
    std::map<std::string, Collection> collections;
    std::int64_t clock = 1'000'000;
    int busy_next = 0;
    std::vector<std::string> calls;
    // Run once, just before the next put_record of that key is decided: another writer's change
    // landing between a caller's read and its conditional write. It edits `collections` directly.
    std::map<std::string, std::function<void(RecordTables&)>> before_put;
    // The same, just before the next delete_record of that key is decided: another caller's
    // conditional delete (a racing redemption) landing first.
    std::map<std::string, std::function<void(RecordTables&)>> before_delete;

    static bool handles(std::string_view capability, const json&) {
        return capability == "create_collection" || capability == "put_record" ||
               capability == "get_record" || capability == "delete_record" ||
               capability == "list_records";
    }

    control::ZettaControlResult<json> run(std::string_view capability, const json& input) {
        calls.emplace_back(capability);
        if (busy_next > 0) {
            --busy_next;
            return fail("busy", "ownership of this key is moving; retry", "");
        }
        const std::string name = input.value("collection", std::string());
        if (capability == "create_collection") {
            json indexes = input.value("indexes", json::array());
            const auto existing = collections.find(name);
            if (existing != collections.end()) {
                if (existing->second.indexes != indexes) {
                    return fail("conflict", "a different collection exists", "/collection");
                }
                return json{{"collection", {{"name", name}}}, {"created", false}};
            }
            collections[name] = Collection{indexes, input.value("retention_ms", std::int64_t{0}), {}};
            return json{{"collection", {{"name", name}, {"indexes", indexes}}}, {"created", true}};
        }
        const auto found = collections.find(name);
        if (found == collections.end()) {
            return fail("not_found", "no record collection '" + name + "'", "/collection");
        }
        Collection& collection = found->second;
        const std::string key = input.value("key", std::string());
        if (capability == "put_record" || capability == "delete_record") {
            if (const auto hook = before_put.find(key);
                capability == "put_record" && hook != before_put.end()) {
                auto change = std::move(hook->second);
                before_put.erase(hook);
                change(*this);
            }
            if (const auto hook = before_delete.find(key);
                capability == "delete_record" && hook != before_delete.end()) {
                auto change = std::move(hook->second);
                before_delete.erase(hook);
                change(*this);
            }
            const auto existing = collection.records.find(key);
            const bool live = existing != collection.records.end();
            if (input.value("if_absent", false) && live) {
                return conflict(existing->second, "/if_absent");
            }
            if (input.contains("if_revision") &&
                (!live || existing->second.at("revision") != input.at("if_revision"))) {
                return live ? conflict(existing->second, "/if_revision")
                            : fail("conflict", "record '" + key + "' does not exist", "/if_revision");
            }
            const std::int64_t revision = ++clock;
            if (capability == "delete_record") {
                if (!live) {
                    return fail("not_found", "no record '" + key + "'", "/key");
                }
                collection.records.erase(existing);
                return written(name, key, revision);
            }
            const std::string value = input.value("value", std::string());
            if (value.empty() || value.size() % 4 != 0) {
                return fail("invalid_input", "want base64", "/value");
            }
            collection.records[key] = {{"collection", name},
                                       {"key", key},
                                       {"value", value},
                                       {"revision", revision},
                                       {"index", input.value("index", json::object())},
                                       {"order", input.value("order", std::string())},
                                       {"updated_ms", revision / 1000}};
            return written(name, key, revision);
        }
        if (capability == "get_record") {
            const auto existing = collection.records.find(key);
            if (existing == collection.records.end()) {
                return fail("not_found", "no record '" + key + "'", "/key");
            }
            return json{{"record", existing->second}};
        }
        // list_records
        const std::string index = input.value("index", std::string());
        const std::string wanted = input.value("value", std::string());
        std::vector<const json*> rows;
        for (const auto& [record_key, record] : collection.records) {
            if (index.empty() || record.at("index").value(index, std::string()) == wanted) {
                rows.push_back(&record);
            }
        }
        const auto position = [&](const json* record) {
            return record->at("order").get<std::string>() + "\x1f" + record->at("key").get<std::string>();
        };
        std::ranges::sort(rows, [&](const json* left, const json* right) {
            return position(left) < position(right);
        });
        const bool descending = input.value("direction", std::string("asc")) == "desc";
        if (descending) {
            std::ranges::reverse(rows);
        }
        const std::string after = input.value("after", std::string());
        const auto limit = static_cast<std::size_t>(input.value("limit", 100));
        json records = json::array();
        std::string last;
        bool more = false;
        for (const json* record : rows) {
            const std::string here = position(record);
            if (!after.empty() && (descending ? here >= after : here <= after)) {
                continue;
            }
            if (records.size() == limit) {
                more = true;
                break;
            }
            records.push_back(*record);
            last = here;
        }
        json page{{"records", std::move(records)}};
        if (more) {
            page["next_after"] = last;
        }
        return page;
    }

    // Every stored record of a lake collection (`ask_messages` → `lake_ask_messages`), for tests
    // that inspect or tamper with what the workspace holds.
    std::map<std::string, json>& rows(std::string_view collection) {
        return collections["lake_" + std::string(collection)].records;
    }

private:
    static control::ZettaControlResult<json> fail(std::string code, std::string message,
                                                  std::string path) {
        return std::unexpected(
            control::ZettaControlError{std::move(code), std::move(message), std::move(path)});
    }

    static control::ZettaControlResult<json> conflict(const json& record, std::string path) {
        control::ZettaControlError error{"conflict",
                                         "record '" + record.at("key").get<std::string>() +
                                             "' is at revision " + record.at("revision").dump(),
                                         std::move(path)};
        error.details = {{"revision", record.at("revision")}};
        return std::unexpected(std::move(error));
    }

    static json written(const std::string& collection, const std::string& key,
                        std::int64_t revision) {
        return {{"collection", collection}, {"key", key},     {"revision", revision},
                {"updated_ms", revision / 1000}, {"owner", "node-1"}, {"replicas", 0},
                {"replicas_expected", 0}};
    }
};

} // namespace lake::test
