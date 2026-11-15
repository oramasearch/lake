// SPDX-License-Identifier: MIT

#include "lake/control/workspace_audit.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <map>
#include <utility>

#include "lake/control/store.hpp"

namespace lake::control {
namespace {

constexpr auto collection = WorkspaceAuditCodec::collection;

template <class T> ZettaControlResult<T> failed(ZettaControlError error) {
    return ZettaControlResult<T>(std::unexpected(std::move(error)));
}

std::string sequence_name(std::int64_t sequence) {
    return "sequence:" + std::to_string(sequence);
}

int digits(std::string_view text, std::size_t at, std::size_t count) {
    int value = 0;
    if (at + count > text.size()) {
        return -1;
    }
    const auto [end, error] = std::from_chars(text.data() + at, text.data() + at + count, value);
    return error == std::errc() && end == text.data() + at + count ? value : -1;
}

// The event a record holds, with its place; nullopt for a body lake did not write.
std::optional<WorkspaceAuditEvent> decoded(const StoredRecord& record,
                                           const std::string& workspace_id) {
    if (record.schema_version != 1 || !record.body.is_object() ||
        !record.body.contains("event") || !record.body.at("event").is_object()) {
        return std::nullopt;
    }
    WorkspaceAuditEvent found;
    found.event = audit::event_from_json(record.body.at("event"));
    found.sequence = record.body.value("sequence", std::int64_t{0});
    found.workspace_id = workspace_id;
    if (found.event.id.empty() || found.event.digest.empty() || found.sequence < 0) {
        return std::nullopt;
    }
    return found;
}

} // namespace

std::string workspace_audit_lookup(const std::string& workspace_id) {
    return "workspace:" + workspace_id;
}

std::int64_t audit_instant_us(std::string_view instant) {
    // YYYY-MM-DD?HH:MM:SS
    const int year = digits(instant, 0, 4), month = digits(instant, 5, 2),
              day = digits(instant, 8, 2), hour = digits(instant, 11, 2),
              minute = digits(instant, 14, 2), second = digits(instant, 17, 2);
    if (year < 0 || month < 1 || day < 1 || hour < 0 || minute < 0 || second < 0) {
        return 0;
    }
    std::int64_t micros = 0;
    if (instant.size() > 19 && instant[19] == '.') {
        std::size_t at = 20;
        int scale = 100000;
        while (at < instant.size() && std::isdigit(static_cast<unsigned char>(instant[at]))) {
            micros += (instant[at] - '0') * scale;
            scale /= 10;
            ++at;
        }
    }
    const std::chrono::year_month_day date{std::chrono::year(year),
                                           std::chrono::month(static_cast<unsigned>(month)),
                                           std::chrono::day(static_cast<unsigned>(day))};
    if (!date.ok()) {
        return 0;
    }
    const auto at = std::chrono::sys_days(date) + std::chrono::hours(hour) +
                    std::chrono::minutes(minute) + std::chrono::seconds(second);
    return std::chrono::duration_cast<std::chrono::microseconds>(at.time_since_epoch()).count() +
           micros;
}

liquid::runtime::task<ZettaControlResult<std::optional<WorkspaceAuditEvent>>>
get_chained_audit_event(WorkspaceRecords& records, std::int64_t sequence) {
    using Found = std::optional<WorkspaceAuditEvent>;
    auto stored = settled(
        co_await records.get(collection, records.index_of(collection, sequence_name(sequence))));
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto event = decoded(**stored, records.workspace_id());
    // A record is at its place only if it says so: one sealed for another place does not open,
    // and one that names another place was not written there by lake.
    if (!event || event->sequence != sequence) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(event));
}

liquid::runtime::task<ZettaControlResult<WorkspaceAuditTail>>
find_workspace_audit_tail(WorkspaceRecords& records, WorkspaceAuditTail known) {
    WorkspaceAuditTail last = known;
    std::int64_t step = 1;
    std::int64_t empty = 0;
    // Doubling probes until a place is empty.
    for (;;) {
        const std::int64_t probe = last.sequence + step;
        auto found = settled(co_await get_chained_audit_event(records, probe));
        if (!found) {
            co_return failed<WorkspaceAuditTail>(found.error());
        }
        if (!*found) {
            empty = probe;
            break;
        }
        last = {probe, (*found)->event.digest};
        step *= 2;
    }
    // Binary search between the last place taken and the first empty one.
    while (empty - last.sequence > 1) {
        const std::int64_t middle = last.sequence + (empty - last.sequence) / 2;
        auto found = settled(co_await get_chained_audit_event(records, middle));
        if (!found) {
            co_return failed<WorkspaceAuditTail>(found.error());
        }
        if (*found) {
            last = {middle, (*found)->event.digest};
        } else {
            empty = middle;
        }
    }
    co_return ZettaControlResult<WorkspaceAuditTail>(std::move(last));
}

liquid::runtime::task<ZettaControlResult<void>>
put_chained_audit_event(WorkspaceRecords& records, std::int64_t sequence,
                        const audit::Event& event) {
    auto stored = settled(co_await records.put(
        collection, records.index_of(collection, sequence_name(sequence)),
        workspace_audit_lookup(records.workspace_id()),
        {{"sequence", sequence}, {"event", audit::to_json(event)}},
        RecordCondition{.if_absent = true}, 1, audit_instant_us(event.occurred_at)));
    if (!stored) {
        co_return failed<void>(stored.error());
    }
    co_return ZettaControlResult<void>{};
}

liquid::runtime::task<ZettaControlResult<WorkspaceAuditTail>>
append_workspace_audit_event(WorkspaceRecords& records,
                             Store& store,
                             const audit::Identity& signer,
                             const audit::PendingEvent& pending,
                             const audit::IdentityOptions& options,
                             std::optional<WorkspaceAuditTail> tail,
                             std::uint64_t* conflicts) {
    using Tail = WorkspaceAuditTail;
    constexpr int place_attempts = 8;
    if (!tail) {
        auto found = settled(co_await find_workspace_audit_tail(records));
        if (!found) {
            co_return failed<Tail>(found.error());
        }
        tail = *found;
    }
    for (int attempt = 0; attempt < place_attempts; ++attempt) {
        auto event = audit::sign_pending(store, signer, pending, tail->digest, options);
        if (!event) {
            co_return failed<Tail>(ZettaControlError{event.error().code, event.error().message});
        }
        const std::int64_t place = tail->sequence + 1;
        auto put = settled(co_await put_chained_audit_event(records, place, *event));
        if (put) {
            co_return ZettaControlResult<Tail>(Tail{place, event->digest});
        }
        if (put.error().code != "conflict") {
            co_return failed<Tail>(put.error());
        }
        // Another writer took this place, or an earlier attempt of this one whose answer was
        // lost did: read it, and search on from there.
        if (conflicts != nullptr) {
            ++*conflicts;
        }
        auto there = settled(co_await get_chained_audit_event(records, place));
        if (!there) {
            co_return failed<Tail>(there.error());
        }
        if (!*there) {
            co_return failed<Tail>(ZettaControlError{
                "conflict",
                "a place in the workspace audit is taken by a record Lake cannot read"});
        }
        if ((*there)->event.id == pending.id) {
            co_return ZettaControlResult<Tail>(Tail{place, (*there)->event.digest});
        }
        auto moved = settled(
            co_await find_workspace_audit_tail(records, Tail{place, (*there)->event.digest}));
        if (!moved) {
            co_return failed<Tail>(moved.error());
        }
        tail = *moved;
    }
    co_return failed<Tail>(
        ZettaControlError{"conflict", "the workspace audit kept moving while appending"});
}

liquid::runtime::task<ZettaControlResult<bool>>
copy_central_audit_event(WorkspaceRecords& records, const audit::Event& event) {
    auto stored = settled(co_await records.put(
        collection, event.id, workspace_audit_lookup(records.workspace_id()),
        {{"event", audit::to_json(event)}}, RecordCondition{.if_absent = true}, 1,
        audit_instant_us(event.occurred_at)));
    if (!stored) {
        if (stored.error().code == "conflict") {
            co_return ZettaControlResult<bool>(false);
        }
        co_return failed<bool>(stored.error());
    }
    co_return ZettaControlResult<bool>(true);
}

liquid::runtime::task<ZettaControlResult<std::optional<WorkspaceAuditEvent>>>
get_copied_audit_event(WorkspaceRecords& records, std::string event_id) {
    using Found = std::optional<WorkspaceAuditEvent>;
    auto stored = settled(co_await records.get(collection, event_id));
    if (!stored) {
        if (stored.error().code == "invalid_input") {
            co_return ZettaControlResult<Found>(Found{}); // not an id lake could have made
        }
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto event = decoded(**stored, records.workspace_id());
    if (!event || event->sequence != 0 || event->event.id != event_id) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(event));
}

liquid::runtime::task<ZettaControlResult<std::vector<WorkspaceAuditEvent>>>
list_workspace_audit(WorkspaceRecords& records, WorkspaceAuditKeep keep, std::size_t newest,
                     std::int64_t since_us) {
    using Events = std::vector<WorkspaceAuditEvent>;
    const std::string workspace_id = records.workspace_id();
    auto stored = settled(co_await records.list(
        collection, workspace_audit_lookup(workspace_id), newest, since_us,
        [&keep, &workspace_id](const StoredRecord& record) {
            const auto event = decoded(record, workspace_id);
            return event && (!keep || keep(event->event));
        }));
    if (!stored) {
        co_return failed<Events>(stored.error());
    }
    Events events;
    events.reserve(stored->size());
    for (const StoredRecord& record : *stored) {
        if (auto event = decoded(record, workspace_id)) {
            events.push_back(std::move(*event));
        }
    }
    std::ranges::reverse(events);
    co_return ZettaControlResult<Events>(std::move(events));
}

liquid::runtime::task<ZettaControlResult<WorkspaceAuditVerification>>
verify_workspace_audit(WorkspaceRecords& records, Store& store) {
    auto all = settled(co_await list_workspace_audit(records, {}, 0));
    if (!all) {
        co_return failed<WorkspaceAuditVerification>(all.error());
    }
    WorkspaceAuditVerification result;
    const auto fault = [&result](std::string code, std::string message,
                                 const WorkspaceAuditEvent* event) {
        result.error_code = std::move(code);
        result.error_message = std::move(message);
        if (event) {
            result.failed_sequence = event->sequence;
            result.failed_event_id = event->event.id;
        }
    };
    std::map<std::int64_t, const WorkspaceAuditEvent*> chain;
    for (const WorkspaceAuditEvent& event : *all) {
        if (auto checked = audit::verify_event(store, event.event); !checked) {
            fault(checked.error().code, checked.error().message, &event);
            co_return ZettaControlResult<WorkspaceAuditVerification>(std::move(result));
        }
        if (event.sequence == 0) {
            ++result.copied;
            continue;
        }
        if (!chain.emplace(event.sequence, &event).second) {
            fault("invalid_chain", "two events claim one place in the chain", &event);
            co_return ZettaControlResult<WorkspaceAuditVerification>(std::move(result));
        }
    }
    std::string previous;
    std::int64_t expected = 1;
    for (const auto& [sequence, event] : chain) {
        if (sequence != expected) {
            result.error_code = "missing_event";
            result.error_message = "the workspace audit has no event at place " +
                                   std::to_string(expected) + " of " +
                                   std::to_string(chain.rbegin()->first);
            result.failed_sequence = expected;
            co_return ZettaControlResult<WorkspaceAuditVerification>(std::move(result));
        }
        if (event->event.previous_digest != previous) {
            fault("invalid_chain", "audit event predecessor digest does not match", event);
            co_return ZettaControlResult<WorkspaceAuditVerification>(std::move(result));
        }
        previous = event->event.digest;
        ++expected;
        ++result.chained;
        result.last_sequence = sequence;
    }
    result.verified = true;
    co_return ZettaControlResult<WorkspaceAuditVerification>(std::move(result));
}

} // namespace lake::control
