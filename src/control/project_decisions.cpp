// SPDX-License-Identifier: MIT

#include "lake/control/project_decisions.hpp"

#include <utility>

namespace lake::control {
namespace {

std::string decisions_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

} // namespace

std::string ProjectDecisionCodec::lookup(const ProjectDecision& decision) {
    return decisions_lookup(decision.customer_id);
}

nlohmann::json ProjectDecisionCodec::encode(const ProjectDecision& decision) {
    return {{"customer_id", decision.customer_id},
            {"project", decision.project},
            {"decision", decision.decision},
            {"created_by", decision.created_by}};
}

std::optional<ProjectDecision> ProjectDecisionCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    ProjectDecision decision;
    decision.project_id = record.id;
    decision.customer_id = record.body.value("customer_id", std::string());
    decision.project = record.body.value("project", std::string());
    decision.decision = record.body.value("decision", std::string());
    decision.created_by = record.body.value("created_by", std::string());
    decision.created_at = record_instant(record.at_us);
    if (decision.customer_id.empty() || decision.decision.empty()) {
        return std::nullopt;
    }
    return decision;
}

liquid::runtime::task<ZettaControlResult<ProjectDecision>>
record_project_decision(WorkspaceRecords& records, ProjectDecision decision) {
    if (decision.project_id.empty() || decision.customer_id.empty() || decision.decision.empty()) {
        co_return ZettaControlResult<ProjectDecision>(
            std::unexpected(ZettaControlError{"invalid_input", "the decision is incomplete"}));
    }
    const std::string id = decision.project_id;
    auto written = settled(co_await records.put(ProjectDecisionCodec::collection,
                                                id,
                                                ProjectDecisionCodec::lookup(decision),
                                                ProjectDecisionCodec::encode(decision),
                                                RecordCondition{.if_absent = true}));
    if (!written) {
        co_return ZettaControlResult<ProjectDecision>(std::unexpected(written.error()));
    }
    auto decoded = ProjectDecisionCodec::decode(*written);
    if (!decoded) {
        co_return ZettaControlResult<ProjectDecision>(
            std::unexpected(ZettaControlError{"store_error", "the stored record did not decode"}));
    }
    co_return ZettaControlResult<ProjectDecision>(std::move(*decoded));
}

liquid::runtime::task<ZettaControlResult<std::optional<ProjectDecision>>>
get_project_decision(WorkspaceRecords& records, std::string customer_id, std::string project_id) {
    using Found = std::optional<ProjectDecision>;
    auto got = settled(co_await records.get(ProjectDecisionCodec::collection, project_id));
    if (!got) {
        // A key the store refuses (not a UUID) names no decision.
        if (got.error().code == "invalid_input" || got.error().code == "not_found") {
            co_return ZettaControlResult<Found>(Found{});
        }
        co_return ZettaControlResult<Found>(std::unexpected(got.error()));
    }
    if (!*got) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto decoded = ProjectDecisionCodec::decode(**got);
    if (!decoded || decoded->customer_id != customer_id) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(decoded));
}

liquid::runtime::task<ZettaControlResult<void>> remove_project_decision(WorkspaceRecords& records,
                                                                        std::string customer_id,
                                                                        std::string project_id) {
    auto found = settled(co_await get_project_decision(records, customer_id, project_id));
    if (!found) {
        co_return ZettaControlResult<void>(std::unexpected(found.error()));
    }
    if (!*found) {
        co_return ZettaControlResult<void>();
    }
    auto removed = settled(co_await records.remove(ProjectDecisionCodec::collection, project_id));
    if (!removed && removed.error().code != "not_found") {
        co_return ZettaControlResult<void>(std::unexpected(removed.error()));
    }
    co_return ZettaControlResult<void>();
}

} // namespace lake::control
