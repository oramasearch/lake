#include "lake/api/clerk_webhooks.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "lake/audit/audit.hpp"
#include "lake/control/store.hpp"

namespace lake::api::clerk_webhooks {
namespace {

std::string base64(const unsigned char* data, std::size_t size) {
    std::string output(((size + 2) / 3) * 4, '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(output.data()), data, static_cast<int>(size));
    output.resize(static_cast<std::size_t>(written));
    return output;
}

std::vector<unsigned char> from_base64(std::string_view text) {
    if (text.empty() || text.size() % 4 != 0) {
        return {};
    }
    std::vector<unsigned char> output(text.size() / 4 * 3);
    const int size = EVP_DecodeBlock(output.data(),
                                     reinterpret_cast<const unsigned char*>(text.data()),
                                     static_cast<int>(text.size()));
    if (size < 0) {
        return {};
    }
    std::size_t padding = 0;
    if (text.ends_with("==")) {
        padding = 2;
    } else if (text.ends_with('=')) {
        padding = 1;
    }
    output.resize(static_cast<std::size_t>(size) - padding);
    return output;
}

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The primary email when Clerk has verified it; empty otherwise, so an unverified address never
// links to an existing account or claims one (decision 44 trusts only Clerk's verification).
std::string verified_primary_email(const nlohmann::json& data) {
    const std::string primary = data.value("primary_email_address_id", "");
    if (!data.contains("email_addresses") || !data.at("email_addresses").is_array()) {
        return {};
    }
    for (const auto& address : data.at("email_addresses")) {
        if (!address.is_object() || address.value("id", "") != primary) {
            continue;
        }
        const auto verification = address.value("verification", nlohmann::json::object());
        if (verification.is_object() && verification.value("status", "") == "verified") {
            return address.value("email_address", "");
        }
    }
    return {};
}

std::string display_name(const nlohmann::json& data) {
    const auto text = [&](const char* key) {
        return data.contains(key) && data.at(key).is_string() ? data.at(key).get<std::string>()
                                                              : std::string();
    };
    std::string name = text("first_name");
    const std::string last = text("last_name");
    if (!last.empty()) {
        name += name.empty() ? last : " " + last;
    }
    return name.empty() ? text("username") : name;
}

void record(ApiContext& context,
            const std::string& action,
            const std::string& outcome,
            const std::string& principal,
            nlohmann::json details) {
    if (context.store == nullptr) {
        return;
    }
    const audit::IdentityOptions options{.configured_seed = context.identity_seed,
                                         .instance_kek = context.instance_kek};
    (void)audit::record(*context.store,
                        audit::EventDraft{.kind = "authentication",
                                          .principal = principal.empty() ? "clerk" : principal,
                                          .object = "clerk",
                                          .action = action,
                                          .outcome = outcome,
                                          .request_id = context.request_id,
                                          .method = "clerk_webhook",
                                          .details = std::move(details)},
                        options);
}

Outcome answer(int status, nlohmann::json body) {
    return Outcome{status, std::move(body)};
}

} // namespace

std::string sign(std::string_view secret, const Delivery& delivery) {
    std::string_view encoded = secret;
    if (encoded.starts_with("whsec_")) {
        encoded.remove_prefix(6);
    }
    const auto key = from_base64(encoded);
    const std::string content = delivery.id + "." + delivery.timestamp + "." + delivery.body;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size = 0;
    if (key.empty() || HMAC(EVP_sha256(),
                            key.data(),
                            static_cast<int>(key.size()),
                            reinterpret_cast<const unsigned char*>(content.data()),
                            content.size(),
                            digest,
                            &size) == nullptr) {
        return {};
    }
    return base64(digest, size);
}

std::expected<void, std::string>
verify(std::string_view secret, const Delivery& delivery, std::int64_t now_unix) {
    if (delivery.id.empty() || delivery.timestamp.empty() || delivery.signature.empty()) {
        return std::unexpected("svix-id, svix-timestamp and svix-signature are required");
    }
    std::int64_t timestamp = 0;
    try {
        std::size_t used = 0;
        timestamp = std::stoll(delivery.timestamp, &used);
        if (used != delivery.timestamp.size()) {
            return std::unexpected("svix-timestamp is not a number");
        }
    } catch (...) {
        return std::unexpected("svix-timestamp is not a number");
    }
    const std::int64_t now = now_unix == 0 ? now_seconds() : now_unix;
    if (timestamp < now - tolerance_seconds || timestamp > now + tolerance_seconds) {
        return std::unexpected("svix-timestamp is outside the five-minute window");
    }
    const std::string expected = sign(secret, delivery);
    if (expected.empty()) {
        return std::unexpected("the webhook secret is not a whsec_ secret");
    }
    std::size_t start = 0;
    const std::string& header = delivery.signature;
    while (start < header.size()) {
        std::size_t end = header.find(' ', start);
        if (end == std::string::npos) {
            end = header.size();
        }
        const std::string_view entry(header.data() + start, end - start);
        if (entry.starts_with("v1,")) {
            const std::string_view candidate = entry.substr(3);
            if (candidate.size() == expected.size() &&
                CRYPTO_memcmp(candidate.data(), expected.data(), expected.size()) == 0) {
                return {};
            }
        }
        start = end + 1;
    }
    return std::unexpected("no signature matches");
}

Outcome receive(ApiContext& context,
                std::string_view secret,
                const Delivery& delivery,
                std::int64_t now_unix) {
    if (context.store == nullptr) {
        return answer(503, {{"error", "store_unavailable"}});
    }
    if (const auto verified = verify(secret, delivery, now_unix); !verified) {
        record(context,
               "webhook",
               "denied",
               "",
               {{"svix_id", delivery.id}, {"reason", verified.error()}});
        return answer(401, {{"error", "unauthorized"}, {"message", verified.error()}});
    }
    const auto applied = context.store->clerk_webhook_applied(delivery.id);
    if (!applied) {
        return answer(503, {{"error", "store_error"}, {"message", applied.error().message}});
    }
    if (*applied) {
        // A replayed delivery is acknowledged and not applied again.
        record(context,
               "webhook",
               "denied",
               "",
               {{"svix_id", delivery.id}, {"reason", "this delivery was already applied"}});
        return answer(200, {{"applied", false}, {"duplicate", true}});
    }
    const auto event = nlohmann::json::parse(delivery.body, nullptr, false);
    if (event.is_discarded() || !event.is_object() || !event.contains("data") ||
        !event.at("data").is_object()) {
        record(context,
               "webhook",
               "failure",
               "",
               {{"svix_id", delivery.id}, {"reason", "the body is not a Clerk event"}});
        return answer(400, {{"error", "invalid_input"}, {"message", "not a Clerk event"}});
    }
    if (!event.contains("type") || !event.at("type").is_string()) {
        return answer(400, {{"error", "invalid_input"}, {"message", "an event type is required"}});
    }
    const std::string type = event.at("type").get<std::string>();
    const nlohmann::json& data = event.at("data");
    const std::string subject =
        data.contains("id") && data.at("id").is_string() ? data.at("id").get<std::string>() : "";
    const std::string action = "webhook." + type;
    nlohmann::json details = {{"svix_id", delivery.id}, {"clerk_user", subject}, {"type", type}};

    // A refusal that a retry cannot change is acknowledged (200), so Clerk stops retrying; a
    // store failure answers 503 so it retries.
    const auto store_failure = [&](const control::StoreError& error) {
        details["reason"] = error.message;
        record(context, action, "failure", "", details);
        const bool final = error.code == "conflict" || error.code == "not_found" ||
                           error.code == "invalid_input" || error.code == "unauthorized";
        return final ? answer(200, {{"applied", false}, {"reason", error.message}})
                     : answer(503, {{"error", error.code}, {"message", error.message}});
    };
    const auto done = [&](const std::string& principal, nlohmann::json body) {
        if (const auto recorded = context.store->record_clerk_webhook(delivery.id, type, subject);
            !recorded) {
            return answer(503, {{"error", "store_error"}, {"message", recorded.error().message}});
        }
        record(context, action, "success", principal, details);
        body["applied"] = true;
        return answer(200, std::move(body));
    };

    if (type == "session.revoked" || type == "session.ended" || type == "session.removed") {
        if (!data.contains("user_id") || !data.at("user_id").is_string() || subject.empty()) {
            return answer(
                400,
                {{"error", "invalid_input"}, {"message", "a session id and user id are required"}});
        }
        const auto at = event.value("timestamp", nlohmann::json());
        if (!at.is_number_integer() || at.get<std::int64_t>() < 0) {
            return answer(
                400,
                {{"error", "invalid_input"}, {"message", "a session event timestamp is required"}});
        }
        const auto revoked = context.store->revoke_clerk_sessions(
            data.at("user_id").get<std::string>(), at.get<std::int64_t>());
        if (!revoked)
            return store_failure(revoked.error());
        details["clerk_session"] = subject;
        details["clerk_user"] = data.at("user_id");
        return done("", {{"revoked", true}});
    }
    if (type == "user.created" || type == "user.updated" || type == "user.deleted") {
        const bool deleted = type == "user.deleted";
        const auto at = data.contains("updated_at") ? data.at("updated_at")
                                                    : event.value("timestamp", nlohmann::json());
        if (!at.is_number_integer() || at.get<std::int64_t>() < 0 || subject.empty() ||
            (!deleted &&
             (!data.contains("banned") || !data.at("banned").is_boolean() ||
              !data.contains("locked") || !data.at("locked").is_boolean() ||
              (data.contains("deprovisioned") && !data.at("deprovisioned").is_boolean())))) {
            return answer(
                400,
                {{"error", "invalid_input"},
                 {"message",
                  "a user state timestamp and boolean banned/locked fields are required"}});
        }
        const bool suspended = deleted || data.at("banned").get<bool>() ||
                               data.at("locked").get<bool>() || data.value("deprovisioned", false);
        const auto applied_state = context.store->apply_clerk_user_state(
            {subject, at.get<std::int64_t>(), suspended, deleted});
        if (!applied_state)
            return store_failure(applied_state.error());
        if (!*applied_state)
            return done("", {{"stale", true}});
        details["suspended"] = suspended;
    }
    if (type == "user.created") {
        const auto minted = audit::mint_custodial_identity(
            audit::effective_instance_kek(*context.store, context.instance_kek));
        if (!minted) {
            return store_failure(control::StoreError{"store_error", minted.error().message});
        }
        const auto provisioned = context.store->provision_clerk_user({subject,
                                                                      verified_primary_email(data),
                                                                      display_name(data),
                                                                      "",
                                                                      minted->taap_id,
                                                                      minted->public_key,
                                                                      minted->wrapped_seed});
        if (!provisioned) {
            return store_failure(provisioned.error());
        }
        details["user_id"] = provisioned->user_id;
        details["created"] = provisioned->created;
        details["linked"] = provisioned->linked;
        return done(provisioned->taap_id,
                    {{"user_id", provisioned->user_id},
                     {"created", provisioned->created},
                     {"linked", provisioned->linked}});
    }
    if (type == "user.updated") {
        const auto updated = context.store->update_clerk_user(
            {subject, verified_primary_email(data), display_name(data), "", "", "", ""});
        if (!updated) {
            return store_failure(updated.error());
        }
        details["user_id"] = *updated;
        return done("", {{"user_id", *updated}});
    }
    if (type == "user.deleted") {
        const auto disabled = context.store->disable_clerk_user(subject);
        if (!disabled) {
            return store_failure(disabled.error());
        }
        details["user_id"] = *disabled;
        return done("", {{"user_id", *disabled}, {"disabled", true}});
    }
    details["reason"] = "Lake does not act on this event";
    record(context, action, "success", "", details);
    if (const auto recorded = context.store->record_clerk_webhook(delivery.id, type, subject);
        !recorded) {
        return answer(503, {{"error", "store_error"}});
    }
    return answer(200, {{"applied", false}, {"ignored", true}});
}

} // namespace lake::api::clerk_webhooks
