// SPDX-License-Identifier: MIT

#include "lake/control/opportunities.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <utility>

#include "lake/control/identity_crypto.hpp"

namespace lake::control {
namespace {

bool known_status(const std::string& status) {
    return status == "new" || status == "project" || status == "dismissed" || status == "not_yet";
}

template <class T> ZettaControlResult<T> refused(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

} // namespace

std::string opportunities_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::string opportunity_id(const std::string& task_id, int rank) {
    std::string hex = identity_crypto::sha256_hex(
        std::string("lake/opportunity\0", 17) + task_id + '\0' + std::to_string(rank));
    hex.resize(32);
    hex[12] = '8';                                                  // version 8: derived
    hex[16] = "89ab"[std::stoi(hex.substr(16, 1), nullptr, 16) & 3]; // RFC 9562 variant
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

std::optional<std::string> opportunity_problem(const Opportunity& opportunity) {
    if (opportunity.customer_id.empty() || opportunity.task_id.empty() ||
        opportunity.intent.empty() || !known_status(opportunity.status)) {
        return "opportunity is invalid";
    }
    return std::nullopt;
}

void sort_opportunities(std::vector<Opportunity>& opportunities) {
    // A scan's rows share one instant; the newest row of each scan orders the scans.
    std::map<std::string, std::int64_t> scan_at;
    for (const Opportunity& opportunity : opportunities) {
        auto& at = scan_at[opportunity.task_id];
        at = std::max(at, opportunity.at_us);
    }
    std::ranges::sort(opportunities, [&](const Opportunity& a, const Opportunity& b) {
        const std::int64_t at_a = scan_at[a.task_id], at_b = scan_at[b.task_id];
        if (at_a != at_b) {
            return at_a > at_b;
        }
        if (a.task_id != b.task_id) {
            return a.task_id > b.task_id;
        }
        return a.rank != b.rank ? a.rank < b.rank : a.id < b.id;
    });
}

std::string OpportunityCodec::lookup(const Opportunity& opportunity) {
    return opportunities_lookup(opportunity.customer_id);
}

nlohmann::json OpportunityCodec::encode(const Opportunity& opportunity) {
    return {{"customer_id", opportunity.customer_id},
            {"task_id", opportunity.task_id},
            {"rank", opportunity.rank},
            {"intent", opportunity.intent},
            {"solution", opportunity.solution},
            {"methodology", opportunity.methodology},
            {"why", opportunity.why},
            {"payload", opportunity.payload},
            {"status", opportunity.status},
            {"project", opportunity.project}};
}

std::optional<Opportunity> OpportunityCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    Opportunity opportunity;
    opportunity.id = record.id;
    opportunity.customer_id = body.value("customer_id", std::string());
    opportunity.task_id = body.value("task_id", std::string());
    opportunity.rank = body.value("rank", 0);
    opportunity.intent = body.value("intent", std::string());
    opportunity.solution = body.value("solution", std::string());
    opportunity.methodology = body.value("methodology", std::string());
    opportunity.why = body.value("why", std::string());
    opportunity.payload = body.value("payload", nlohmann::json::object());
    opportunity.status = body.value("status", std::string());
    opportunity.project = body.value("project", std::string());
    opportunity.at_us = record.at_us;
    opportunity.created_at = record_instant(record.at_us);
    opportunity.lake_revision = record.lake_revision;
    if (opportunity_problem(opportunity)) {
        return std::nullopt;
    }
    return opportunity;
}

liquid::runtime::task<ZettaControlResult<std::size_t>>
record_opportunities(WorkspaceRecords& records, std::vector<Opportunity> rows) {
    if (rows.empty()) {
        co_return ZettaControlResult<std::size_t>(std::size_t{0});
    }
    const std::int64_t at_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    for (Opportunity& row : rows) {
        if (auto problem = opportunity_problem(row)) {
            co_return refused<std::size_t>("invalid_input", *problem);
        }
        row.id = opportunity_id(row.task_id, row.rank);
    }
    // Rows are written in order, so a present last row means the scan was saved before: one read
    // per settle once it has been.
    auto last = settled(co_await records.get(OpportunityCodec::collection, rows.back().id));
    if (!last) {
        co_return ZettaControlResult<std::size_t>(std::unexpected(last.error()));
    }
    if (*last) {
        co_return ZettaControlResult<std::size_t>(std::size_t{0});
    }
    Opportunities set(records);
    std::size_t written = 0;
    for (Opportunity& row : rows) {
        const std::string id = row.id;
        auto appended = settled(co_await set.append(std::move(row), RecordWrite{id, at_us}));
        if (appended) {
            ++written;
        } else if (appended.error().code != "conflict") {
            co_return ZettaControlResult<std::size_t>(std::unexpected(appended.error()));
        }
    }
    co_return ZettaControlResult<std::size_t>(written);
}

liquid::runtime::task<ZettaControlResult<std::vector<Opportunity>>>
list_opportunities(WorkspaceRecords& records, std::string customer_id) {
    Opportunities set(records);
    auto listed = settled(co_await set.list(opportunities_lookup(customer_id)));
    if (!listed) {
        co_return ZettaControlResult<std::vector<Opportunity>>(std::unexpected(listed.error()));
    }
    std::erase_if(*listed, [&](const Opportunity& row) { return row.customer_id != customer_id; });
    sort_opportunities(*listed);
    co_return ZettaControlResult<std::vector<Opportunity>>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<std::optional<Opportunity>>>
get_opportunity(WorkspaceRecords& records, std::string customer_id, std::string id) {
    using Found = std::optional<Opportunity>;
    auto got = settled(co_await records.get(OpportunityCodec::collection, std::move(id)));
    if (!got) {
        if (got.error().code == "invalid_input") {
            co_return ZettaControlResult<Found>(Found{}); // not a lake id: no such Opportunity
        }
        co_return ZettaControlResult<Found>(std::unexpected(got.error()));
    }
    if (!*got) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto decoded = OpportunityCodec::decode(**got);
    if (!decoded || decoded->customer_id != customer_id) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(Found{std::move(*decoded)});
}

liquid::runtime::task<ZettaControlResult<Opportunity>>
set_opportunity_status(WorkspaceRecords& records, std::string customer_id, std::string id,
                       std::string status, std::string project) {
    if (!known_status(status)) {
        co_return refused<Opportunity>("invalid_input", "unknown Opportunity status " + status);
    }
    ZettaControlError last{"conflict", "the Opportunity kept changing"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto got = settled(co_await records.get(OpportunityCodec::collection, id));
        if (!got && got.error().code != "invalid_input") {
            co_return ZettaControlResult<Opportunity>(std::unexpected(got.error()));
        }
        std::optional<Opportunity> current;
        if (got && *got) {
            current = OpportunityCodec::decode(**got);
        }
        // Only an Opportunity of this space opens and decodes here; its space never changes, so
        // the write below keeps its lookup.
        if (!current || current->customer_id != customer_id) {
            co_return refused<Opportunity>("not_found", "no such Opportunity in this workspace");
        }
        Opportunity next = *current;
        next.status = status;
        next.project = project;
        auto written = settled(co_await records.put(
            OpportunityCodec::collection, id, OpportunityCodec::lookup(next),
            OpportunityCodec::encode(next), RecordCondition{.if_revision = (*got)->revision},
            (*got)->lake_revision + 1, (*got)->at_us));
        if (written) {
            auto decoded = OpportunityCodec::decode(*written);
            if (!decoded) {
                co_return refused<Opportunity>("store_error", "the stored record did not decode");
            }
            co_return ZettaControlResult<Opportunity>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<Opportunity>(std::unexpected(written.error()));
        }
        last = written.error();
    }
    co_return ZettaControlResult<Opportunity>(std::unexpected(last));
}

} // namespace lake::control
