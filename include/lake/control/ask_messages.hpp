#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Ask Orama and Agent Factory conversations (docs/api/ask-orama.md, lake#370): the first
// collection on WorkspaceRecords. A message lives in the workspace Zetta of the workspace it was
// written in, found by its conversation and the person who had it, so only that person reads it
// back and another workspace of the same space never sees it.
struct AskMessageCodec {
    using Value = AskMessage;
    static constexpr RecordCollection collection{"ask_messages", 1};

    static std::string lookup(const AskMessage& message);
    static nlohmann::json encode(const AskMessage& message);
    static std::optional<AskMessage> decode(const StoredRecord& record);
};

// The lookup key of one person's conversation.
std::string ask_conversation_lookup(const std::string& conversation_id, const std::string& user_id);

// Refuses a message that names no conversation or person, or a role other than user/assistant.
std::optional<std::string> ask_message_problem(const AskMessage& message);

using AskMessages = RecordSet<AskMessageCodec>;

} // namespace lake::control
