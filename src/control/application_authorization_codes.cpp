#include "lake/control/application_authorization_codes.hpp"

#include "lake/control/application_webhook_records.hpp"

#include <chrono>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> failed(const ZettaControlError& error) {
    return ZettaControlResult<T>(std::unexpected(error));
}

std::int64_t current_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ZettaControlError refused() {
    return ZettaControlError{"unauthorized",
                             "authorization code is invalid, expired, or already used"};
}

} // namespace

std::string ApplicationAuthorizationCodeCodec::lookup(const ApplicationAuthorizationCode& code) {
    return application_webhook_lookup(code.application_id);
}

nlohmann::json ApplicationAuthorizationCodeCodec::encode(const ApplicationAuthorizationCode& code) {
    return {{"application_id", code.application_id},
            {"user_id", code.user_id},
            {"redirect_uri", code.redirect_uri},
            {"code_hash", code.code_hash},
            {"code_challenge", code.code_challenge},
            {"nonce", code.nonce},
            {"expires_us", code.expires_us}};
}

std::optional<ApplicationAuthorizationCode>
ApplicationAuthorizationCodeCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    ApplicationAuthorizationCode code;
    code.application_id = body.value("application_id", std::string());
    code.user_id = body.value("user_id", std::string());
    code.redirect_uri = body.value("redirect_uri", std::string());
    code.code_hash = body.value("code_hash", std::string());
    code.code_challenge = body.value("code_challenge", std::string());
    code.nonce = body.value("nonce", std::string());
    code.expires_us = body.value("expires_us", std::int64_t{0});
    code.at_us = record.at_us;
    code.revision = record.revision;
    if (code.application_id.empty() || code.user_id.empty() || code.redirect_uri.empty() ||
        code.code_hash.empty() || code.code_challenge.empty() || code.expires_us <= 0) {
        return std::nullopt;
    }
    return code;
}

std::string application_authorization_code_key(const WorkspaceRecords& records,
                                               const std::string& application_id,
                                               const std::string& code_hash) {
    return records.index_of(ApplicationAuthorizationCodeCodec::collection,
                            "application:" + application_id + "\ncode:" + code_hash);
}

liquid::runtime::task<ZettaControlResult<ApplicationAuthorizationCode>>
issue_application_authorization_code(WorkspaceRecords& records, ApplicationAuthorizationCode code) {
    if (code.application_id.empty() || code.user_id.empty() || code.redirect_uri.empty() ||
        code.code_hash.empty() || code.code_challenge.empty()) {
        co_return failed<ApplicationAuthorizationCode>(
            ZettaControlError{"invalid_input", "authorization-code fields are required"});
    }
    if (code.at_us <= 0) {
        code.at_us = current_us();
    }
    if (code.expires_us <= 0) {
        code.expires_us = code.at_us + ApplicationAuthorizationCodeCodec::lifetime_ms * 1000;
    }
    const std::string key =
        application_authorization_code_key(records, code.application_id, code.code_hash);
    auto stored = settled(co_await records.put(
        ApplicationAuthorizationCodeCodec::collection, key,
        ApplicationAuthorizationCodeCodec::lookup(code),
        ApplicationAuthorizationCodeCodec::encode(code), RecordCondition{.if_absent = true}, 1,
        code.at_us));
    if (!stored) {
        co_return failed<ApplicationAuthorizationCode>(stored.error());
    }
    code.at_us = stored->at_us;
    code.revision = stored->revision;
    co_return ZettaControlResult<ApplicationAuthorizationCode>(std::move(code));
}

liquid::runtime::task<ZettaControlResult<ApplicationAuthorizationCode>>
redeem_application_authorization_code(WorkspaceRecords& records, std::string application_id,
                                      std::string code_hash, std::int64_t now_us) {
    const std::string key = application_authorization_code_key(records, application_id, code_hash);
    auto stored =
        settled(co_await records.get(ApplicationAuthorizationCodeCodec::collection, key));
    if (!stored) {
        co_return failed<ApplicationAuthorizationCode>(stored.error());
    }
    if (!*stored) {
        co_return failed<ApplicationAuthorizationCode>(refused());
    }
    auto code = ApplicationAuthorizationCodeCodec::decode(**stored);
    if (!code || code->application_id != application_id || code->code_hash != code_hash) {
        co_return failed<ApplicationAuthorizationCode>(refused());
    }
    // The delete at the revision read is the redemption: only one caller's succeeds.
    auto removed = settled(co_await records.remove(ApplicationAuthorizationCodeCodec::collection,
                                                   key, (*stored)->revision));
    if (!removed) {
        if (removed.error().code == "conflict" || removed.error().code == "not_found") {
            co_return failed<ApplicationAuthorizationCode>(refused());
        }
        co_return failed<ApplicationAuthorizationCode>(removed.error());
    }
    // An expired code is removed as well, and refused.
    if (code->expires_us <= (now_us > 0 ? now_us : current_us())) {
        co_return failed<ApplicationAuthorizationCode>(refused());
    }
    co_return ZettaControlResult<ApplicationAuthorizationCode>(std::move(*code));
}

liquid::runtime::task<ZettaControlResult<std::vector<ApplicationAuthorizationCode>>>
list_application_authorization_codes(WorkspaceRecords& records, std::string application_id) {
    using Codes = std::vector<ApplicationAuthorizationCode>;
    auto stored = settled(co_await records.list(ApplicationAuthorizationCodeCodec::collection,
                                                application_webhook_lookup(application_id)));
    if (!stored) {
        co_return failed<Codes>(stored.error());
    }
    Codes codes;
    for (const StoredRecord& record : *stored) {
        auto code = ApplicationAuthorizationCodeCodec::decode(record);
        if (code && code->application_id == application_id &&
            record.id == application_authorization_code_key(records, application_id,
                                                             code->code_hash)) {
            codes.push_back(std::move(*code));
        }
    }
    co_return ZettaControlResult<Codes>(std::move(codes));
}

} // namespace lake::control
