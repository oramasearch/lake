#include "lake/control/stream_owners.hpp"

#include <array>
#include <format>

#include <openssl/evp.h>

#include "lake/control/agent_bus.hpp"

namespace lake::control {

std::string StreamOwnerCodec::lookup(const StreamOwner& owner) {
    return "topic:" + owner.topic;
}

nlohmann::json StreamOwnerCodec::encode(const StreamOwner& owner) {
    return {{"customer_id", owner.customer_id},
            {"topic", owner.topic},
            {"created_by", owner.created_by}};
}

std::optional<StreamOwner> StreamOwnerCodec::decode(const StoredRecord& record,
                                                    const std::string& workspace_id) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    StreamOwner owner{record.body.value("customer_id", std::string()), workspace_id,
                      record.body.value("topic", std::string()),
                      record.body.value("created_by", std::string())};
    if (owner.customer_id.empty() || owner.topic.empty() || owner.created_by.empty()) {
        return std::nullopt;
    }
    return owner;
}

std::string stream_owner_key(const WorkspaceRecords& records, const std::string& topic) {
    return records.index_of(StreamOwnerCodec::collection, "topic:" + topic);
}

namespace {

using Found = std::optional<std::pair<StreamOwner, StoredRecord>>;

// The record of `topic`, when it opens here and names this space and topic. A record of another
// space cannot exist in this workspace; one that does not match is treated as tampered and absent.
liquid::runtime::task<ZettaControlResult<Found>> owner_record(WorkspaceRecords& records,
                                                              const std::string& customer_id,
                                                              const std::string& topic) {
    const std::string key = stream_owner_key(records, topic);
    if (key.empty()) {
        co_return ZettaControlResult<Found>(
            std::unexpected(ZettaControlError{"store_error", "could not index the topic"}));
    }
    auto stored = settled(co_await records.get(StreamOwnerCodec::collection, key));
    if (!stored) {
        co_return ZettaControlResult<Found>(std::unexpected(stored.error()));
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto owner = StreamOwnerCodec::decode(**stored, records.workspace_id());
    if (!owner || owner->customer_id != customer_id || owner->topic != topic) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(Found{std::pair{std::move(*owner), std::move(**stored)}});
}

} // namespace

liquid::runtime::task<ZettaControlResult<std::string>>
stream_creator(WorkspaceRecords& records, std::string customer_id, std::string topic) {
    auto found = settled(co_await owner_record(records, customer_id, topic));
    if (!found) {
        co_return ZettaControlResult<std::string>(std::unexpected(found.error()));
    }
    co_return ZettaControlResult<std::string>(*found ? (*found)->first.created_by : std::string());
}

liquid::runtime::task<ZettaControlResult<StreamOwner>>
claim_stream_owner(WorkspaceRecords& records, StreamOwner owner, std::int64_t at_us) {
    const std::string key = stream_owner_key(records, owner.topic);
    if (key.empty() || owner.customer_id.empty() || owner.topic.empty() ||
        owner.created_by.empty()) {
        co_return ZettaControlResult<StreamOwner>(
            std::unexpected(ZettaControlError{"invalid_input", "stream ownership is incomplete"}));
    }
    // `if_absent`: the key's owner in Zetta decides atomically, so of two concurrent claims of one
    // name exactly one is recorded and the other is `conflict`.
    auto written = settled(co_await records.put(
        StreamOwnerCodec::collection, key, StreamOwnerCodec::lookup(owner),
        StreamOwnerCodec::encode(owner), RecordCondition{.if_absent = true}, 1, at_us));
    if (!written) {
        co_return ZettaControlResult<StreamOwner>(std::unexpected(written.error()));
    }
    owner.workspace_id = records.workspace_id();
    co_return ZettaControlResult<StreamOwner>(std::move(owner));
}

liquid::runtime::task<ZettaControlResult<void>> release_stream_owner(WorkspaceRecords& records,
                                                                     StreamOwner owner) {
    auto found = settled(co_await owner_record(records, owner.customer_id, owner.topic));
    if (!found) {
        co_return ZettaControlResult<void>(std::unexpected(found.error()));
    }
    if (!*found || (*found)->first.created_by != owner.created_by) {
        co_return ZettaControlResult<void>{};
    }
    auto removed = settled(co_await records.remove(StreamOwnerCodec::collection,
                                                   (*found)->second.id,
                                                   (*found)->second.revision));
    if (!removed && removed.error().code != "not_found") {
        co_return ZettaControlResult<void>(std::unexpected(removed.error()));
    }
    co_return ZettaControlResult<void>{};
}

} // namespace lake::control
