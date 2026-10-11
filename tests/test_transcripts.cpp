// Speech-to-text and transcripts (docs/api/transcripts.md, lake#561): the pure parts. Caption
// cleaning, audio decoding, the OpenAI request lake sends and how it reads the answer, paging and
// excerpts. The capabilities against workspace records are in test_workspace_records.cpp.

#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/audit_routing.hpp"
#include "lake/api/transcription.hpp"
#include "lake/control/transcripts.hpp"

namespace {

using json = nlohmann::json;
using lake::api::TranscriptPostResponse;

TEST(Transcripts, WebVttIsCleanedToWhatWasSaid) {
    const std::string vtt = "WEBVTT\r\n"
                            "Kind: captions\r\n"
                            "Language: en\r\n"
                            "\r\n"
                            "NOTE recorded on a phone\r\n"
                            "and spoken slowly\r\n"
                            "\r\n"
                            "STYLE\r\n"
                            "::cue { color: lime }\r\n"
                            "\r\n"
                            "intro\r\n"
                            "00:00:01.000 --> 00:00:04.000 align:start\r\n"
                            "<v Alice>Hello &amp; welcome</v>\r\n"
                            "\r\n"
                            "2\r\n"
                            "00:00:04.000 --> 00:00:06.500\r\n"
                            "<v.loud Bob>We start <00:00:05.000><c>now</c>.\r\n"
                            "\r\n"
                            "00:00:06.500 --> 00:00:08.000\r\n"
                            "<v.loud Bob>We start <00:00:05.000><c>now</c>.\r\n"
                            "Then 3 things.\r\n";
    ASSERT_TRUE(lake::api::looks_like_captions(vtt));
    EXPECT_EQ(lake::api::clean_caption_text(vtt),
              "Alice: Hello & welcome\nBob: We start now.\nThen 3 things.")
        << "header, NOTE and STYLE blocks, identifiers, timings and tags go; a line the next cue "
           "repeats is kept once";
}

TEST(Transcripts, SubRipIsCleanedOfCueNumbersAndTimings) {
    const std::string srt = "1\n"
                            "00:00:01,000 --> 00:00:03,200\n"
                            "First line\n"
                            "continues here\n"
                            "\n"
                            "2\n"
                            "00:00:03,200 --> 00:00:05,000\n"
                            "<i>Second</i> cue\n"
                            "\n"
                            "3\n"
                            "00:00:05,000 --> 00:00:07,000\n"
                            "42\n"
                            "\n";
    ASSERT_TRUE(lake::api::looks_like_captions(srt));
    EXPECT_EQ(lake::api::clean_caption_text(srt), "First line\ncontinues here\nSecond cue");
}

TEST(Transcripts, PlainTextIsKeptAsItIs) {
    const std::string text = "Minutes of the call.\r\n\r\n1. Budget --> approved\nNext steps.";
    EXPECT_FALSE(lake::api::looks_like_captions(text)) << "an arrow in prose is not a timing";
    EXPECT_EQ(lake::api::clean_caption_text(text),
              "Minutes of the call.\n\n1. Budget --> approved\nNext steps.");
    EXPECT_EQ(lake::api::clean_caption_text("WEBVTT\n\n"), "");
}

TEST(Transcripts, AudioIsDecodedFromBase64OrADataUrl) {
    const auto plain = lake::api::decode_audio("aGVs\nbG8=", "/audio_base64");
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->bytes, "hello");
    EXPECT_EQ(plain->mime, "");
    const auto url =
        lake::api::decode_audio("data:audio/webm;codecs=opus;base64,aGVsbG8", "/audio_base64");
    ASSERT_TRUE(url) << url.error().message;
    EXPECT_EQ(url->bytes, "hello");
    EXPECT_EQ(url->mime, "audio/webm");

    for (const std::string bad : {"", "not base64!", "data:audio/webm,aGVsbG8", "aGVs=bG8"}) {
        const auto refused = lake::api::decode_audio(bad, "/audio_base64");
        ASSERT_FALSE(refused) << bad;
        EXPECT_EQ(refused.error().code, "invalid_input") << bad;
        EXPECT_EQ(refused.error().path, "/audio_base64") << bad;
    }
    const std::string large((lake::api::max_transcription_audio_bytes / 3 + 2) * 4, 'A');
    const auto refused = lake::api::decode_audio(large, "/audio_base64");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "size_invalid");
    EXPECT_EQ(refused.error().path, "/audio_base64");
    const std::string limit(lake::api::max_transcription_audio_bytes / 3 * 4, 'A');
    EXPECT_TRUE(lake::api::decode_audio(limit, "/audio_base64")) << "25 MiB and under is taken";
}

TEST(Transcripts, TheAudioFileNameCarriesAFormatTheProviderKnows) {
    EXPECT_EQ(lake::api::audio_file_name("memo.m4a", ""), "memo.m4a");
    EXPECT_EQ(lake::api::audio_file_name("../dir/Call \"one\".MP3", ""), "Call one.MP3");
    EXPECT_EQ(lake::api::audio_file_name("recording", "audio/mp4"), "recording.mp4");
    EXPECT_EQ(lake::api::audio_file_name("", "audio/webm"), "recording.webm");
    EXPECT_EQ(lake::api::audio_file_name("voice", ""), "voice.webm");
    EXPECT_EQ(lake::api::audio_file_name("a\r\nb.wav", ""), "ab.wav");
    EXPECT_EQ(lake::api::audio_mime("x.m4a"), "audio/mp4");
    EXPECT_EQ(lake::api::audio_mime("x.bin"), "application/octet-stream");
    EXPECT_TRUE(lake::api::not_audio_reason("memo.m4a").empty());
    EXPECT_TRUE(lake::api::not_audio_reason("voice").empty()) << "its media type decides";
    EXPECT_TRUE(lake::api::not_audio_reason("memo.txt").starts_with("memo.txt is not audio"));
    EXPECT_NE(lake::api::not_audio_reason("call.vtt").find("create_transcript"), std::string::npos);
}

struct Sent {
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::chrono::seconds timeout{0};
};

lake::api::Transcriber fake_openai(Sent& sent, TranscriptPostResponse answer) {
    return lake::api::openai_transcriber(
        "sk-test-secret",
        "",
        [&sent, answer](const std::string& url,
                        std::string body,
                        std::vector<std::pair<std::string, std::string>> headers,
                        std::chrono::seconds timeout) {
            sent = {url, std::move(body), std::move(headers), timeout};
            return answer;
        });
}

std::string header(const Sent& sent, const std::string& name) {
    for (const auto& [key, value] : sent.headers) {
        if (key == name) {
            return value;
        }
    }
    return {};
}

TEST(Transcripts, OpenAiIsAskedWithOneMultipartUpload) {
    Sent sent;
    auto transcriber = fake_openai(
        sent, {200, R"({"text":"hello there","usage":{"type":"duration","seconds":3.5}})", ""});
    const auto done = transcriber({.file_name = "memo.webm",
                                   .mime = "audio/webm",
                                   .audio = std::string("\0\1audio", 7),
                                   .language = "en"});
    ASSERT_TRUE(done) << done.error().message;
    EXPECT_EQ(done->text, "hello there");
    EXPECT_EQ(done->seconds, 3.5);
    EXPECT_EQ(done->model, "gpt-4o-transcribe");

    EXPECT_EQ(sent.url, "https://api.openai.com/v1/audio/transcriptions");
    EXPECT_EQ(header(sent, "Authorization"), "Bearer sk-test-secret");
    const std::string type = header(sent, "Content-Type");
    ASSERT_TRUE(type.starts_with("multipart/form-data; boundary="));
    const std::string boundary = type.substr(type.find('=') + 1);
    EXPECT_GE(sent.timeout, std::chrono::seconds(120)) << "an upload outlasts the 30 s default";
    EXPECT_NE(sent.body.find("--" + boundary +
                             "\r\nContent-Disposition: form-data; "
                             "name=\"file\"; filename=\"memo.webm\"\r\nContent-Type: "
                             "audio/webm\r\n\r\n" +
                             std::string("\0\1audio", 7) + "\r\n"),
              std::string::npos);
    EXPECT_NE(sent.body.find("name=\"model\"\r\n\r\ngpt-4o-transcribe\r\n"), std::string::npos);
    EXPECT_NE(sent.body.find("name=\"language\"\r\n\r\nen\r\n"), std::string::npos);
    EXPECT_NE(sent.body.find("name=\"response_format\"\r\n\r\njson\r\n"), std::string::npos);
    EXPECT_TRUE(sent.body.ends_with("--" + boundary + "--\r\n"));

    // LAKE_TRANSCRIBE_MODEL replaces the model; no language lets OpenAI detect it.
    Sent other;
    auto whisper = lake::api::openai_transcriber(
        "k",
        "whisper-1",
        [&other](const std::string& url,
                 std::string body,
                 std::vector<std::pair<std::string, std::string>> headers,
                 std::chrono::seconds) {
            other = {url, std::move(body), std::move(headers), {}};
            return TranscriptPostResponse{200, R"({"text":"hi","duration":1.25})", ""};
        });
    const auto heard = whisper({.file_name = "a.mp3", .mime = "audio/mpeg", .audio = "x"});
    ASSERT_TRUE(heard);
    EXPECT_EQ(heard->model, "whisper-1");
    EXPECT_EQ(heard->seconds, 1.25);
    EXPECT_NE(other.body.find("whisper-1"), std::string::npos);
    EXPECT_EQ(other.body.find("name=\"language\""), std::string::npos);
}

TEST(Transcripts, OpenAiFailuresBecomePlainEngineErrors) {
    Sent sent;
    const lake::api::TranscriptionRequest request{.file_name = "a.webm", .audio = "x"};
    const auto refused_key = fake_openai(
        sent, {401, R"({"error":{"message":"Incorrect API key provided: sk-test-****cret"}})", ""})(
        request);
    ASSERT_FALSE(refused_key);
    EXPECT_EQ(refused_key.error().code, "engine_error");
    EXPECT_EQ(refused_key.error().message.find("sk-"), std::string::npos)
        << "a refused key is never echoed";

    const auto bad_audio = fake_openai(
        sent, {400, R"({"error":{"message":"Audio file might be corrupted or unsupported"}})", ""})(
        request);
    ASSERT_FALSE(bad_audio);
    EXPECT_EQ(bad_audio.error().code, "engine_error");
    EXPECT_EQ(bad_audio.error().message,
              "OpenAI could not transcribe the audio: Audio file might be corrupted or "
              "unsupported");

    const auto busy = fake_openai(sent, {429, "{}", ""})(request);
    EXPECT_EQ(busy.error().code, "engine_error");
    EXPECT_NE(busy.error().message.find("HTTP 429"), std::string::npos);

    const auto unreachable = fake_openai(sent, {0, "", "outbound request timed out"})(request);
    EXPECT_EQ(unreachable.error().code, "engine_error");
    EXPECT_NE(unreachable.error().message.find("timed out"), std::string::npos);

    const auto garbled = fake_openai(sent, {200, "not json", ""})(request);
    EXPECT_EQ(garbled.error().code, "engine_error");
}

lake::control::Transcript transcript(std::string id, std::string title, std::string text) {
    lake::control::Transcript out;
    out.id = std::move(id);
    out.title = std::move(title);
    out.text = std::move(text);
    return out;
}

TEST(Transcripts, PagesFilterIgnoringCaseAndFollowTheCursor) {
    const std::vector<lake::control::Transcript> all{
        transcript("e", "Board call", "Budget approved"),
        transcript("d", "Standup", "nothing"),
        transcript("c", "Sales sync", "the BUDGET slips"),
        transcript("b", "Retro", "budget?"),
        transcript("a", "Budget review", "numbers")};
    auto page = lake::control::page_transcripts(all, "budget", 2, "");
    ASSERT_EQ(page.items.size(), 2U);
    EXPECT_EQ(page.items[0].id, "e");
    EXPECT_EQ(page.items[1].id, "c");
    EXPECT_EQ(page.next_cursor, "c");
    EXPECT_EQ(page.total, 4U) << "total counts every match, not the page (lake#574)";
    page = lake::control::page_transcripts(all, "budget", 2, page.next_cursor);
    EXPECT_EQ(page.total, 4U) << "on every page";
    ASSERT_EQ(page.items.size(), 2U);
    EXPECT_EQ(page.items[0].id, "b");
    EXPECT_EQ(page.items[1].id, "a") << "a title matches too";
    EXPECT_TRUE(page.next_cursor.empty());
    EXPECT_EQ(lake::control::page_transcripts(all, "", 10, "").items.size(), 5U);
    EXPECT_EQ(lake::control::page_transcripts(all, "", 1, "").total, 5U);
    EXPECT_FALSE(lake::control::page_transcripts(all, "", 10, "zz").cursor_found);
}

TEST(Transcripts, AnExcerptIsOneLineCutAtACharacter) {
    EXPECT_EQ(lake::control::transcript_excerpt("  one\n\ntwo\tthree  "), "one two three");
    EXPECT_EQ(lake::control::transcript_excerpt("abcdef", 3), "abc…");
    EXPECT_EQ(lake::control::transcript_excerpt("ab\xC3\xA9z", 3), "ab\xC3\xA9…")
        << "a UTF-8 sequence is never split";
}

TEST(Transcripts, ASearchExcerptIsTheTextAroundTheMatch) {
    using lake::control::transcript_excerpt_around;
    const std::string text = "Opening remarks went long. " + std::string(300, 'x') +
                             " Then Dana said churn is up in the basic plan, so we act.\n\nMore.";
    const std::string around = transcript_excerpt_around(text, "CHURN is up", 80);
    EXPECT_EQ(around.rfind("…", 0), 0U) << "it does not start at the top: " << around;
    EXPECT_NE(around.find("churn is up in the basic plan"), std::string::npos) << around;
    EXPECT_EQ(around.find("Opening"), std::string::npos);
    EXPECT_EQ(around.substr(std::string("…").size(), 5), "Then ") << "it starts at a word";
    EXPECT_EQ(transcript_excerpt_around("short text with churn", "churn", 200),
              "short text with churn")
        << "a match near the top keeps the top, with no ellipsis";
    EXPECT_EQ(transcript_excerpt_around(text, "", 40), lake::control::transcript_excerpt(text, 40))
        << "no query: the start";
    EXPECT_EQ(transcript_excerpt_around(text, "not here", 40),
              lake::control::transcript_excerpt(text, 40))
        << "a title-only match: the start";
    const std::string unicode = std::string(100, 'a') + "\xC3\xA9\xC3\xA9\xC3\xA9needle";
    const std::string cut = transcript_excerpt_around(unicode, "needle", 17);
    EXPECT_EQ(cut, "…\xC3\xA9\xC3\xA9\xC3\xA9needle") << "a UTF-8 sequence is never split: " << cut;
}

TEST(Transcripts, TranscriptAndDictationAuditGoToTheWorkspace) {
    using lake::api::AuditLog;
    for (const std::string name : {"create_transcript",
                                   "list_transcripts",
                                   "get_transcript",
                                   "update_transcript",
                                   "delete_transcript",
                                   "transcribe"}) {
        EXPECT_EQ(lake::api::audit_log_of(name), AuditLog::Workspace) << name;
    }
}

} // namespace
