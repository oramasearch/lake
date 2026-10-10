#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lake/api/registry.hpp"

namespace lake::api {

// Speech-to-text and the text a transcript keeps (docs/api/transcripts.md, lake#561). Everything
// here is pure: the HTTP exchange is the server's outbound client, injected as a TranscriptPost.

// The most audio one call takes, decoded: OpenAI's own limit for a transcription upload.
inline constexpr std::size_t max_transcription_audio_bytes = std::size_t{25} * 1024 * 1024;
// The most text one transcript keeps. A record's sealed value is at most 1 MiB of base64 in the
// workspace Zetta; this leaves room for the envelope and the other fields.
inline constexpr std::size_t max_transcript_text_bytes = 400'000;
inline constexpr std::size_t max_transcript_title_bytes = 300;
// The model lake asks for when LAKE_TRANSCRIBE_MODEL is not set.
inline constexpr std::string_view default_transcribe_model = "gpt-4o-transcribe";
inline constexpr std::string_view openai_transcriptions_url =
    "https://api.openai.com/v1/audio/transcriptions";

struct DecodedAudio {
    std::string bytes;
    std::string mime; // from a data: URL, else empty
};

// The audio of `audio_base64`: padded base64, optionally behind a `data:<mime>;base64,` prefix,
// whitespace ignored. invalid_input at `path` when it is not base64 or is empty; size_invalid at
// `path` when it decodes to more than max_transcription_audio_bytes.
std::expected<DecodedAudio, ApiError> decode_audio(std::string_view audio_base64,
                                                   const std::string& path);

// The file name sent with the audio: `name` without path, quotes or control characters, with an
// extension the provider recognises. A name without one takes the extension of `mime` (a phone's
// recording is audio/webm or audio/mp4), else ".webm".
std::string audio_file_name(std::string_view name, std::string_view mime);
// The media type of a file name's extension, application/octet-stream when unknown.
std::string audio_mime(std::string_view file_name);

// Whether `text` is WebVTT or SubRip captions rather than plain text: a WEBVTT header, or a cue
// timing line (`00:00:01,000 --> 00:00:04,000`).
bool looks_like_captions(std::string_view text);
// The spoken text of WebVTT or SubRip captions: no header, NOTE, STYLE or REGION blocks, cue
// numbers, cue identifiers, timings or inline tags; a `<v Name>` voice becomes "Name: "; a line
// repeated by the next cue (rolling captions) is kept once. Cues become lines. Plain text comes
// back unchanged, apart from CRLF line ends.
std::string clean_caption_text(std::string_view text);

// The multipart/form-data body of OpenAI's transcription request.
std::string openai_transcription_body(std::string_view boundary,
                                      const TranscriptionRequest& request,
                                      std::string_view model);
// OpenAI's answer as a Transcription, or engine_error with a plain message. The provider's own
// message is kept, except on 401/403, where it may echo part of the key.
std::expected<Transcription, ApiError>
parse_openai_transcription(int status, std::string_view body, std::string_view model);

struct TranscriptPostResponse {
    int status = 0;
    std::string body;
    std::string error; // the transport's failure, never the request
};
// One HTTPS POST: url, body, headers, timeout.
using TranscriptPost =
    std::function<TranscriptPostResponse(const std::string& url,
                                         std::string body,
                                         std::vector<std::pair<std::string, std::string>> headers,
                                         std::chrono::seconds timeout)>;

// A Transcriber that calls OpenAI with `api_key` through `post`. `model` empty: the default.
Transcriber openai_transcriber(std::string api_key, std::string model, TranscriptPost post);

} // namespace lake::api
