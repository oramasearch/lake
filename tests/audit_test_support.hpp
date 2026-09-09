#pragma once

#include "lake/control/store.hpp"

namespace lake::test {

// Exhaust matching pages. A complete chain or aggregate must never silently
// become the first page when more fixture events are added.
inline control::Result<std::vector<audit::Event>> audit_events(control::Store& store,
                                                               audit::Filter filter = {}) {
    std::vector<audit::Event> events;
    for (;;) {
        const auto page = store.list_audit_events(filter);
        if (!page) {
            return std::unexpected(page.error());
        }
        if (page->empty()) {
            return events;
        }
        filter.after_id = page->back().id;
        events.insert(events.end(), page->begin(), page->end());
    }
}

} // namespace lake::test
