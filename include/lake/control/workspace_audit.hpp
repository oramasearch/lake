// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>

#include "lake/audit/audit.hpp"
#include "lake/control/workspace_records.hpp"

namespace lake::control {

// A workspace's audit (lake#317, docs/api/audit.md#workspace-audit): the signed
// events about that workspace's data, in the sealed collection `lake_audit_events` of its own
// Zetta. Every record is found by one lookup (`workspace:<id>`) and ordered by the event's
// instant, so one listing reads a workspace's audit newest first.
//
// Two kinds of record:
//
// - **Chained events.** Each event lake writes there is an `audit_event/1` whose
//   `previous_digest` is the digest of the workspace's previous chained event, so the workspace
//   has its own hash chain, verified like the central one. The n-th event's key is the blind
//   index of `sequence:<n>`, written `if_absent`: claiming a place in the chain is that one
//   conditional write, so two lake replicas never fork it, and a write retried after a lost
//   answer finds its own event at its place.
// - **Copied events.** An event recorded in the central log before the audit split and copied by
//   `copy_workspace_records` keeps its id, signature and central predecessor, under its id. The
//   central chain holds its place; it is checked alone (audit::verify_event).
//
// Retention: kept until the workspace is deleted (`retention_ms` 0), which is never shorter than
// any space's audit retention (lake#318).
struct WorkspaceAuditCodec {
    static constexpr RecordCollection collection{"audit_events", 1, 0};
};

struct WorkspaceAuditEvent {
    audit::Event event;
    // Its place in the workspace's chain, from 1; 0 for a copied central event.
    std::int64_t sequence = 0;
    std::string workspace_id;
};

// The chain's last event: its place and digest; {0, ""} for an empty chain.
struct WorkspaceAuditTail {
    std::int64_t sequence = 0;
    std::string digest;
};

std::string workspace_audit_lookup(const std::string& workspace_id);

// Microseconds since the Unix epoch of an audit instant in UTC ("YYYY-MM-DDTHH:MM:SS[.ffffff]Z",
// as audit events carry it); 0 when it does not parse.
std::int64_t audit_instant_us(std::string_view instant);

// The chained event at `sequence`, or nullopt when that place is empty here.
liquid::runtime::task<ZettaControlResult<std::optional<WorkspaceAuditEvent>>>
get_chained_audit_event(WorkspaceRecords& records, std::int64_t sequence);

// The chain's last event, searched forward from `known` (a place known to be taken, or {0, ""}):
// doubling probes, then a binary search between the last place taken and the first empty one.
liquid::runtime::task<ZettaControlResult<WorkspaceAuditTail>>
find_workspace_audit_tail(WorkspaceRecords& records, WorkspaceAuditTail known = {});

// Writes `event` at `sequence`, only if that place is empty (`conflict` otherwise).
liquid::runtime::task<ZettaControlResult<void>>
put_chained_audit_event(WorkspaceRecords& records,
                        std::int64_t sequence,
                        const audit::Event& event);

// Signs `pending` after the chain's last event and writes it at the next place: the one way an
// event enters a workspace's chain (the AuditRecorder's flush, the platform sync). `tail` is a
// hint (nullopt: search from the start); a place another writer took first is read, the tail is
// searched again from it, and the event is signed again after it, up to eight times. A place
// already holding this event (a write whose answer was lost) counts as written. Answers the new
// tail. `conflicts`, when given, counts the places another writer took first.
liquid::runtime::task<ZettaControlResult<WorkspaceAuditTail>>
append_workspace_audit_event(WorkspaceRecords& records,
                             Store& store,
                             const audit::Identity& signer,
                             const audit::PendingEvent& pending,
                             const audit::IdentityOptions& options,
                             std::optional<WorkspaceAuditTail> tail = std::nullopt,
                             std::uint64_t* conflicts = nullptr);

// Copies a central event as it is, under its id, `if_absent`. True when written, false when it is
// already there.
liquid::runtime::task<ZettaControlResult<bool>>
copy_central_audit_event(WorkspaceRecords& records, const audit::Event& event);

// A copied central event by id, or nullopt.
liquid::runtime::task<ZettaControlResult<std::optional<WorkspaceAuditEvent>>>
get_copied_audit_event(WorkspaceRecords& records, std::string event_id);

// The workspace's events newest first: at most `newest` that `keep` accepts (0: every one), none
// older than `since_us` (0: no bound).
using WorkspaceAuditKeep = std::function<bool(const audit::Event&)>;
liquid::runtime::task<ZettaControlResult<std::vector<WorkspaceAuditEvent>>>
list_workspace_audit(WorkspaceRecords& records, WorkspaceAuditKeep keep, std::size_t newest,
                     std::int64_t since_us = 0);

// What verify found: the chained events checked (first and last place), the copied events
// checked alone, and the first fault, if any.
struct WorkspaceAuditVerification {
    bool verified = false;
    std::size_t chained = 0;
    std::size_t copied = 0;
    std::int64_t last_sequence = 0;
    std::string error_code;    // invalid_chain, invalid_signature, invalid_digest, missing_event
    std::string error_message;
    std::int64_t failed_sequence = 0;
    std::string failed_event_id;
};

// Reads the whole audit of the workspace and checks it: the chained events form one unbroken
// chain from place 1 (each event's predecessor is the previous place's digest, and no place is
// empty below the last), every event's signature verifies against lake's identity history, and
// every digest matches its event. A record lake did not seal for this workspace does not open
// and so counts as missing.
liquid::runtime::task<ZettaControlResult<WorkspaceAuditVerification>>
verify_workspace_audit(WorkspaceRecords& records, Store& store);

} // namespace lake::control
