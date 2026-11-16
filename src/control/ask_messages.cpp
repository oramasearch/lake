// SPDX-License-Identifier: MIT

#include "lake/control/ask_messages.hpp"

namespace lake::control {

std::string ask_conversation_lookup(const std::string& conversation_id,
                                    const std::string& user_id) {
    return "conversation:" + conversation_id + "\nuser:" + user_id;
}

std::optional<std::string> ask_message_problem(const AskMessage& message) {
    if (message.conversation_id.empty() || message.user_id.empty() ||
        (message.role != "user" && message.role != "assistant")) {
        return "ask message is invalid";
    }
    return std::nullopt;
}

std::string AskMessageCodec::lookup(const AskMessage& message) {
    return ask_conversation_lookup(message.conversation_id, message.user_id);
}

nlohmann::json AskMessageCodec::encode(const AskMessage& message) {
    return {{"conversation_id", message.conversation_id},
            {"customer_id", message.customer_id},
            {"user_id", message.user_id},
            {"role", message.role},
            {"content", message.content},
            {"payload", message.payload}};
}

std::optional<AskMessage> AskMessageCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    AskMessage message;
    message.id = record.id;
    message.conversation_id = body.value("conversation_id", std::string());
    message.customer_id = body.value("customer_id", std::string());
    message.user_id = body.value("user_id", std::string());
    message.role = body.value("role", std::string());
    message.content = body.value("content", std::string());
    message.payload = body.value("payload", nlohmann::json());
    message.created_at = record_instant(record.at_us);
    message.at_us = record.at_us;
    if (ask_message_problem(message)) {
        return std::nullopt;
    }
    return message;
}

} // namespace lake::control
