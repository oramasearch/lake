// SPDX-License-Identifier: MIT

#include "lake/control/forge_records.hpp"

#include <algorithm>

namespace lake::control {

bool is_forge_record_kind(std::string_view kind) {
    return std::ranges::find(forge_record_kinds, kind) != forge_record_kinds.end();
}

std::string forge_record_lookup(const std::string& repository_id, std::string_view kind) {
    return "repository:" + repository_id + "\nkind:" + std::string(kind);
}

std::string ForgeRecordCodec::lookup(const ForgeRecord& record) {
    return forge_record_lookup(record.repository_id, record.kind);
}

nlohmann::json ForgeRecordCodec::encode(const ForgeRecord& record) {
    return {{"repository_id", record.repository_id},
            {"kind", record.kind},
            {"value", record.value},
            {"updated_at", record.updated_at}};
}

std::optional<ForgeRecord> ForgeRecordCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ForgeRecord decoded;
    decoded.id = record.id;
    decoded.repository_id = body.value("repository_id", std::string());
    decoded.kind = body.value("kind", std::string());
    decoded.value = body.value("value", nlohmann::json::object());
    decoded.created_at = record_instant(record.at_us);
    decoded.updated_at = body.value("updated_at", std::string());
    if (decoded.updated_at.empty()) {
        decoded.updated_at = decoded.created_at;
    }
    decoded.at_us = record.at_us;
    decoded.revision = record.revision;
    decoded.lake_revision = record.lake_revision;
    if (decoded.repository_id.empty() || !is_forge_record_kind(decoded.kind) ||
        !decoded.value.is_object()) {
        return std::nullopt;
    }
    return decoded;
}

} // namespace lake::control
