#include "lake/control/application_webhook_records.hpp"

#include "lake/control/store.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> failed(const ZettaControlError& error) {
    return ZettaControlResult<T>(std::unexpected(error));
}

std::string attempt_name(const ApplicationWebhookDelivery& delivery) {
    return "event:" + delivery.event_id + "\nwebhook:" + delivery.webhook_id +
           "\nattempt:" + std::to_string(delivery.attempt);
}

} // namespace

std::string application_webhook_lookup(const std::string& application_id) {
    return "application:" + application_id;
}

std::string ApplicationWebhookEventCodec::lookup(const ApplicationWebhookEvent& event) {
    return application_webhook_lookup(event.application_id);
}

nlohmann::json ApplicationWebhookEventCodec::encode(const ApplicationWebhookEvent& event) {
    return {{"application_id", event.application_id},
            {"event", event.event},
            {"payload", event.payload}};
}

std::optional<ApplicationWebhookEvent>
ApplicationWebhookEventCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    ApplicationWebhookEvent event;
    event.id = record.id;
    event.application_id = record.body.value("application_id", std::string());
    event.event = record.body.value("event", std::string());
    event.payload = record.body.value("payload", nlohmann::json::object());
    event.at_us = record.at_us;
    event.created_at = record_instant(record.at_us);
    if (event.application_id.empty() || event.event.empty()) {
        return std::nullopt;
    }
    return event;
}

std::string ApplicationWebhookDeliveryCodec::lookup(const ApplicationWebhookDelivery& delivery) {
    return application_webhook_lookup(delivery.application_id);
}

nlohmann::json ApplicationWebhookDeliveryCodec::encode(const ApplicationWebhookDelivery& delivery) {
    return {{"id", delivery.id},
            {"application_id", delivery.application_id},
            {"event_id", delivery.event_id},
            {"webhook_id", delivery.webhook_id},
            {"attempt", delivery.attempt},
            {"status", delivery.status},
            {"signature", delivery.signature},
            {"error", delivery.error},
            {"pending", delivery.pending}};
}

std::optional<ApplicationWebhookDelivery>
ApplicationWebhookDeliveryCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ApplicationWebhookDelivery delivery;
    delivery.id = body.value("id", std::string());
    delivery.application_id = body.value("application_id", std::string());
    delivery.event_id = body.value("event_id", std::string());
    delivery.webhook_id = body.value("webhook_id", std::string());
    delivery.attempt = body.value("attempt", 0);
    delivery.status = body.value("status", 0);
    delivery.signature = body.value("signature", std::string());
    delivery.error = body.value("error", std::string());
    delivery.pending = body.value("pending", false);
    delivery.at_us = record.at_us;
    delivery.created_at = record_instant(record.at_us);
    if (delivery.id.empty() || delivery.application_id.empty() || delivery.event_id.empty() ||
        delivery.webhook_id.empty() || delivery.attempt <= 0) {
        return std::nullopt;
    }
    return delivery;
}

std::string application_webhook_attempt_key(const WorkspaceRecords& records,
                                            const ApplicationWebhookDelivery& delivery) {
    return records.index_of(ApplicationWebhookDeliveryCodec::collection, attempt_name(delivery));
}

liquid::runtime::task<ZettaControlResult<std::optional<ApplicationWebhookEvent>>>
get_application_webhook_event(WorkspaceRecords& records, std::string application_id,
                              std::string event_id) {
    using Found = std::optional<ApplicationWebhookEvent>;
    auto stored = settled(
        co_await records.get(ApplicationWebhookEventCodec::collection, std::move(event_id)));
    if (!stored) {
        if (stored.error().code == "invalid_input") {
            co_return ZettaControlResult<Found>(Found{}); // not an id lake could have made
        }
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto event = ApplicationWebhookEventCodec::decode(**stored);
    if (!event || event->application_id != application_id) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(event));
}

liquid::runtime::task<ZettaControlResult<std::vector<ApplicationWebhookEvent>>>
list_application_webhook_events(WorkspaceRecords& records, std::string application_id,
                                std::size_t newest) {
    using Events = std::vector<ApplicationWebhookEvent>;
    ApplicationWebhookEvents events(records);
    auto listed = settled(co_await events.list(application_webhook_lookup(application_id), newest));
    if (!listed) {
        co_return failed<Events>(listed.error());
    }
    std::ranges::reverse(*listed);
    co_return ZettaControlResult<Events>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<std::vector<ApplicationWebhookDelivery>>>
list_application_webhook_deliveries(WorkspaceRecords& records, std::string application_id,
                                    std::int64_t since_us) {
    using Deliveries = std::vector<ApplicationWebhookDelivery>;
    auto stored = settled(co_await records.list(ApplicationWebhookDeliveryCodec::collection,
                                                application_webhook_lookup(application_id), 0,
                                                since_us));
    if (!stored) {
        co_return failed<Deliveries>(stored.error());
    }
    Deliveries deliveries;
    for (const StoredRecord& record : *stored) {
        auto delivery = ApplicationWebhookDeliveryCodec::decode(record);
        // A record whose key is not its attempt's was not written by lake for that attempt.
        if (delivery && delivery->application_id == application_id &&
            application_webhook_attempt_key(records, *delivery) == record.id) {
            deliveries.push_back(std::move(*delivery));
        }
    }
    std::ranges::sort(deliveries, [](const auto& left, const auto& right) {
        return std::tie(left.at_us, left.attempt) < std::tie(right.at_us, right.attempt);
    });
    co_return ZettaControlResult<Deliveries>(std::move(deliveries));
}

liquid::runtime::task<ZettaControlResult<ClaimedApplicationWebhookDelivery>>
claim_application_webhook_delivery(WorkspaceRecords& records, ApplicationWebhookDelivery delivery) {
    using Claimed = ClaimedApplicationWebhookDelivery;
    if (delivery.id.empty()) {
        delivery.id = new_uuid_v4();
    }
    const std::string key = application_webhook_attempt_key(records, delivery);
    auto stored = settled(co_await records.put(
        ApplicationWebhookDeliveryCodec::collection, key,
        ApplicationWebhookDeliveryCodec::lookup(delivery),
        ApplicationWebhookDeliveryCodec::encode(delivery), RecordCondition{.if_absent = true}, 1,
        delivery.at_us));
    if (!stored) {
        co_return failed<Claimed>(stored.error());
    }
    delivery.at_us = stored->at_us;
    delivery.created_at = record_instant(stored->at_us);
    co_return ZettaControlResult<Claimed>(Claimed{std::move(delivery), stored->revision});
}

liquid::runtime::task<ZettaControlResult<ApplicationWebhookDelivery>>
settle_application_webhook_delivery(WorkspaceRecords& records,
                                    ClaimedApplicationWebhookDelivery claimed) {
    using Delivery = ApplicationWebhookDelivery;
    claimed.delivery.pending = false;
    const std::string key = application_webhook_attempt_key(records, claimed.delivery);
    auto stored = settled(co_await records.put(
        ApplicationWebhookDeliveryCodec::collection, key,
        ApplicationWebhookDeliveryCodec::lookup(claimed.delivery),
        ApplicationWebhookDeliveryCodec::encode(claimed.delivery),
        RecordCondition{.if_revision = claimed.revision}, 2, claimed.delivery.at_us));
    if (!stored) {
        co_return failed<Delivery>(stored.error());
    }
    co_return ZettaControlResult<Delivery>(std::move(claimed.delivery));
}

} // namespace lake::control
