// How long a recording is, read from its container (lake#658): the seconds speech-to-text is billed
// by when the model reports none. Each format is built here byte by byte, as a recorder writes it.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "lake/api/transcription.hpp"

namespace {

using lake::api::audio_duration_seconds;

void le(std::string& out, std::uint64_t value, int width) {
    for (int index = 0; index < width; ++index) {
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

void be(std::string& out, std::uint64_t value, int width) {
    for (int index = width - 1; index >= 0; --index) {
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xFF));
    }
}

std::string box(std::string_view type, const std::string& body) {
    std::string out;
    be(out, 8 + body.size(), 4);
    out += type;
    return out + body;
}

// An EBML element with a one-byte (or unknown) size and the id's own bytes.
std::string element(std::string_view id, const std::string& body, bool unknown = false) {
    std::string out(id);
    if (unknown) {
        out += "\x01\xFF\xFF\xFF\xFF\xFF\xFF\xFF";
    } else {
        out.push_back(static_cast<char>(0x80 | body.size()));
    }
    return out + body;
}

std::string ogg_page(std::uint64_t granule, const std::string& packet) {
    std::string out = "OggS";
    out.push_back(0);
    out.push_back(0);
    le(out, granule, 8);
    le(out, 1234, 4); // serial
    le(out, 0, 4);
    le(out, 0, 4);
    out.push_back(1);
    out.push_back(static_cast<char>(packet.size()));
    return out + packet;
}

} // namespace

TEST(AudioDuration, WavIsItsDataOverItsByteRate) {
    std::string wav = "RIFF";
    le(wav, 0, 4);
    wav += "WAVEfmt ";
    le(wav, 16, 4);
    le(wav, 1, 2);
    le(wav, 2, 2);
    le(wav, 48000, 4);
    le(wav, 192000, 4);
    le(wav, 4, 2);
    le(wav, 16, 2);
    wav += "data";
    le(wav, 192000 * 3, 4);
    wav.append(192000 * 3, '\0');
    EXPECT_DOUBLE_EQ(*audio_duration_seconds(wav), 3.0);
    // A streamed WAV that says 0 bytes: the rest of the file is its data.
    std::string streamed = wav;
    std::memset(streamed.data() + 40, 0, 4);
    EXPECT_DOUBLE_EQ(*audio_duration_seconds(streamed), 3.0);
}

TEST(AudioDuration, FlacIsItsStreamInfo) {
    std::string flac = "fLaC";
    flac += std::string("\x80\x00\x00\x22", 4); // last block, STREAMINFO, 34 bytes
    flac.append(10, '\0');
    // 44.1 kHz, 2 channels, 16 bits, 441000 samples: ten seconds.
    const std::uint64_t packed = (std::uint64_t{44100} << 44) | (std::uint64_t{1} << 41) |
                                 (std::uint64_t{15} << 36) | 441000;
    be(flac, packed, 8);
    flac.append(16, '\0');
    EXPECT_DOUBLE_EQ(*audio_duration_seconds(flac), 10.0);
}

TEST(AudioDuration, OggOpusIsItsLastGranuleLessThePreSkip) {
    std::string head = "OpusHead";
    head.push_back(1);
    head.push_back(1);
    le(head, 312, 2); // pre-skip
    le(head, 48000, 4);
    head.append(3, '\0');
    const std::string ogg = ogg_page(0, head) + ogg_page(0, std::string(10, 'x')) +
                            ogg_page(48000 * 5 + 312, std::string(20, 'y'));
    EXPECT_DOUBLE_EQ(*audio_duration_seconds(ogg), 5.0);
}

TEST(AudioDuration, WebmIsItsDurationOrItsLatestBlock) {
    const std::string ebml = element("\x1A\x45\xDF\xA3", "\x42\x82\x84webm");
    std::string duration_bytes;
    double seconds_ms = 7250.0; // in the default 1 ms timecode units
    std::uint64_t raw = 0;
    std::memcpy(&raw, &seconds_ms, 8);
    be(duration_bytes, raw, 8);
    const std::string with_duration =
        ebml + element("\x18\x53\x80\x67",
                       element("\x15\x49\xA9\x66", element("\x44\x89", duration_bytes)), true);
    EXPECT_DOUBLE_EQ(*audio_duration_seconds(with_duration), 7.25);

    // A browser's recording: no Duration, clusters of unknown size, blocks with relative times.
    const auto block = [](std::int16_t relative) {
        std::string body = "\x81"; // track 1
        be(body, static_cast<std::uint16_t>(relative), 2);
        body += "\x80" "frame";
        return element("\xA3", body);
    };
    std::string cluster_one = element("\xE7", "\x00") + block(0) + block(980);
    std::string cluster_two = element("\xE7", std::string("\x13\x88", 2)) + block(0) + block(1500);
    const std::string recorded =
        ebml + element("\x18\x53\x80\x67",
                       element("\x15\x49\xA9\x66", element("\x2A\xD7\xB1", std::string("\x0F\x42\x40", 3))) +
                           element("\x1F\x43\xB6\x75", cluster_one, true) +
                           element("\x1F\x43\xB6\x75", cluster_two, true),
                       true);
    EXPECT_DOUBLE_EQ(*audio_duration_seconds(recorded), 6.5) << "5000 ms + 1500 ms";
}

TEST(AudioDuration, Mp4IsItsMovieHeaderOrItsFragments) {
    std::string mvhd(4, '\0'); // version 0, flags
    be(mvhd, 0, 4);
    be(mvhd, 0, 4);
    be(mvhd, 1000, 4);  // timescale
    be(mvhd, 12500, 4); // duration
    const std::string plain = box("ftyp", "M4A \0\0\0\0") + box("moov", box("mvhd", mvhd));
    EXPECT_DOUBLE_EQ(*audio_duration_seconds(plain), 12.5);

    // Fragmented (Safari's MediaRecorder): mvhd says 0; the fragments carry the durations.
    std::string empty_mvhd(4, '\0');
    be(empty_mvhd, 0, 4);
    be(empty_mvhd, 0, 4);
    be(empty_mvhd, 1000, 4);
    be(empty_mvhd, 0, 4);
    std::string tkhd(4, '\0');
    be(tkhd, 0, 4);
    be(tkhd, 0, 4);
    be(tkhd, 1, 4); // track id
    std::string mdhd(4, '\0');
    be(mdhd, 0, 4);
    be(mdhd, 0, 4);
    be(mdhd, 48000, 4);
    std::string trex(4, '\0');
    be(trex, 1, 4);    // track id
    be(trex, 1, 4);    // sample description
    be(trex, 1024, 4); // default duration
    const std::string moov = box("moov", box("mvhd", empty_mvhd) +
                                             box("trak", box("tkhd", tkhd) +
                                                             box("mdia", box("mdhd", mdhd))) +
                                             box("mvex", box("trex", trex)));
    const auto fragment = [](std::uint32_t samples, bool durations) {
        std::string tfhd(4, '\0');
        be(tfhd, 1, 4);
        std::string trun;
        be(trun, durations ? 0x100 : 0, 4);
        be(trun, samples, 4);
        for (std::uint32_t sample = 0; durations && sample < samples; ++sample) {
            be(trun, 960, 4);
        }
        return box("moof", box("traf", box("tfhd", tfhd) + box("trun", trun))) +
               box("mdat", "x");
    };
    // 100 samples of 960 and 50 of the default 1024 at 48 kHz: 3.0667 seconds.
    const std::string fragmented =
        box("ftyp", "iso5\0\0\0\0") + moov + fragment(100, true) + fragment(50, false);
    EXPECT_NEAR(*audio_duration_seconds(fragmented), (100 * 960 + 50 * 1024) / 48000.0, 1e-9);
}

TEST(AudioDuration, Mp3IsItsFrameCountOrItsBitRate) {
    // MPEG-1 Layer III, 128 kbps, 44.1 kHz, stereo.
    const std::string header("\xFF\xFB\x90\x00", 4);
    std::string cbr = "ID3";
    cbr += std::string("\x04\x00\x00\x00\x00\x00\x00", 7); // version, flags, size 0
    cbr += header;
    cbr.append(16000 * 4 - 4, '\0'); // four seconds at 16,000 bytes a second
    EXPECT_NEAR(*audio_duration_seconds(cbr), 4.0, 1e-9);

    std::string vbr = header;
    vbr.append(32, '\0');
    vbr += "Xing";
    be(vbr, 0x1, 4);
    be(vbr, 383, 4); // 383 frames of 1152 samples
    vbr.append(400, '\0');
    EXPECT_NEAR(*audio_duration_seconds(vbr), 383 * 1152 / 44100.0, 1e-9);
}

TEST(AudioDuration, AnythingElseIsNotGuessed) {
    EXPECT_FALSE(audio_duration_seconds(""));
    EXPECT_FALSE(audio_duration_seconds("speech"));
    EXPECT_FALSE(audio_duration_seconds(std::string("RIFF\0\0\0\0WAVE", 12)));
    EXPECT_FALSE(audio_duration_seconds(std::string("\x1A\x45\xDF\xA3\x84", 5))) << "cut short";
    EXPECT_FALSE(audio_duration_seconds(std::string(64, '\xFF'))) << "no valid frame";
}
