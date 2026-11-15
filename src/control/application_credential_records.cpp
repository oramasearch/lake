// SPDX-License-Identifier: MIT

#include "lake/control/application_credential_records.hpp"

#include "lake/control/application_webhook_records.hpp"
#include "lake/control/store.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> failed(const ZettaControlError& error) {
    return ZettaControlResult<T>(std::unexpected(error));
}

nlohmann::json key_json(const ApplicationSigningKey& key) {
    return {{"kid", key.kid},
            {"version", key.version},
            {"public_key", key.public_key},
            {"encrypted_seed", key.encrypted_seed},
            {"current", key.current},
            {"created_at", key.created_at},
            {"retired_at", key.retired_at}};
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string ApplicationSigningKeysCodec::lookup(const ApplicationSigningKeyRing& ring) {
    return application_webhook_lookup(ring.application_id);
}

nlohmann::json ApplicationSigningKeysCodec::encode(const ApplicationSigningKeyRing& ring) {
    nlohmann::json keys = nlohmann::json::array();
    for (const ApplicationSigningKey& key : ring.keys) {
        keys.push_back(key_json(key));
    }
    return {{"application_id", ring.application_id}, {"keys", std::move(keys)}};
}

std::optional<ApplicationSigningKeyRing>
ApplicationSigningKeysCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object() ||
        !record.body.contains("keys") || !record.body.at("keys").is_array()) {
        return std::nullopt;
    }
    ApplicationSigningKeyRing ring;
    ring.application_id = record.body.value("application_id", std::string());
    // The ring is keyed by its application: a body under another key is not this application's.
    if (ring.application_id.empty() || ring.application_id != record.id) {
        return std::nullopt;
    }
    int current = 0;
    for (const nlohmann::json& value : record.body.at("keys")) {
        if (!value.is_object()) {
            return std::nullopt;
        }
        ApplicationSigningKey key;
        key.application_id = ring.application_id;
        key.kid = value.value("kid", std::string());
        key.version = value.value("version", 0);
        key.public_key = value.value("public_key", std::string());
        key.encrypted_seed = value.value("encrypted_seed", std::string());
        key.current = value.value("current", false);
        key.created_at = value.value("created_at", std::string());
        key.retired_at = value.value("retired_at", std::string());
        if (key.kid.empty() || key.version <= 0 || key.public_key.empty() ||
            key.encrypted_seed.empty()) {
            return std::nullopt;
        }
        current += key.current ? 1 : 0;
        ring.keys.push_back(std::move(key));
    }
    if (current != 1) {
        return std::nullopt;
    }
    ring.at_us = record.at_us;
    ring.lake_revision = record.lake_revision;
    ring.revision = record.revision;
    return ring;
}

std::string ApplicationWebhookCodec::lookup(const ApplicationWebhook& webhook) {
    return application_webhook_lookup(webhook.application_id);
}

nlohmann::json ApplicationWebhookCodec::encode(const ApplicationWebhook& webhook) {
    return {{"application_id", webhook.application_id},
            {"url", webhook.url},
            {"events", webhook.events},
            {"encrypted_secret", webhook.encrypted_secret},
            {"enabled", webhook.enabled},
            {"updated_at", webhook.updated_at}};
}

std::optional<ApplicationWebhook> ApplicationWebhookCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ApplicationWebhook webhook;
    webhook.id = record.id;
    webhook.application_id = body.value("application_id", std::string());
    webhook.url = body.value("url", std::string());
    const auto events = body.find("events");
    if (events != body.end() && events->is_array()) {
        for (const nlohmann::json& event : *events) {
            if (event.is_string()) {
                webhook.events.push_back(event.get<std::string>());
            }
        }
    }
    webhook.encrypted_secret = body.value("encrypted_secret", std::string());
    webhook.enabled = body.value("enabled", true);
    webhook.at_us = record.at_us;
    webhook.created_at = record_instant(record.at_us);
    webhook.updated_at = body.value("updated_at", webhook.created_at);
    if (webhook.application_id.empty() || webhook.url.empty() || webhook.events.empty() ||
        webhook.encrypted_secret.empty()) {
        return std::nullopt;
    }
    return webhook;
}

liquid::runtime::task<ZettaControlResult<std::optional<ApplicationSigningKeyRing>>>
get_application_signing_keys(WorkspaceRecords& records, std::string application_id) {
    using Found = std::optional<ApplicationSigningKeyRing>;
    auto stored =
        settled(co_await records.get(ApplicationSigningKeysCodec::collection, application_id));
    // Every failure is reported, an invalid id or a workspace that is gone included: absence is
    // only what the workspace says is absent.
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto ring = ApplicationSigningKeysCodec::decode(**stored);
    if (!ring || ring->application_id != application_id) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(ring));
}

liquid::runtime::task<ZettaControlResult<ApplicationSigningKeyRing>>
create_application_signing_keys(WorkspaceRecords& records, ApplicationSigningKeyRing ring) {
    auto stored = settled(co_await records.put(
        ApplicationSigningKeysCodec::collection, ring.application_id,
        ApplicationSigningKeysCodec::lookup(ring), ApplicationSigningKeysCodec::encode(ring),
        RecordCondition{.if_absent = true}, 1, ring.at_us));
    if (!stored) {
        co_return failed<ApplicationSigningKeyRing>(stored.error());
    }
    ring.at_us = stored->at_us;
    ring.lake_revision = stored->lake_revision;
    ring.revision = stored->revision;
    co_return ZettaControlResult<ApplicationSigningKeyRing>(std::move(ring));
}

liquid::runtime::task<ZettaControlResult<ApplicationSigningKey>>
rotate_application_signing_keys(WorkspaceRecords& records, std::string application_id,
                                SigningKeyMaker make_key, int attempts) {
    ZettaControlError last{"conflict", "the signing keys kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto found = settled(co_await get_application_signing_keys(records, application_id));
        if (!found) {
            co_return failed<ApplicationSigningKey>(found.error());
        }
        if (!*found) {
            co_return failed<ApplicationSigningKey>(ZettaControlError{
                "not_found", "application has no signing keys in its workspace"});
        }
        ApplicationSigningKeyRing ring = std::move(**found);
        int version = 0;
        for (const ApplicationSigningKey& key : ring.keys) {
            version = std::max(version, key.version);
        }
        auto next = make_key(version + 1);
        if (!next) {
            co_return failed<ApplicationSigningKey>(next.error());
        }
        const std::string now = record_instant(now_us());
        for (ApplicationSigningKey& key : ring.keys) {
            if (key.current) {
                key.current = false;
                key.retired_at = now;
            }
        }
        ApplicationSigningKey added = std::move(*next);
        added.application_id = application_id;
        added.seed.clear(); // only the envelope is kept
        added.current = true;
        added.created_at = now;
        added.retired_at.clear();
        ring.keys.push_back(added);
        auto written = settled(co_await records.put(
            ApplicationSigningKeysCodec::collection, application_id,
            ApplicationSigningKeysCodec::lookup(ring), ApplicationSigningKeysCodec::encode(ring),
            RecordCondition{.if_revision = ring.revision}, ring.lake_revision + 1, ring.at_us));
        if (written) {
            co_return ZettaControlResult<ApplicationSigningKey>(std::move(added));
        }
        if (written.error().code != "conflict") {
            co_return failed<ApplicationSigningKey>(written.error());
        }
        last = written.error(); // another rotation won: reread and make the next version
    }
    co_return failed<ApplicationSigningKey>(last);
}

liquid::runtime::task<ZettaControlResult<ApplicationWebhook>>
create_application_webhook(WorkspaceRecords& records, ApplicationWebhook webhook) {
    if (webhook.id.empty()) {
        webhook.id = new_uuid_v4();
    }
    webhook.secret.clear(); // only the envelope is kept
    const std::int64_t at_us = webhook.at_us > 0 ? webhook.at_us : now_us();
    if (webhook.updated_at.empty()) {
        webhook.updated_at = record_instant(at_us);
    }
    auto stored = settled(co_await records.put(
        ApplicationWebhookCodec::collection, webhook.id, ApplicationWebhookCodec::lookup(webhook),
        ApplicationWebhookCodec::encode(webhook), RecordCondition{.if_absent = true}, 1, at_us));
    if (!stored) {
        co_return failed<ApplicationWebhook>(stored.error());
    }
    auto decoded = ApplicationWebhookCodec::decode(*stored);
    if (!decoded) {
        co_return failed<ApplicationWebhook>(
            ZettaControlError{"store_error", "the stored webhook did not decode"});
    }
    co_return ZettaControlResult<ApplicationWebhook>(std::move(*decoded));
}

liquid::runtime::task<ZettaControlResult<std::optional<ApplicationWebhook>>>
get_application_webhook(WorkspaceRecords& records, std::string application_id,
                        std::string webhook_id) {
    using Found = std::optional<ApplicationWebhook>;
    auto stored =
        settled(co_await records.get(ApplicationWebhookCodec::collection, std::move(webhook_id)));
    // Every failure is reported, an invalid id or a workspace that is gone included: absence is
    // only what the workspace says is absent.
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto webhook = ApplicationWebhookCodec::decode(**stored);
    if (!webhook || webhook->application_id != application_id) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(webhook));
}

liquid::runtime::task<ZettaControlResult<std::vector<ApplicationWebhook>>>
list_application_webhooks(WorkspaceRecords& records, std::string application_id,
                          std::size_t newest) {
    using Webhooks = std::vector<ApplicationWebhook>;
    auto stored = settled(co_await records.list(ApplicationWebhookCodec::collection,
                                                application_webhook_lookup(application_id),
                                                newest));
    if (!stored) {
        co_return failed<Webhooks>(stored.error());
    }
    Webhooks webhooks;
    for (const StoredRecord& record : *stored) {
        auto webhook = ApplicationWebhookCodec::decode(record);
        if (webhook && webhook->application_id == application_id) {
            webhooks.push_back(std::move(*webhook));
        }
    }
    std::ranges::reverse(webhooks);
    co_return ZettaControlResult<Webhooks>(std::move(webhooks));
}

} // namespace lake::control
