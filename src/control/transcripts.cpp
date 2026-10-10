#include "lake/control/transcripts.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> refused(std::string code, std::string message) {
    return std::unexpected(ZettaControlError{std::move(code), std::move(message)});
}

std::string folded(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Reads the transcript `id` of the space, or answers why not (`not_found` for another space's or
// a missing one).
struct Read {
    std::optional<Transcript> transcript;
    std::int64_t revision = 0;
};

liquid::runtime::task<ZettaControlResult<Read>>
read_own(WorkspaceRecords& records, const std::string& customer_id, const std::string& id) {
    auto got = settled(co_await records.get(TranscriptCodec::collection, id));
    if (!got && got.error().code != "invalid_input") {
        co_return ZettaControlResult<Read>(std::unexpected(got.error()));
    }
    Read read;
    if (got && *got) {
        read.transcript = TranscriptCodec::decode(**got);
        read.revision = (*got)->revision;
    }
    if (!read.transcript || read.transcript->customer_id != customer_id) {
        co_return refused<Read>("not_found", "no such transcript in this workspace");
    }
    co_return ZettaControlResult<Read>(std::move(read));
}

} // namespace

bool known_transcript_source(const std::string& source) {
    return source == "recording" || source == "upload" || source == "text";
}

std::string transcripts_lookup(const std::string& customer_id) {
    return "customer:" + customer_id;
}

std::optional<std::string> transcript_problem(const Transcript& transcript) {
    if (transcript.customer_id.empty() || transcript.title.empty() || transcript.text.empty() ||
        !known_transcript_source(transcript.source)) {
        return "transcript is invalid";
    }
    return std::nullopt;
}

std::string TranscriptCodec::lookup(const Transcript& transcript) {
    return transcripts_lookup(transcript.customer_id);
}

nlohmann::json TranscriptCodec::encode(const Transcript& transcript) {
    nlohmann::json body{{"customer_id", transcript.customer_id},
                        {"title", transcript.title},
                        {"text", transcript.text},
                        {"source", transcript.source},
                        {"language", transcript.language},
                        {"recorded_at", transcript.recorded_at},
                        {"created_by", transcript.created_by}};
    if (transcript.seconds) {
        body["seconds"] = *transcript.seconds;
    }
    return body;
}

std::optional<Transcript> TranscriptCodec::decode(const StoredRecord& record) {
    // Version 1 is the only body schema so far; a later version decodes its predecessors here.
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    Transcript transcript;
    transcript.id = record.id;
    transcript.customer_id = body.value("customer_id", std::string());
    transcript.title = body.value("title", std::string());
    transcript.text = body.value("text", std::string());
    transcript.source = body.value("source", std::string());
    if (body.contains("seconds") && body.at("seconds").is_number()) {
        transcript.seconds = body.at("seconds").get<double>();
    }
    transcript.language = body.value("language", std::string());
    transcript.recorded_at = body.value("recorded_at", std::string());
    transcript.created_by = body.value("created_by", std::string());
    transcript.created_at = record_instant(record.at_us);
    transcript.at_us = record.at_us;
    transcript.lake_revision = record.lake_revision;
    if (transcript_problem(transcript)) {
        return std::nullopt;
    }
    return transcript;
}

liquid::runtime::task<ZettaControlResult<Transcript>> record_transcript(WorkspaceRecords& records,
                                                                        Transcript transcript) {
    if (auto problem = transcript_problem(transcript)) {
        co_return refused<Transcript>("invalid_input", *problem);
    }
    Transcripts set(records);
    co_return settled(co_await set.append(std::move(transcript)));
}

liquid::runtime::task<ZettaControlResult<std::vector<Transcript>>>
list_transcripts(WorkspaceRecords& records, std::string customer_id) {
    Transcripts set(records);
    auto listed = settled(co_await set.list(transcripts_lookup(customer_id)));
    if (!listed) {
        co_return ZettaControlResult<std::vector<Transcript>>(std::unexpected(listed.error()));
    }
    std::erase_if(*listed, [&](const Transcript& row) { return row.customer_id != customer_id; });
    std::ranges::sort(*listed, [](const Transcript& a, const Transcript& b) {
        return a.at_us != b.at_us ? a.at_us > b.at_us : a.id > b.id;
    });
    co_return ZettaControlResult<std::vector<Transcript>>(std::move(*listed));
}

liquid::runtime::task<ZettaControlResult<std::optional<Transcript>>>
get_transcript(WorkspaceRecords& records, std::string customer_id, std::string id) {
    using Found = std::optional<Transcript>;
    auto read = settled(co_await read_own(records, customer_id, id));
    if (!read) {
        if (read.error().code == "not_found") {
            co_return ZettaControlResult<Found>(Found{});
        }
        co_return ZettaControlResult<Found>(std::unexpected(read.error()));
    }
    co_return ZettaControlResult<Found>(std::move(read->transcript));
}

liquid::runtime::task<ZettaControlResult<Transcript>>
edit_transcript(WorkspaceRecords& records,
                std::string customer_id,
                std::string id,
                std::optional<std::string> title,
                std::optional<std::string> text) {
    ZettaControlError last{"conflict", "the transcript kept changing"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto read = settled(co_await read_own(records, customer_id, id));
        if (!read) {
            co_return ZettaControlResult<Transcript>(std::unexpected(read.error()));
        }
        // Its space never changes, so the write below keeps its lookup.
        Transcript next = *read->transcript;
        if (title) {
            next.title = *title;
        }
        if (text) {
            next.text = *text;
        }
        if (auto problem = transcript_problem(next)) {
            co_return refused<Transcript>("invalid_input", *problem);
        }
        auto written = settled(co_await records.put(TranscriptCodec::collection,
                                                    id,
                                                    TranscriptCodec::lookup(next),
                                                    TranscriptCodec::encode(next),
                                                    RecordCondition{.if_revision = read->revision},
                                                    next.lake_revision + 1,
                                                    next.at_us));
        if (written) {
            auto decoded = TranscriptCodec::decode(*written);
            if (!decoded) {
                co_return refused<Transcript>("store_error", "the stored record did not decode");
            }
            co_return ZettaControlResult<Transcript>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return ZettaControlResult<Transcript>(std::unexpected(written.error()));
        }
        last = written.error();
    }
    co_return ZettaControlResult<Transcript>(std::unexpected(last));
}

liquid::runtime::task<ZettaControlResult<void>>
remove_transcript(WorkspaceRecords& records, std::string customer_id, std::string id) {
    ZettaControlError last{"conflict", "the transcript kept changing"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto read = settled(co_await read_own(records, customer_id, id));
        if (!read) {
            co_return ZettaControlResult<void>(std::unexpected(read.error()));
        }
        auto removed =
            settled(co_await records.remove(TranscriptCodec::collection, id, read->revision));
        if (removed || removed.error().code == "not_found") {
            co_return ZettaControlResult<void>();
        }
        if (removed.error().code != "conflict") {
            co_return ZettaControlResult<void>(std::unexpected(removed.error()));
        }
        last = removed.error();
    }
    co_return ZettaControlResult<void>(std::unexpected(last));
}

TranscriptPage page_transcripts(const std::vector<Transcript>& transcripts,
                                const std::string& query,
                                std::size_t limit,
                                const std::string& cursor) {
    TranscriptPage page;
    const std::string needle = folded(query);
    std::size_t start = 0;
    if (!cursor.empty()) {
        const auto found = std::ranges::find(transcripts, cursor, &Transcript::id);
        if (found == transcripts.end()) {
            page.cursor_found = false;
            return page;
        }
        start = static_cast<std::size_t>(found - transcripts.begin()) + 1;
    }
    for (std::size_t index = start; index < transcripts.size(); ++index) {
        const Transcript& transcript = transcripts[index];
        if (!needle.empty() && folded(transcript.title).find(needle) == std::string::npos &&
            folded(transcript.text).find(needle) == std::string::npos) {
            continue;
        }
        if (page.items.size() == limit) {
            page.next_cursor = page.items.back().id;
            break;
        }
        page.items.push_back(transcript);
    }
    return page;
}

std::string transcript_excerpt(const std::string& text, std::size_t length) {
    std::string line;
    line.reserve(std::min(text.size(), length + 4));
    bool cut = false;
    bool gap = false;
    for (const char c : text) {
        if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
            gap = !line.empty();
            continue;
        }
        if (gap) {
            line.push_back(' ');
            gap = false;
        }
        if (line.size() >= length &&
            (static_cast<unsigned char>(c) & 0xC0) != 0x80) { // not inside a UTF-8 sequence
            cut = true;
            break;
        }
        line.push_back(c);
    }
    if (cut) {
        line += "…";
    }
    return line;
}

} // namespace lake::control
