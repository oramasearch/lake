#pragma once

#include <optional>
#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// Who created each workspace topic (docs/control-plane/streams.md, lake#379). The record lives in
// the workspace Zetta whose topic it describes, so creator ownership follows that workspace's
// incarnation: deleting or recreating the workspace takes its topics and their creators together.
// One record per topic, keyed by the blind index of the topic name and written `if_absent`, so a
// name has at most one recorded creator and only the first confirmed creation claims it.
struct StreamOwnerCodec {
    using Value = StreamOwner;
    static constexpr RecordCollection collection{"stream_owners", 1};

    static std::string lookup(const StreamOwner& owner);
    static nlohmann::json encode(const StreamOwner& owner);
    // `workspace_id` is the workspace the record opened in (the seal binds it).
    static std::optional<StreamOwner> decode(const StoredRecord& record,
                                             const std::string& workspace_id);
};

// The record key of a topic's ownership in one workspace.
std::string stream_owner_key(const WorkspaceRecords& records, const std::string& topic);

// The recorded creator of `topic`, or an empty string when nobody claimed it.
liquid::runtime::task<ZettaControlResult<std::string>>
stream_creator(WorkspaceRecords& records, std::string customer_id, std::string topic);

// Records `owner.created_by` as the creator of `owner.topic`, only when no creator is recorded;
// an existing record is `conflict`. `at_us` keeps a copied row's instant (0: now).
liquid::runtime::task<ZettaControlResult<StreamOwner>>
claim_stream_owner(WorkspaceRecords& records, StreamOwner owner, std::int64_t at_us = 0);

// Removes the ownership record when `owner.created_by` is its creator; anything else is left as
// it is. A topic without a record is already released.
liquid::runtime::task<ZettaControlResult<void>> release_stream_owner(WorkspaceRecords& records,
                                                                     StreamOwner owner);

} // namespace lake::control
