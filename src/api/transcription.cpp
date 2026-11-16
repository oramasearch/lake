// SPDX-License-Identifier: MIT

#include "lake/api/transcription.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <random>

#include <nlohmann/json.hpp>

namespace lake::api {
namespace {

constexpr std::chrono::seconds transcription_timeout{300};

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

int sextet(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

// Extension → media type, the formats OpenAI's transcription endpoint takes.
constexpr std::array<std::pair<std::string_view, std::string_view>, 10> audio_types{{
    {".webm", "audio/webm"},
    {".mp4", "audio/mp4"},
    {".m4a", "audio/mp4"},
    {".mp3", "audio/mpeg"},
    {".mpga", "audio/mpeg"},
    {".mpeg", "audio/mpeg"},
    {".wav", "audio/wav"},
    {".ogg", "audio/ogg"},
    {".oga", "audio/ogg"},
    {".flac", "audio/flac"},
}};

std::string extension_of(std::string_view name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string() : lower(name.substr(dot));
}

// The extension a media type is recorded with; empty when lake does not know it.
std::string_view extension_for(std::string_view mime) {
    const std::string type = lower(trim(mime.substr(0, mime.find(';'))));
    if (type == "audio/x-m4a" || type == "audio/m4a" || type == "audio/aac") {
        return ".m4a";
    }
    if (type == "audio/x-wav" || type == "audio/wave") {
        return ".wav";
    }
    if (type == "audio/mp3") {
        return ".mp3";
    }
    if (type == "video/webm") {
        return ".webm";
    }
    if (type == "video/mp4") {
        return ".mp4";
    }
    for (const auto& [extension, known] : audio_types) {
        if (known == type) {
            return extension;
        }
    }
    return {};
}

bool all_digits(std::string_view text) {
    return !text.empty() &&
           std::ranges::all_of(text, [](unsigned char c) { return std::isdigit(c) != 0; });
}

bool starts_with_word(std::string_view line, std::string_view word) {
    return line.starts_with(word) && (line.size() == word.size() || space(line[word.size()]));
}

// A cue's text line without its markup: `<v Name>` becomes "Name: ", every other tag goes, and
// the entities WebVTT escapes are restored.
std::string caption_line(std::string_view line) {
    std::string out;
    std::size_t index = 0;
    while (index < line.size()) {
        if (line[index] == '<') {
            const std::size_t close = line.find('>', index);
            if (close == std::string_view::npos) {
                out.append(line.substr(index));
                break;
            }
            const std::string_view tag = line.substr(index + 1, close - index - 1);
            if (tag.starts_with("v ") || tag.starts_with("v.")) {
                const std::size_t name = tag.find(' ');
                if (name != std::string_view::npos) {
                    const std::string_view speaker = trim(tag.substr(name + 1));
                    if (!speaker.empty()) {
                        out.append(speaker).append(": ");
                    }
                }
            }
            index = close + 1;
            continue;
        }
        if (line[index] == '&') {
            bool replaced = false;
            for (const auto& [entity, value] :
                 std::array<std::pair<std::string_view, std::string_view>, 5>{{{"&amp;", "&"},
                                                                               {"&lt;", "<"},
                                                                               {"&gt;", ">"},
                                                                               {"&nbsp;", " "},
                                                                               {"&quot;", "\""}}}) {
                if (line.substr(index).starts_with(entity)) {
                    out.append(value);
                    index += entity.size();
                    replaced = true;
                    break;
                }
            }
            if (replaced) {
                continue;
            }
        }
        out.push_back(line[index++]);
    }
    return std::string(trim(out));
}

std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string_view line = text.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return lines;
}

std::string plain_message(std::string_view body) {
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    std::string message;
    if (parsed.is_object() && parsed.contains("error")) {
        const nlohmann::json& error = parsed.at("error");
        if (error.is_object() && error.contains("message") && error.at("message").is_string()) {
            message = error.at("message").get<std::string>();
        } else if (error.is_string()) {
            message = error.get<std::string>();
        }
    }
    message = std::string(trim(message));
    if (message.size() > 300) {
        message.resize(300);
        message += "…";
    }
    return message;
}

std::string random_boundary() {
    static thread_local std::mt19937_64 generator{std::random_device{}()};
    return std::format("lake-transcribe-{:016x}{:016x}", generator(), generator());
}

} // namespace

std::expected<DecodedAudio, ApiError> decode_audio(std::string_view audio_base64,
                                                   const std::string& path) {
    DecodedAudio decoded;
    std::string_view data = trim(audio_base64);
    if (data.starts_with("data:")) {
        const std::size_t comma = data.find(',');
        if (comma == std::string_view::npos) {
            return std::unexpected(ApiError{"invalid_input", "the data: URL has no data", path});
        }
        const std::string_view header = data.substr(5, comma - 5);
        if (header.find(";base64") == std::string_view::npos) {
            return std::unexpected(
                ApiError{"invalid_input", "the data: URL must be base64-encoded", path});
        }
        decoded.mime = std::string(trim(header.substr(0, header.find(';'))));
        data.remove_prefix(comma + 1);
    }
    if (data.size() / 4 * 3 > max_transcription_audio_bytes + 3) {
        return std::unexpected(
            ApiError{"size_invalid",
                     "the audio is larger than 25 MiB; record or upload a shorter clip",
                     path});
    }
    decoded.bytes.reserve(data.size() / 4 * 3 + 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    bool padded = false;
    for (const char c : data) {
        if (space(c)) {
            continue;
        }
        if (c == '=') {
            padded = true;
            continue;
        }
        const int value = sextet(c);
        if (value < 0 || padded) {
            return std::unexpected(ApiError{"invalid_input", "the audio is not base64", path});
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.bytes.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    if (bits >= 6) {
        return std::unexpected(ApiError{"invalid_input", "the audio is not base64", path});
    }
    if (decoded.bytes.empty()) {
        return std::unexpected(ApiError{"invalid_input", "the audio is empty", path});
    }
    if (decoded.bytes.size() > max_transcription_audio_bytes) {
        return std::unexpected(
            ApiError{"size_invalid",
                     "the audio is larger than 25 MiB; record or upload a shorter clip",
                     path});
    }
    return decoded;
}

std::string audio_mime(std::string_view file_name) {
    const std::string extension = extension_of(file_name);
    for (const auto& [known, type] : audio_types) {
        if (known == extension) {
            return std::string(type);
        }
    }
    return "application/octet-stream";
}

std::string not_audio_reason(std::string_view name) {
    if (const std::size_t slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    const std::string extension = extension_of(name);
    if (extension.empty() || audio_mime(name) != "application/octet-stream") {
        return {};
    }
    std::string known;
    for (const auto& [listed, type] : audio_types) {
        known += (known.empty() ? "" : ", ") + std::string(listed);
    }
    std::string reason = std::string(name) + " is not audio or video: send " + known;
    if (extension == ".txt" || extension == ".vtt" || extension == ".srt") {
        reason += ", or add the text with create_transcript (source text or upload)";
    }
    return reason;
}

std::string audio_file_name(std::string_view name, std::string_view mime) {
    if (const std::size_t slash = name.find_last_of("/\\"); slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::string clean;
    for (const char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte >= 0x20 && byte != 0x7f && c != '"' && c != '\\') {
            clean.push_back(c);
        }
    }
    clean = std::string(trim(clean));
    if (clean.size() > 120) {
        clean = clean.substr(clean.size() - 120);
    }
    if (clean.empty()) {
        clean = "recording";
    }
    if (audio_mime(clean) == "application/octet-stream") {
        const std::string_view extension = extension_for(mime);
        clean += extension.empty() ? std::string_view(".webm") : extension;
    }
    return clean;
}

bool looks_like_captions(std::string_view text) {
    const std::string_view start = trim(text.substr(0, 64));
    if (start.starts_with("WEBVTT") || (start.size() > 3 && start.substr(0, 3) == "\xEF\xBB\xBF" &&
                                        trim(start.substr(3)).starts_with("WEBVTT"))) {
        return true;
    }
    for (const std::string_view line : lines_of(text.substr(0, 4096))) {
        const std::size_t arrow = line.find("-->");
        if (arrow == std::string_view::npos) {
            continue;
        }
        const std::string_view from = trim(line.substr(0, arrow));
        if (from.size() >= 5 && std::isdigit(static_cast<unsigned char>(from.front())) != 0 &&
            from.find(':') != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

std::string clean_caption_text(std::string_view text) {
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3);
    }
    if (!looks_like_captions(text)) {
        std::string plain;
        plain.reserve(text.size());
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
                continue;
            }
            plain.push_back(text[index]);
        }
        return plain;
    }
    const std::vector<std::string_view> lines = lines_of(text);
    std::vector<std::string> kept;
    bool skipping = false;   // inside a header, NOTE, STYLE or REGION block
    bool block_start = true; // the previous line was blank (or this is the first)
    bool first_block = true;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        const std::string_view line = trim(lines[index]);
        if (line.empty()) {
            if (!block_start) {
                first_block = false;
            }
            skipping = false;
            block_start = true;
            continue;
        }
        const bool at_start = block_start;
        block_start = false;
        if (skipping) {
            continue;
        }
        if (at_start &&
            ((first_block && line.starts_with("WEBVTT")) || starts_with_word(line, "NOTE") ||
             starts_with_word(line, "STYLE") || starts_with_word(line, "REGION"))) {
            skipping = true;
            continue;
        }
        if (line.find("-->") != std::string_view::npos) {
            continue; // a cue's timing
        }
        if (index + 1 < lines.size() && lines[index + 1].find("-->") != std::string_view::npos) {
            continue; // a cue number (SubRip) or identifier (WebVTT)
        }
        if (all_digits(line)) {
            continue;
        }
        std::string spoken = caption_line(line);
        if (spoken.empty() || (!kept.empty() && kept.back() == spoken)) {
            continue;
        }
        kept.push_back(std::move(spoken));
    }
    std::string out;
    for (const std::string& line : kept) {
        if (!out.empty()) {
            out.push_back('\n');
        }
        out += line;
    }
    return out;
}

std::string openai_transcription_body(std::string_view boundary,
                                      const TranscriptionRequest& request,
                                      std::string_view model) {
    std::string body;
    body.reserve(request.audio.size() + 1024);
    const auto field = [&](std::string_view name, std::string_view value) {
        body += std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"\r\n\r\n{}\r\n",
                            boundary,
                            name,
                            value);
    };
    body += std::format("--{}\r\nContent-Disposition: form-data; name=\"file\"; "
                        "filename=\"{}\"\r\nContent-Type: {}\r\n\r\n",
                        boundary,
                        request.file_name,
                        request.mime.empty() ? audio_mime(request.file_name) : request.mime);
    body += request.audio;
    body += "\r\n";
    field("model", model);
    field("response_format", "json");
    if (!request.language.empty()) {
        field("language", request.language);
    }
    body += std::format("--{}--\r\n", boundary);
    return body;
}

std::expected<Transcription, ApiError>
parse_openai_transcription(int status, std::string_view body, std::string_view model) {
    if (status == 401 || status == 403) {
        return std::unexpected(
            ApiError{"engine_error",
                     "OpenAI refused this Lake's API key, so speech-to-text is unavailable; an "
                     "administrator should check LAKE_OPENAI_API_KEY"});
    }
    if (status < 200 || status >= 300) {
        const std::string message = plain_message(body);
        std::string text = status == 429   ? "OpenAI is busy or over its quota; try again shortly"
                           : status == 413 ? "OpenAI refused the audio as too large"
                           : status >= 500 ? "OpenAI could not transcribe the audio right now"
                                           : "OpenAI could not transcribe the audio";
        text += message.empty() ? std::format(" (HTTP {})", status) : ": " + message;
        return std::unexpected(ApiError{"engine_error", std::move(text)});
    }
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    if (!parsed.is_object() || !parsed.contains("text") || !parsed.at("text").is_string()) {
        return std::unexpected(ApiError{"engine_error", "OpenAI answered without a transcript"});
    }
    Transcription out{parsed.at("text").get<std::string>(), std::nullopt, std::string(model)};
    if (parsed.contains("duration") && parsed.at("duration").is_number()) {
        out.seconds = parsed.at("duration").get<double>();
    } else if (const nlohmann::json usage = parsed.value("usage", nlohmann::json());
               usage.is_object() && usage.value("type", std::string()) == "duration" &&
               usage.contains("seconds") && usage.at("seconds").is_number()) {
        out.seconds = usage.at("seconds").get<double>();
    }
    // gpt-4o-transcribe counts tokens instead: kept as unbilled detail for the audit; the
    // speech is billed by its measured minutes (lake#658).
    if (const nlohmann::json usage = parsed.value("usage", nlohmann::json());
        usage.is_object() && usage.value("type", std::string()) == "tokens") {
        const auto tokens = [&](const char* name) -> std::optional<std::uint64_t> {
            const nlohmann::json value = usage.value(name, nlohmann::json());
            if (value.is_number_unsigned() ||
                (value.is_number_integer() && value.get<std::int64_t>() >= 0)) {
                return value.get<std::uint64_t>();
            }
            return std::nullopt;
        };
        out.input_tokens = tokens("input_tokens");
        out.output_tokens = tokens("output_tokens");
    }
    return out;
}

Transcriber openai_transcriber(std::string api_key, std::string model, TranscriptPost post) {
    if (model.empty()) {
        model = std::string(default_transcribe_model);
    }
    return [api_key = std::move(api_key), model = std::move(model), post = std::move(post)](
               const TranscriptionRequest& request) -> std::expected<Transcription, ApiError> {
        const std::string boundary = random_boundary();
        TranscriptPostResponse response =
            post(std::string(openai_transcriptions_url),
                 openai_transcription_body(boundary, request, model),
                 {{"Authorization", "Bearer " + api_key},
                  {"Content-Type", "multipart/form-data; boundary=" + boundary},
                  {"Accept", "application/json"},
                  {"User-Agent", "lake-transcribe/1"}},
                 transcription_timeout);
        if (!response.error.empty()) {
            return std::unexpected(ApiError{
                "engine_error", "could not reach OpenAI for speech-to-text: " + response.error});
        }
        return parse_openai_transcription(response.status, response.body, model);
    };
}

} // namespace lake::api
