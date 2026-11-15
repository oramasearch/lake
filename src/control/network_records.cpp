// SPDX-License-Identifier: MIT

#include "lake/control/network_records.hpp"

namespace lake::control {

liquid::runtime::task<ZettaControlResult<NetworkRecordPage>>
PendingNetworkRecords::sessions(NetworkScope, std::size_t) {
    co_return ZettaControlResult<NetworkRecordPage>(NetworkRecordPage{});
}

liquid::runtime::task<ZettaControlResult<NetworkRecordPage>>
PendingNetworkRecords::activity(NetworkScope, std::size_t) {
    co_return ZettaControlResult<NetworkRecordPage>(NetworkRecordPage{});
}

} // namespace lake::control
