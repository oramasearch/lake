#include "lake/control/drive_share_links.hpp"

#include <algorithm>
#include <cctype>

namespace lake::control {
namespace {

bool uuid_shaped(std::string_view value) {
    return value.size() == 36 && std::ranges::all_of(value, [](unsigned char c) {
               return std::isxdigit(c) != 0 || c == '-';
           });
}

} // namespace

std::string drive_share_link_lookup(const std::string& repository_id,
                                    const std::string& object_id) {
    return "repository:" + repository_id + "\nobject:" + object_id;
}

std::optional<std::string> drive_share_link_problem(const DriveShareLink& link) {
    if (link.repository_id.empty() || link.object_id.empty() || link.token_hash.empty() ||
        link.expires_at.empty()) {
        return "share link requires a repository, folder, token and expiry";
    }
    if (link.role != "read" && link.role != "write") {
        return "share link role must be read or write";
    }
    return std::nullopt;
}

std::string DriveShareLinkCodec::lookup(const DriveShareLink& link) {
    return drive_share_link_lookup(link.repository_id, link.object_id);
}

nlohmann::json DriveShareLinkCodec::encode(const DriveShareLink& link) {
    return {{"repository_id", link.repository_id},
            {"object_id", link.object_id},
            {"node_id", link.node_id},
            {"path", link.path},
            {"role", link.role},
            {"token_hash", link.token_hash},
            {"expires_at", link.expires_at},
            {"revoked_at", link.revoked_at},
            {"created_by_taap_id", link.created_by_taap_id}};
}

std::optional<DriveShareLink> DriveShareLinkCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    DriveShareLink link;
    link.id = record.id;
    link.repository_id = body.value("repository_id", std::string());
    link.object_id = body.value("object_id", std::string());
    link.node_id = body.value("node_id", std::string());
    link.path = body.value("path", std::string());
    link.role = body.value("role", std::string());
    link.token_hash = body.value("token_hash", std::string());
    link.expires_at = body.value("expires_at", std::string());
    link.revoked_at = body.value("revoked_at", std::string());
    link.created_by_taap_id = body.value("created_by_taap_id", std::string());
    link.created_at = record_instant(record.at_us);
    link.at_us = record.at_us;
    link.revision = record.revision;
    link.lake_revision = record.lake_revision;
    if (drive_share_link_problem(link)) {
        return std::nullopt;
    }
    return link;
}

std::string drive_share_token(const std::string& workspace_id, const std::string& link_id,
                              const std::string& secret) {
    return workspace_id + "." + link_id + "." + secret;
}

std::optional<DriveShareToken> parse_drive_share_token(std::string_view token) {
    const std::size_t first = token.find('.');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t second = token.find('.', first + 1);
    if (second == std::string_view::npos || second + 1 >= token.size() ||
        token.find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view workspace = token.substr(0, first);
    const std::string_view link = token.substr(first + 1, second - first - 1);
    if (!uuid_shaped(workspace) || !uuid_shaped(link)) {
        return std::nullopt;
    }
    return DriveShareToken{std::string(workspace), std::string(link)};
}

} // namespace lake::control
