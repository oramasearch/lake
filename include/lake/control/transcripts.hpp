// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// A transcript a person added to a workspace (docs/api/transcripts.md, lake#561): what was said,
// from a recording, an uploaded audio or caption file, or pasted text. It lives in the workspace
// Zetta of its workspace, in the sealed collection `lake_transcripts`, found by its space, so
// every member of the space who may read the workspace reads it and another workspace of the
// space never sees it. The audio it was made from is never kept.
struct Transcript {
    std::string id;
    std::string customer_id;
    std::string title;
    std::string text;
    std::string source;            // recording, upload or text
    std::optional<double> seconds; // the audio's length, when known
    std::string language;          // ISO-639-1, or empty
    std::string recorded_at;       // when it was said, as the person gave it, or empty
    std::string created_by;        // the user id (or the operator's identity) that added it
    std::string created_at;        // "YYYY-MM-DDTHH:MM:SSZ"
    std::int64_t at_us = 0;
    std::int64_t lake_revision = 1;
};

struct TranscriptCodec {
    using Value = Transcript;
    static constexpr RecordCollection collection{"transcripts", 1};

    static std::string lookup(const Transcript& transcript);
    static nlohmann::json encode(const Transcript& transcript);
    static std::optional<Transcript> decode(const StoredRecord& record);
};

using Transcripts = RecordSet<TranscriptCodec>;

// The lookup key of a space's transcripts in a workspace.
std::string transcripts_lookup(const std::string& customer_id);

// Refuses a transcript without a space, a title or text, or with an unknown source.
std::optional<std::string> transcript_problem(const Transcript& transcript);
bool known_transcript_source(const std::string& source);

// Writes a new transcript under a new id; answers it as stored.
liquid::runtime::task<ZettaControlResult<Transcript>> record_transcript(WorkspaceRecords& records,
                                                                        Transcript transcript);

// Every transcript of the space in this workspace, newest first.
liquid::runtime::task<ZettaControlResult<std::vector<Transcript>>>
list_transcripts(WorkspaceRecords& records, std::string customer_id);

// One transcript of the space by id; nullopt when this workspace holds none.
liquid::runtime::task<ZettaControlResult<std::optional<Transcript>>>
get_transcript(WorkspaceRecords& records, std::string customer_id, std::string id);

// Changes a transcript's title and/or text with a write conditional on the revision it read,
// rereading on a conflict. `not_found` when this workspace holds no such transcript of the space.
liquid::runtime::task<ZettaControlResult<Transcript>>
edit_transcript(WorkspaceRecords& records,
                std::string customer_id,
                std::string id,
                std::optional<std::string> title,
                std::optional<std::string> text);

// Deletes a transcript at the revision read. `not_found` when this workspace holds none.
liquid::runtime::task<ZettaControlResult<void>>
remove_transcript(WorkspaceRecords& records, std::string customer_id, std::string id);

// One page of `transcripts` (newest first) whose title or text contains `query`, ignoring case
// (ASCII), starting after the transcript `cursor` names (empty: the first). `next_cursor` is set
// when more follow. `cursor_found` is false when `cursor` names no transcript of the list.
struct TranscriptPage {
    std::vector<Transcript> items;
    std::string next_cursor;
    bool cursor_found = true;
    // Every transcript the query keeps, on every page (lake#574: Home's figure reads it).
    std::size_t total = 0;
};
TranscriptPage page_transcripts(const std::vector<Transcript>& transcripts,
                                const std::string& query,
                                std::size_t limit,
                                const std::string& cursor);

// The first `length` bytes of `text` as one line, cut at a UTF-8 boundary, with "…" when cut.
std::string transcript_excerpt(const std::string& text, std::size_t length = 200);

// About `length` bytes of `text` as one line around the first match of `query` (ignoring case,
// ASCII), so a search shows why a transcript was found (lake#633): it starts at a word up to a
// third of `length` before the match, with a leading "…" when it does not start at the top, and
// ends as transcript_excerpt does. No query, or a query the text does not hold (the title
// matched), answers transcript_excerpt(text, length).
std::string transcript_excerpt_around(const std::string& text,
                                      const std::string& query,
                                      std::size_t length = 200);

} // namespace lake::control
