#pragma once

#include <cstddef>
#include <string>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/network.hpp"
#include "lake/control/zetta_control.hpp"

namespace lake::control {

// Workspace network history, supplied by zetta#1281 when its wire contract is available.
// This interface does not prescribe a Zetta collection or an event body schema.
struct NetworkRecordPage {
    nlohmann::json records = nlohmann::json::array();
    bool available = false;
    std::string pending = "zetta#1281";
};

class NetworkRecords {
public:
    virtual ~NetworkRecords() = default;
    virtual liquid::runtime::task<ZettaControlResult<NetworkRecordPage>>
    sessions(NetworkScope scope, std::size_t limit) = 0;
    virtual liquid::runtime::task<ZettaControlResult<NetworkRecordPage>>
    activity(NetworkScope scope, std::size_t limit) = 0;
};

// The production default explicitly distinguishes unavailable history from no activity.
class PendingNetworkRecords final : public NetworkRecords {
public:
    liquid::runtime::task<ZettaControlResult<NetworkRecordPage>>
    sessions(NetworkScope scope, std::size_t limit) override;
    liquid::runtime::task<ZettaControlResult<NetworkRecordPage>>
    activity(NetworkScope scope, std::size_t limit) override;
};

} // namespace lake::control
