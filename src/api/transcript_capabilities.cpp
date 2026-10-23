// Speech-to-text and workspace transcripts (docs/api/transcripts.md, lake#561): `transcribe`
// turns audio into text through the injected Transcriber (OpenAI with lake's key), and the five
// transcript capabilities keep what was said as sealed `lake_transcripts` records in the
// workspace Zetta. The audio is held only while it is sent; it is never stored or logged.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "lake/api/capabilities.hpp"
#include "lake/api/transcription.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/control/store.hpp"
#include "lake/control/transcripts.hpp"
#include "workspace_records_api.hpp"

namespace lake::api {
namespace {

using json = nlohmann::json;

constexpr std::size_t default_list_limit = 50;

std::expected<std::string, ApiError> customer_object(const json& input, ApiContext& context) {
    if (!context.store) {
        return std::unexpected(
            ApiError{"store_unavailable", "no control-plane store is configured", {}});
    }
    const auto object = context.store->find_policy_object("customer", input.at("customer_id"));
    if (!object) {
        return std::unexpected(
            ApiError{object.error().code, object.error().message, "/customer_id"});
    }
    return object->id;
}

// Read on the space, for list_transcripts, which authorizes itself as every list_ read does.
std::optional<ApiResult> authorize_space_read(const json& input, ApiContext& context) {
    const auto object = customer_object(input, context);
    if (!object) {
        return ApiResult::failure(object.error().code, object.error().message, object.error().path);
    }
    if (context.acting_identity == "admin") {
        return std::nullopt;
    }
    const auto decision = control::authorize(
        *context.store, context.acting_identity, *object, "read", context.authorization_cache);
    if (!decision) {
        return ApiResult::failure(decision.error().code, decision.error().message);
    }
    if (!decision->allowed) {
        return ApiResult::failure("unauthorized", "principal is not allowed to read the customer");
    }
    return std::nullopt;
}

json string_schema() {
    return {{"type", "string"}};
}

json workspace_input(json properties, json required) {
    properties["customer_id"] = string_schema();
    properties["workspace_id"] = string_schema();
    required.insert(required.begin(), "workspace_id");
    required.insert(required.begin(), "customer_id");
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json nullable(std::string_view type) {
    return {{"type", json::array({type, "null"})}};
}

json transcript_schema() {
    return {{"type", "object"},
            {"properties",
             {{"id", string_schema()},
              {"title", string_schema()},
              {"text", string_schema()},
              {"source", {{"type", "string"}, {"enum", {"recording", "upload", "text"}}}},
              {"seconds", nullable("number")},
              {"language", nullable("string")},
              {"recorded_at", nullable("string")},
              {"created_at", string_schema()},
              {"created_by", string_schema()},
              {"created_by_name", nullable("string")}}},
            {"required",
             json::array({"id",
                          "title",
                          "text",
                          "source",
                          "seconds",
                          "language",
                          "recorded_at",
                          "created_at",
                          "created_by",
                          "created_by_name"})},
            {"additionalProperties", false}};
}

json or_null(const std::string& value) {
    return value.empty() ? json(nullptr) : json(value);
}

json seconds_json(const std::optional<double>& seconds) {
    return seconds ? json(*seconds) : json(nullptr);
}

// The author's name (lake#579): the display name of the user `created_by` names, else their
// email, read from the control store, where identity lives. An author lake has no user for (an
// operator credential) answers null, and a page then names no one rather than an id. `names_`
// keeps each author's answer for one call, so a list reads each author once.
class AuthorNames {
public:
    explicit AuthorNames(ApiContext& context) : context_(context) {}

    json operator()(const std::string& user_id) {
        if (const auto known = names_.find(user_id); known != names_.end()) {
            return known->second;
        }
        json name = nullptr;
        if (context_.store && !user_id.empty()) {
            if (const auto user = context_.store->get_user(user_id); user) {
                if (!user->display_name.empty()) {
                    name = user->display_name;
                } else if (!user->email.empty()) {
                    name = user->email;
                }
            }
        }
        names_.emplace(user_id, name);
        return name;
    }

private:
    ApiContext& context_;
    std::map<std::string, json> names_;
};

json transcript_json(const control::Transcript& transcript, AuthorNames& names) {
    return {{"id", transcript.id},
            {"title", transcript.title},
            {"text", transcript.text},
            {"source", transcript.source},
            {"seconds", seconds_json(transcript.seconds)},
            {"language", or_null(transcript.language)},
            {"recorded_at", or_null(transcript.recorded_at)},
            {"created_at", transcript.created_at},
            {"created_by", transcript.created_by},
            {"created_by_name", names(transcript.created_by)}};
}

// A row of list_transcripts. With a query, its excerpt is the text around the match (lake#633).
json summary_json(const control::Transcript& transcript,
                  AuthorNames& names,
                  const std::string& query = std::string()) {
    return {{"id", transcript.id},
            {"title", transcript.title},
            {"excerpt", control::transcript_excerpt_around(transcript.text, query)},
            {"source", transcript.source},
            {"seconds", seconds_json(transcript.seconds)},
            {"created_at", transcript.created_at},
            {"created_by", transcript.created_by},
            {"created_by_name", names(transcript.created_by)}};
}

ApiResult failed(const control::ZettaControlError& error) {
    return ApiResult::failure(error.code, error.message, error.path);
}

std::string trimmed(std::string text) {
    const auto blank = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && blank(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    const auto first = std::ranges::find_if_not(text, blank);
    text.erase(text.begin(), first);
    return text;
}

std::optional<ApiResult> title_problem(const std::string& title) {
    if (title.empty()) {
        return ApiResult::failure("invalid_input", "a transcript needs a title", "/title");
    }
    if (title.size() > max_transcript_title_bytes) {
        return ApiResult::failure("invalid_input", "the title is longer than 300 bytes", "/title");
    }
    return std::nullopt;
}

// The text a transcript keeps: captions cleaned to what was said, then trimmed.
std::expected<std::string, ApiResult> transcript_text(const std::string& text) {
    std::string kept = trimmed(clean_caption_text(text));
    if (kept.empty()) {
        return std::unexpected(
            ApiResult::failure("invalid_input", "the transcript text is empty", "/text"));
    }
    if (kept.size() > max_transcript_text_bytes) {
        return std::unexpected(ApiResult::failure(
            "size_invalid", "the transcript text is longer than 400,000 bytes", "/text"));
    }
    return kept;
}

std::optional<ApiResult> language_problem(const std::string& language) {
    const bool valid = language.size() <= 16 && std::ranges::all_of(language, [](unsigned char c) {
                           return std::isalpha(c) != 0 || c == '-';
                       });
    if (!valid) {
        return ApiResult::failure(
            "invalid_input", "language is an ISO-639-1 code such as \"en\"", "/language");
    }
    return std::nullopt;
}

std::optional<ApiResult> recorded_at_problem(const std::string& recorded_at) {
    if (recorded_at.empty()) {
        return std::nullopt;
    }
    const auto digit = [&](std::size_t index) {
        return std::isdigit(static_cast<unsigned char>(recorded_at[index])) != 0;
    };
    const bool date = recorded_at.size() >= 10 && recorded_at.size() <= 64 && digit(0) &&
                      digit(1) && digit(2) && digit(3) && recorded_at[4] == '-' && digit(5) &&
                      digit(6) && recorded_at[7] == '-' && digit(8) && digit(9);
    if (!date) {
        return ApiResult::failure(
            "invalid_input", "recorded_at is an RFC 3339 date or instant", "/recorded_at");
    }
    return std::nullopt;
}

// What a Transcriber answered, carried out of the blocking pool (a liquid task already wraps its
// value in an expected, so the value is not one).
struct Transcribed {
    std::optional<Transcription> transcription;
    ApiError error;
};

// What transcribe_audio heard, or why not (for the same reason, not an expected).
struct Heard {
    std::optional<Transcription> transcription;
    ApiResult failure;
};

Heard refused_audio(ApiResult failure) {
    return {std::nullopt, std::move(failure)};
}

// Decodes `audio_base64` and sends it to the transcriber off the server thread. The decoded
// bytes live only in this call.
liquid::runtime::task<Heard> transcribe_audio(ApiContext& context,
                                              std::string audio_base64,
                                              std::string name,
                                              std::string language,
                                              std::string path,
                                              std::string name_path) {
    if (!context.transcriber) {
        co_return refused_audio(
            ApiResult::failure("not_supported", "Speech-to-text is not configured on this Lake."));
    }
    // A text file is refused here, not sent to be called corrupt (lake#594).
    if (std::string reason = not_audio_reason(name); !reason.empty()) {
        co_return refused_audio(
            ApiResult::failure("invalid_input", std::move(reason), std::move(name_path)));
    }
    auto audio = decode_audio(audio_base64, path);
    if (!audio) {
        co_return refused_audio(
            ApiResult::failure(audio.error().code, audio.error().message, audio.error().path));
    }
    // Its length, measured before the bytes go: billed when the model reports none (lake#658).
    const std::optional<double> measured = audio_duration_seconds(audio->bytes);
    TranscriptionRequest request{.file_name = audio_file_name(name, audio->mime),
                                 .mime = {},
                                 .audio = std::move(audio->bytes),
                                 .language = language};
    request.mime = audio_mime(request.file_name);
    auto ran = co_await run_blocking(
        [transcriber = context.transcriber, request = std::move(request)]() -> Transcribed {
            auto answered = transcriber(request);
            if (!answered) {
                return {std::nullopt, std::move(answered.error())};
            }
            return {std::move(*answered), {}};
        });
    if (!ran) {
        co_return refused_audio(ApiResult::failure("busy", ran.error().message));
    }
    if (!ran->transcription) {
        const std::string code = ran->error.code.empty() ? "engine_error" : ran->error.code;
        co_return refused_audio(ApiResult::failure(code, ran->error.message));
    }
    if (!ran->transcription->seconds) {
        ran->transcription->seconds = measured;
    }
    co_return Heard{std::move(ran->transcription), {}};
}

// The transcription, or the failure to answer with.
std::expected<Transcription, ApiResult> heard(std::expected<Heard, liquid::error> awaited) {
    if (!awaited) {
        return std::unexpected(ApiResult::failure("store_error", awaited.error().message));
    }
    if (!awaited->transcription) {
        return std::unexpected(std::move(awaited->failure));
    }
    return std::move(*awaited->transcription);
}

// Speech-to-text usage of one heard recording, in its workspace (lake#564, lake#613, lake#658):
// its audio seconds, rounded up, as `speech` of `kind` (transcript or dictation): what the model
// reports (whisper-1), else what lake measured from the recording. Speech is billed by the minute
// only: tokens a model reports (gpt-4o-transcribe) go to the audit as detail, never to
// `model_tokens`. A recording whose length could not be read counts nothing and says so in the
// audit. Only adds to the in-memory recorder.
void record_speech(ApiContext& context,
                   const std::string& workspace_id,
                   const Transcription& done,
                   std::string_view kind) {
    if (done.input_tokens || done.output_tokens) {
        context.audit_details["speech_tokens"] = {{"input", done.input_tokens.value_or(0)},
                                                  {"output", done.output_tokens.value_or(0)}};
    }
    if (!done.seconds || *done.seconds <= 0) {
        context.audit_details["speech_seconds"] = nullptr; // not measured
        return;
    }
    const auto seconds = static_cast<std::uint64_t>(std::ceil(*done.seconds));
    context.audit_details["speech_seconds"] = seconds;
    if (context.usage_recorder != nullptr && !workspace_id.empty()) {
        context.usage_recorder->add(workspace_id, "speech", seconds, {{"kind", std::string(kind)}});
    }
}

liquid::runtime::task<ApiResult> transcribe(const json& input, ApiContext& context) {
    const std::string language = input.value("language", std::string());
    if (auto refused = language_problem(language)) {
        co_return *refused;
    }
    auto done = heard(co_await transcribe_audio(
        context, input.at("audio_base64"), input.at("name"), language, "/audio_base64", "/name"));
    if (!done) {
        co_return done.error();
    }
    // The audio was heard: its seconds, or the tokens a model reports instead, are the usage of
    // the workspace it was dictated in (lake#613, lake#658), counted off the request path.
    record_speech(context, input.at("workspace_id"), *done, "dictation");
    json out{{"text", done->text}, {"model", done->model}};
    if (done->seconds) {
        out["seconds"] = *done->seconds;
    }
    context.audit_details["model"] = done->model;
    co_return ApiResult::success(std::move(out));
}

liquid::runtime::task<ApiResult> create_transcript(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    const std::string customer_id = input.at("customer_id");
    const std::string workspace_id = input.at("workspace_id");
    control::Transcript transcript;
    transcript.customer_id = customer_id;
    transcript.title = trimmed(input.at("title"));
    transcript.source = input.at("source");
    transcript.language = input.value("language", std::string());
    transcript.recorded_at = input.value("recorded_at", std::string());
    transcript.created_by =
        context.acting_user_id.empty() ? context.acting_identity : context.acting_user_id;
    if (auto refused = title_problem(transcript.title)) {
        co_return *refused;
    }
    if (auto refused = language_problem(transcript.language)) {
        co_return *refused;
    }
    if (auto refused = recorded_at_problem(transcript.recorded_at)) {
        co_return *refused;
    }
    const std::string text = input.value("text", std::string());
    const std::string audio = input.value("audio_base64", std::string());
    auto records = workspace_records(context, customer_id, workspace_id);
    if (!records) {
        co_return failed(records.error());
    }
    if (!trimmed(text).empty()) {
        auto kept = transcript_text(text);
        if (!kept) {
            co_return kept.error();
        }
        transcript.text = std::move(*kept);
    } else if (!audio.empty()) {
        auto done = heard(co_await transcribe_audio(context,
                                                    audio,
                                                    input.value("audio_name", std::string()),
                                                    transcript.language,
                                                    "/audio_base64",
                                                    "/audio_name"));
        if (!done) {
            co_return done.error();
        }
        // The audio was heard, whatever happens to the transcript: its seconds are the
        // workspace's speech-to-text usage (lake#564), counted off the request path.
        record_speech(context, workspace_id, *done, "transcript");
        transcript.text = trimmed(std::move(done->text));
        transcript.seconds = done->seconds;
        if (transcript.text.empty()) {
            co_return ApiResult::failure(
                "invalid_input", "no speech was heard in the audio", "/audio_base64");
        }
        if (transcript.text.size() > max_transcript_text_bytes) {
            co_return ApiResult::failure("size_invalid",
                                         "the transcript text is longer than 400,000 bytes",
                                         "/audio_base64");
        }
    } else {
        co_return ApiResult::failure(
            "invalid_input", "send the transcript's text or its audio", "/text");
    }
    auto stored =
        control::settled(co_await control::record_transcript(*records, std::move(transcript)));
    if (!stored) {
        co_return failed(stored.error());
    }
    context.audit_details["transcript_id"] = stored->id;
    context.audit_details["source"] = stored->source;
    AuthorNames names(context);
    co_return ApiResult::success(transcript_json(*stored, names));
}

liquid::runtime::task<ApiResult> list_transcripts(const json& input, ApiContext& context) {
    if (!context.store) {
        co_return ApiResult::failure("store_unavailable", "workspaces are not configured");
    }
    if (auto refused = authorize_space_read(input, context)) {
        co_return *refused;
    }
    const std::string customer_id = input.at("customer_id");
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    const std::size_t limit = input.value("limit", default_list_limit);
    const std::string cursor = input.value("cursor", std::string());
    auto listed = control::settled(co_await control::list_transcripts(*records, customer_id));
    if (!listed) {
        co_return failed(listed.error());
    }
    const std::string query = input.value("q", std::string());
    const control::TranscriptPage page = control::page_transcripts(*listed, query, limit, cursor);
    if (!page.cursor_found) {
        co_return ApiResult::failure(
            "invalid_input", "cursor names no transcript of this list", "/cursor");
    }
    json items = json::array();
    AuthorNames names(context);
    for (const control::Transcript& transcript : page.items) {
        items.push_back(summary_json(transcript, names, query));
    }
    json out{{"transcripts", std::move(items)}, {"total", page.total}};
    if (!page.next_cursor.empty()) {
        out["next_cursor"] = page.next_cursor;
    }
    co_return ApiResult::success(std::move(out));
}

liquid::runtime::task<ApiResult> get_transcript(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    auto found =
        control::settled(co_await control::get_transcript(*records, customer_id, input.at("id")));
    if (!found) {
        co_return failed(found.error());
    }
    if (!*found) {
        co_return ApiResult::failure("not_found", "no such transcript in this workspace", "/id");
    }
    AuthorNames names(context);
    co_return ApiResult::success(transcript_json(**found, names));
}

liquid::runtime::task<ApiResult> update_transcript(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    std::optional<std::string> title;
    std::optional<std::string> text;
    if (input.contains("title") && !input.at("title").is_null()) {
        title = trimmed(input.at("title"));
        if (auto refused = title_problem(*title)) {
            co_return *refused;
        }
    }
    if (input.contains("text") && !input.at("text").is_null()) {
        auto kept = transcript_text(input.at("text"));
        if (!kept) {
            co_return kept.error();
        }
        text = std::move(*kept);
    }
    if (!title && !text) {
        co_return ApiResult::failure("invalid_input", "send a new title or text", "/title");
    }
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    context.audit_details["transcript_id"] = input.at("id");
    auto edited = control::settled(co_await control::edit_transcript(
        *records, customer_id, input.at("id"), std::move(title), std::move(text)));
    if (!edited) {
        if (edited.error().code == "not_found") {
            co_return ApiResult::failure(
                "not_found", "no such transcript in this workspace", "/id");
        }
        co_return failed(edited.error());
    }
    AuthorNames names(context);
    co_return ApiResult::success(transcript_json(*edited, names));
}

liquid::runtime::task<ApiResult> delete_transcript(const json& input, ApiContext& context) {
    const std::string customer_id = input.at("customer_id");
    auto records = workspace_records(context, customer_id, input.at("workspace_id"));
    if (!records) {
        co_return failed(records.error());
    }
    context.audit_details["transcript_id"] = input.at("id");
    auto removed = control::settled(
        co_await control::remove_transcript(*records, customer_id, input.at("id")));
    if (!removed) {
        if (removed.error().code == "not_found") {
            co_return ApiResult::failure(
                "not_found", "no such transcript in this workspace", "/id");
        }
        co_return failed(removed.error());
    }
    co_return ApiResult::success({{"deleted", true}});
}

} // namespace

void register_transcript_capabilities(Registry& registry) {
    const json audio{{"type", "string"},
                     {"contentEncoding", "base64"},
                     {"description",
                      "the audio as padded base64, or a data: URL; at most 25 MiB "
                      "decoded (webm, mp4, m4a, mp3, wav, ogg or flac)"}};
    const json language{{"type", "string"},
                        {"description", "ISO-639-1 code of the spoken language; omit to detect"}};
    registry.add(Capability{
        .name = "transcribe",
        .title = "Transcribe audio",
        .description =
            "Turn a recording into text with speech-to-text (OpenAI, through Lake's own key), "
            "dictated in a workspace: its audio minutes are that workspace's speech-to-text "
            "usage. Nothing is stored and the audio is not kept: create_transcript saves a "
            "transcript. not_supported when this Lake has no speech-to-text configured.",
        .input_schema = workspace_input(
            {{"name",
              {{"type", "string"},
               {"description",
                "the file name, such as memo.m4a; its extension tells the audio's format"}}},
             {"audio_base64", audio},
             {"language", language}},
            json::array({"name", "audio_base64"})),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"text", string_schema()},
                            {"seconds", {{"type", "number"}}},
                            {"model", string_schema()}}},
                          {"required", json::array({"text", "model"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .max_body_bytes = maximum_capability_body_bytes,
        // Whoever may write the workspace may dictate in it: its usage is the workspace's.
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = transcribe});
    registry.add(Capability{
        .name = "create_transcript",
        .title = "Add a transcript",
        .description =
            "Add a transcript to a workspace: pasted text, an uploaded .txt, .vtt or .srt "
            "(captions are cleaned to what was said), or audio that is transcribed first. Either "
            "`text` or `audio_base64` is required. It is kept as a sealed record in the "
            "workspace Zetta, searchable with list_transcripts; the audio is never kept.",
        .input_schema = workspace_input(
            {{"title", string_schema()},
             {"text", {{"type", "string"}, {"description", "the transcript, or captions"}}},
             {"audio_base64", audio},
             {"audio_name", {{"type", "string"}, {"description", "the audio's file name"}}},
             {"language", language},
             {"source", {{"type", "string"}, {"enum", {"recording", "upload", "text"}}}},
             {"recorded_at", {{"type", "string"}, {"description", "when it was said, RFC 3339"}}}},
            json::array({"title", "source"})),
        .output_schema = transcript_schema(),
        .kind = CapabilityKind::Action,
        .max_body_bytes = maximum_capability_body_bytes,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = create_transcript});
    registry.add(Capability{
        .name = "list_transcripts",
        .title = "List Transcripts",
        .description = "The workspace's transcripts, newest first, each with an excerpt and its "
                       "author's name. `q` keeps those whose title or text contains it, ignoring "
                       "case, and its excerpt is then the text around the match; `cursor` is "
                       "the `next_cursor` of the page before; `total` counts every transcript "
                       "`q` keeps, on every page.",
        .input_schema =
            workspace_input({{"q", string_schema()},
                             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
                             {"cursor", string_schema()}},
                            json::array()),
        .output_schema = {{"type", "object"},
                          {"properties",
                           {{"transcripts",
                             {{"type", "array"},
                              {"items",
                               {{"type", "object"},
                                {"properties",
                                 {{"id", string_schema()},
                                  {"title", string_schema()},
                                  {"excerpt", string_schema()},
                                  {"source", string_schema()},
                                  {"seconds", nullable("number")},
                                  {"created_at", string_schema()},
                                  {"created_by", string_schema()},
                                  {"created_by_name", nullable("string")}}},
                                {"required",
                                 json::array({"id",
                                              "title",
                                              "excerpt",
                                              "source",
                                              "seconds",
                                              "created_at",
                                              "created_by",
                                              "created_by_name"})},
                                {"additionalProperties", false}}}}},
                            {"total", {{"type", "integer"}, {"minimum", 0}}},
                            {"next_cursor", string_schema()}}},
                          {"required", json::array({"transcripts", "total"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Read,
        .object = "customer",
        .action = "read",
        .resolve_object = customer_object,
        .authorizes_candidates = true,
        .async_handler = list_transcripts});
    registry.add(
        Capability{.name = "get_transcript",
                   .title = "Read a Transcript",
                   .description = "One transcript of the workspace with its full text.",
                   .input_schema = workspace_input({{"id", string_schema()}}, json::array({"id"})),
                   .output_schema = transcript_schema(),
                   .kind = CapabilityKind::Read,
                   .object = "customer",
                   .action = "read",
                   .resolve_object = customer_object,
                   .async_handler = get_transcript});
    registry.add(Capability{
        .name = "update_transcript",
        .title = "Edit a transcript",
        .description = "Change a transcript's title or text (or both).",
        .input_schema = workspace_input(
            {{"id", string_schema()}, {"title", string_schema()}, {"text", string_schema()}},
            json::array({"id"})),
        .output_schema = transcript_schema(),
        .kind = CapabilityKind::Action,
        .max_body_bytes = maximum_capability_body_bytes,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = update_transcript});
    registry.add(Capability{
        .name = "delete_transcript",
        .title = "Delete a transcript",
        .description = "Delete a transcript from the workspace; it cannot be restored.",
        .input_schema = workspace_input({{"id", string_schema()}}, json::array({"id"})),
        .output_schema = {{"type", "object"},
                          {"properties", {{"deleted", {{"type", "boolean"}, {"const", true}}}}},
                          {"required", json::array({"deleted"})},
                          {"additionalProperties", false}},
        .kind = CapabilityKind::Action,
        .object = "customer",
        .action = "write",
        .resolve_object = customer_object,
        .async_handler = delete_transcript});
}

} // namespace lake::api
