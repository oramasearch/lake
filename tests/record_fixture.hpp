#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
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

    // The labelled time series (zetta#1231, zetta docs/data/time-series.md) as lake's request
    // series uses it: counters and gauges, every point kept as recorded.
    struct Metric {
        json definition;
        std::vector<json> points; // {labels, value, at_ms}
    };
    std::map<std::string, Metric> metrics;
    // Answers that many metric calls with 503 `store_unavailable` first.
    int metric_unavailable_next = 0;

    static bool handles(std::string_view capability, const json&) {
        return capability == "create_collection" || capability == "put_record" ||
               capability == "get_record" || capability == "delete_record" ||
               capability == "list_records" || capability == "create_metric" ||
               capability == "record_metric" || capability == "query_metric";
    }

    // The total of a counter over the series whose labels include `labels`.
    double metric_total(const std::string& name, const json& labels = json::object()) const {
        const auto found = metrics.find(name);
        double total = 0;
        if (found == metrics.end()) {
            return total;
        }
        for (const auto& point : found->second.points) {
            bool matches = true;
            for (const auto& [key, value] : labels.items()) {
                matches = matches && point.at("labels").value(key, json()) == value;
            }
            total += matches ? point.at("value").get<double>() : 0;
        }
        return total;
    }

    control::ZettaControlResult<json> run_metric(std::string_view capability, const json& input) {
        if (metric_unavailable_next > 0) {
            --metric_unavailable_next;
            return fail("store_unavailable", "the workspace did not answer", "");
        }
        const std::string name = input.value("metric", std::string());
        if (capability == "create_metric") {
            json definition{{"metric", name},
                            {"kind", input.value("kind", std::string())},
                            {"resolution_ms", input.value("resolution_ms", 60000)},
                            {"retention_ms", input.value("retention_ms", json())}};
            const auto existing = metrics.find(name);
            if (existing != metrics.end()) {
                if (existing->second.definition != definition) {
                    return fail("conflict", "a different metric exists", "/metric");
                }
                return json{{"metric", definition}, {"created", false}};
            }
            metrics[name] = Metric{definition, {}};
            return json{{"metric", definition}, {"created", true}};
        }
        const auto found = metrics.find(name);
        if (found == metrics.end()) {
            return fail("not_found", "no metric '" + name + "'", "/metric");
        }
        if (capability == "record_metric") {
            const json points = input.value("points", json::array());
            if (points.empty() || points.size() > 1000) {
                return fail("invalid_input", "1 to 1000 points", "/points");
            }
            for (const auto& point : points) {
                found->second.points.push_back(point);
            }
            return json{{"metric", name}, {"recorded", points.size()}};
        }
        // query_metric (zetta docs/data/time-series.md): the points in [from_ms, to_ms) whose
        // labels include `labels`, one series per value of the `group_by` labels (none: one
        // series for everything), each with its total and, by `step_ms`, its points.
        const json wanted = input.value("labels", json::object());
        const json group_by = input.value("group_by", json::array());
        const auto from_ms = input.value("from_ms", std::int64_t{0});
        const auto to_ms = input.value("to_ms", std::numeric_limits<std::int64_t>::max());
        const auto step_ms = input.value("step_ms", std::int64_t{0});
        // A gauge answers each step's latest reading (`last`, its default aggregate); a counter
        // the sum.
        const bool last = found->second.definition.value("kind", std::string()) == "gauge" &&
                          input.value("aggregate", std::string("last")) == "last";
        struct Series {
            json labels = json::object();
            double total = 0;
            std::int64_t total_at = std::numeric_limits<std::int64_t>::min();
            std::map<std::int64_t, double> steps;
            std::map<std::int64_t, std::int64_t> step_at;
        };
        std::map<std::string, Series> series;
        for (const auto& point : found->second.points) {
            bool matches = true;
            for (const auto& [key, value] : wanted.items()) {
                matches = matches && point.at("labels").value(key, json()) == value;
            }
            const auto at_ms = point.value("at_ms", std::int64_t{0});
            if (!matches || at_ms < from_ms || at_ms >= to_ms) {
                continue;
            }
            json labels = json::object();
            for (const auto& name : group_by) {
                if (point.at("labels").contains(name.get<std::string>())) {
                    labels[name.get<std::string>()] =
                        point.at("labels").at(name.get<std::string>());
                }
            }
            auto& entry = series[labels.dump()];
            entry.labels = labels;
            const double value = point.at("value").get<double>();
            const std::int64_t step = step_ms > 0 ? at_ms - (at_ms - from_ms) % step_ms : from_ms;
            if (last) {
                if (at_ms >= entry.total_at) {
                    entry.total = value;
                    entry.total_at = at_ms;
                }
                if (!entry.step_at.contains(step) || at_ms >= entry.step_at[step]) {
                    entry.steps[step] = value;
                    entry.step_at[step] = at_ms;
                }
                continue;
            }
            entry.total += value;
            entry.steps[step] += value;
        }
        json rows = json::array();
        for (const auto& [ignored, entry] : series) {
            json points = json::array();
            for (const auto& [at_ms, value] : entry.steps) {
                points.push_back({{"at_ms", at_ms}, {"value", value}});
            }
            rows.push_back({{"labels", entry.labels}, {"total", entry.total}, {"points", points}});
        }
        return json{{"metric", found->second.definition},
                    {"aggregate", last ? "last" : "sum"},
                    {"series", rows}};
    }

    control::ZettaControlResult<json> run(std::string_view capability, const json& input) {
        calls.emplace_back(capability);
        if (capability.ends_with("_metric")) {
            return run_metric(capability, input);
        }
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
