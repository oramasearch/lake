// SPDX-License-Identifier: MIT

// The length of a recording, read from its container (lake#658, docs/api/transcripts.md#how-long-
// a-recording-is): speech-to-text is billed by the minute, and gpt-4o-transcribe reports tokens,
// not seconds, so lake measures the audio it sends. Pure functions over the bytes; every read is
// bounds-checked, and anything lake cannot read answers nullopt rather than a guess.

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string_view>

#include "lake/api/transcription.hpp"

namespace lake::api {
namespace {

using Bytes = std::string_view;

std::optional<std::uint64_t> be(Bytes data, std::size_t at, std::size_t width) {
    if (width > 8 || at > data.size() || data.size() - at < width) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value = (value << 8) | static_cast<unsigned char>(data[at + index]);
    }
    return value;
}

std::optional<std::uint64_t> le(Bytes data, std::size_t at, std::size_t width) {
    if (width > 8 || at > data.size() || data.size() - at < width) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t index = width; index > 0; --index) {
        value = (value << 8) | static_cast<unsigned char>(data[at + index - 1]);
    }
    return value;
}

std::optional<double> positive(double seconds) {
    if (!(seconds > 0) || seconds > 24.0 * 3600) { // NaN, nothing, or not a recording
        return std::nullopt;
    }
    return seconds;
}

// ---- WAV: the data chunk's bytes over the format's byte rate.
std::optional<double> wav_seconds(Bytes data) {
    std::uint64_t byte_rate = 0;
    std::optional<std::uint64_t> data_bytes;
    for (std::size_t at = 12; at + 8 <= data.size();) {
        const Bytes id = data.substr(at, 4);
        const auto size = le(data, at + 4, 4);
        if (!size) {
            break;
        }
        if (id == "fmt ") {
            byte_rate = le(data, at + 16, 4).value_or(0);
            // The rate the samples imply: a header may overstate byte_rate to shorten the
            // recording it bills (LSR-112, lake#979), so the smaller of the two is used.
            const auto channels = le(data, at + 10, 2).value_or(0);
            const auto sample_rate = le(data, at + 12, 4).value_or(0);
            const auto bits = le(data, at + 22, 2).value_or(0);
            const std::uint64_t implied = channels * sample_rate * ((bits + 7) / 8);
            if (implied > 0 && (byte_rate == 0 || implied < byte_rate)) {
                byte_rate = implied;
            }
        } else if (id == "data") {
            // Every byte after the data header is sent to the model, whatever the header says:
            // a size that understates them does not shorten what is billed (lake#979).
            data_bytes = data.size() - at - 8;
            break;
        }
        at += 8 + *size + (*size & 1);
    }
    if (byte_rate == 0 || !data_bytes) {
        return std::nullopt;
    }
    return positive(static_cast<double>(*data_bytes) / static_cast<double>(byte_rate));
}

// ---- FLAC: STREAMINFO's total samples over its sample rate.
std::optional<double> flac_seconds(Bytes data) {
    const auto packed = be(data, 8 + 10, 8); // after "fLaC" and the block header
    if (!packed) {
        return std::nullopt;
    }
    const std::uint64_t rate = *packed >> 44;
    const std::uint64_t samples = *packed & 0xFFFFFFFFFULL;
    if (rate == 0 || samples == 0) {
        return std::nullopt;
    }
    return positive(static_cast<double>(samples) / static_cast<double>(rate));
}

// ---- Ogg (Opus, Vorbis): the last page's granule position over the codec's rate.
std::optional<double> ogg_seconds(Bytes data) {
    if (data.size() < 28) {
        return std::nullopt;
    }
    const std::uint64_t serial = le(data, 14, 4).value_or(0);
    const std::size_t first_packet = 27 + static_cast<unsigned char>(data[26]);
    double rate = 0;
    std::uint64_t pre_skip = 0;
    if (data.substr(first_packet, 8) == "OpusHead") {
        rate = 48000; // Opus granules count at 48 kHz whatever the input rate
        pre_skip = le(data, first_packet + 10, 2).value_or(0);
    } else if (data.substr(first_packet, 7) == "\x01vorbis") {
        rate = static_cast<double>(le(data, first_packet + 12, 4).value_or(0));
    }
    if (rate <= 0) {
        return std::nullopt;
    }
    for (std::size_t at = data.rfind("OggS"); at != Bytes::npos && at > 0;
         at = data.rfind("OggS", at - 1)) {
        const auto granule = le(data, at + 6, 8);
        if (granule && *granule != std::numeric_limits<std::uint64_t>::max() &&
            le(data, at + 14, 4).value_or(serial + 1) == serial) {
            return positive(static_cast<double>(*granule - std::min(*granule, pre_skip)) / rate);
        }
    }
    return std::nullopt;
}

// ---- MP4 / M4A / MOV: the movie header's duration, or, for a fragmented file (a browser's
// recording), the summed sample durations of its fragments.
struct Box {
    Bytes type;
    std::size_t body = 0; // offset of the body
    std::size_t end = 0;  // offset after the box
};

std::optional<Box> box_at(Bytes data, std::size_t at, std::size_t limit) {
    const auto size = be(data, at, 4);
    if (!size || at + 8 > limit) {
        return std::nullopt;
    }
    Box box{data.substr(at + 4, 4), at + 8, 0};
    std::uint64_t length = *size;
    if (length == 1) {
        const auto large = be(data, at + 8, 8);
        if (!large) {
            return std::nullopt;
        }
        length = *large;
        box.body = at + 16;
    } else if (length == 0) {
        length = limit - at;
    }
    if (length < box.body - at || length > limit - at) {
        return std::nullopt;
    }
    box.end = at + static_cast<std::size_t>(length);
    return box;
}

template <class Visit> void each_box(Bytes data, std::size_t from, std::size_t to, Visit visit) {
    for (std::size_t at = from; at < to;) {
        const auto box = box_at(data, at, to);
        if (!box || box->end <= at) {
            return;
        }
        visit(*box);
        at = box->end;
    }
}

std::optional<double> mp4_seconds(Bytes data) {
    std::optional<double> movie;
    std::map<std::uint64_t, std::uint64_t> timescale;        // track id -> mdhd timescale
    std::map<std::uint64_t, std::uint64_t> default_duration; // track id -> trex default
    std::map<std::uint64_t, std::uint64_t> fragments;        // track id -> summed durations
    each_box(data, 0, data.size(), [&](const Box& top) {
        if (top.type == "moov") {
            each_box(data, top.body, top.end, [&](const Box& box) {
                if (box.type == "mvhd") {
                    const bool wide = be(data, box.body, 1).value_or(0) == 1;
                    const auto scale = be(data, box.body + (wide ? 20 : 12), 4);
                    const auto length = be(data, box.body + (wide ? 24 : 16), wide ? 8 : 4);
                    if (scale && length && *scale > 0 && *length > 0 &&
                        *length != (wide ? std::numeric_limits<std::uint64_t>::max()
                                         : 0xFFFFFFFFULL)) {
                        movie = static_cast<double>(*length) / static_cast<double>(*scale);
                    }
                } else if (box.type == "trak") {
                    std::uint64_t id = 0, scale = 0;
                    each_box(data, box.body, box.end, [&](const Box& part) {
                        if (part.type == "tkhd") {
                            const bool wide = be(data, part.body, 1).value_or(0) == 1;
                            id = be(data, part.body + (wide ? 20 : 12), 4).value_or(0);
                        } else if (part.type == "mdia") {
                            each_box(data, part.body, part.end, [&](const Box& media) {
                                if (media.type == "mdhd") {
                                    const bool wide = be(data, media.body, 1).value_or(0) == 1;
                                    scale = be(data, media.body + (wide ? 20 : 12), 4).value_or(0);
                                }
                            });
                        }
                    });
                    if (scale > 0) {
                        timescale[id] = scale;
                    }
                } else if (box.type == "mvex") {
                    each_box(data, box.body, box.end, [&](const Box& trex) {
                        if (trex.type == "trex") {
                            const auto id = be(data, trex.body + 4, 4);
                            const auto duration = be(data, trex.body + 12, 4);
                            if (id && duration) {
                                default_duration[*id] = *duration;
                            }
                        }
                    });
                }
            });
        } else if (top.type == "moof") {
            each_box(data, top.body, top.end, [&](const Box& traf) {
                if (traf.type != "traf") {
                    return;
                }
                std::uint64_t id = 0, fallback = 0;
                each_box(data, traf.body, traf.end, [&](const Box& part) {
                    const std::uint64_t flags = be(data, part.body, 4).value_or(0) & 0xFFFFFF;
                    if (part.type == "tfhd") {
                        id = be(data, part.body + 4, 4).value_or(0);
                        fallback = default_duration.contains(id) ? default_duration[id] : 0;
                        std::size_t at = part.body + 8;
                        at += (flags & 0x1) ? 8 : 0;
                        at += (flags & 0x2) ? 4 : 0;
                        if (flags & 0x8) {
                            fallback = be(data, at, 4).value_or(fallback);
                        }
                    } else if (part.type == "trun") {
                        const std::uint64_t count = be(data, part.body + 4, 4).value_or(0);
                        std::size_t at = part.body + 8;
                        at += (flags & 0x1) ? 4 : 0;
                        at += (flags & 0x4) ? 4 : 0;
                        const std::size_t each = ((flags & 0x100) ? 4 : 0) +
                                                 ((flags & 0x200) ? 4 : 0) +
                                                 ((flags & 0x400) ? 4 : 0) +
                                                 ((flags & 0x800) ? 4 : 0);
                        if (!(flags & 0x100)) {
                            fragments[id] += count * fallback;
                            return;
                        }
                        for (std::uint64_t sample = 0; sample < count && at + each <= part.end;
                             ++sample, at += each) {
                            fragments[id] += be(data, at, 4).value_or(0);
                        }
                    }
                });
            });
        }
    });
    if (movie) {
        return positive(*movie);
    }
    double longest = 0;
    for (const auto& [id, units] : fragments) {
        if (timescale.contains(id) && timescale[id] > 0) {
            longest = std::max(longest,
                               static_cast<double>(units) / static_cast<double>(timescale[id]));
        }
    }
    return positive(longest);
}

// ---- WebM / Matroska: the segment's Duration, or, for a browser's recording that has none,
// the latest block's time (its cluster's timecode plus the block's own offset).
std::optional<std::pair<std::uint64_t, std::size_t>> ebml_id(Bytes data, std::size_t at) {
    if (at >= data.size()) {
        return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(data[at]);
    const std::size_t width = first == 0 ? 0 : static_cast<std::size_t>(std::countl_zero(first)) + 1;
    if (width == 0 || width > 4) {
        return std::nullopt;
    }
    const auto id = be(data, at, width);
    if (!id) {
        return std::nullopt;
    }
    return std::pair{*id, width};
}

// A size, or max() for an unknown size.
std::optional<std::pair<std::uint64_t, std::size_t>> ebml_size(Bytes data, std::size_t at) {
    if (at >= data.size()) {
        return std::nullopt;
    }
    const auto first = static_cast<unsigned char>(data[at]);
    const std::size_t width = first == 0 ? 0 : static_cast<std::size_t>(std::countl_zero(first)) + 1;
    const auto raw = width == 0 ? std::nullopt : be(data, at, width);
    if (!raw) {
        return std::nullopt;
    }
    const std::uint64_t value = *raw & ((std::uint64_t{1} << (7 * width)) - 1);
    const bool unknown = value == (std::uint64_t{1} << (7 * width)) - 1;
    return std::pair{unknown ? std::numeric_limits<std::uint64_t>::max() : value, width};
}

std::optional<double> webm_seconds(Bytes data) {
    constexpr std::uint64_t segment = 0x18538067, cluster = 0x1F43B675, info = 0x1549A966,
                            block_group = 0xA0, block = 0xA1, simple_block = 0xA3,
                            cluster_timecode = 0xE7, timecode_scale = 0x2AD7B1,
                            duration = 0x4489;
    double scale = 1'000'000; // nanoseconds per timecode unit
    std::optional<double> declared;
    std::uint64_t cluster_time = 0;
    std::optional<std::int64_t> latest;
    for (std::size_t at = 0; at < data.size();) {
        const auto id = ebml_id(data, at);
        const auto size = id ? ebml_size(data, at + id->second) : std::nullopt;
        if (!size) {
            break;
        }
        const std::size_t body = at + id->second + size->second;
        const std::uint64_t length = size->first;
        const bool unknown = length == std::numeric_limits<std::uint64_t>::max();
        if (id->first == segment || id->first == cluster || id->first == info ||
            id->first == block_group) {
            at = body; // a container: read what is inside it, in order
            continue;
        }
        if (unknown || body > data.size() || length > data.size() - body) {
            break; // cut short: what was read so far stands
        }
        if (id->first == timecode_scale) {
            scale = static_cast<double>(be(data, body, length).value_or(1'000'000));
        } else if (id->first == duration && (length == 4 || length == 8)) {
            const std::uint64_t raw = be(data, body, length).value_or(0);
            declared = length == 4 ? static_cast<double>(std::bit_cast<float>(
                                         static_cast<std::uint32_t>(raw)))
                                   : std::bit_cast<double>(raw);
        } else if (id->first == cluster_timecode) {
            cluster_time = be(data, body, length).value_or(0);
        } else if (id->first == simple_block || id->first == block) {
            const auto track = ebml_size(data, body); // the track number is a size-coded vint
            if (track) {
                const auto offset = be(data, body + track->second, 2);
                if (offset) {
                    const auto relative = static_cast<std::int16_t>(*offset);
                    const std::int64_t time = static_cast<std::int64_t>(cluster_time) + relative;
                    latest = latest ? std::max(*latest, time) : time;
                }
            }
        }
        at = body + static_cast<std::size_t>(length);
    }
    if (declared && *declared > 0) {
        return positive(*declared * scale / 1e9);
    }
    if (latest) {
        return positive(static_cast<double>(*latest) * scale / 1e9);
    }
    return std::nullopt;
}

// ---- MP3: a Xing/Info or VBRI header's frame count, else the bit rate over the bytes.
std::optional<double> mp3_seconds(Bytes data) {
    std::size_t at = 0;
    if (data.substr(0, 3) == "ID3" && data.size() >= 10) {
        const std::size_t tag = (static_cast<std::size_t>(data[6] & 0x7F) << 21) |
                                (static_cast<std::size_t>(data[7] & 0x7F) << 14) |
                                (static_cast<std::size_t>(data[8] & 0x7F) << 7) |
                                static_cast<std::size_t>(data[9] & 0x7F);
        at = 10 + tag + ((data[5] & 0x10) ? 10 : 0);
    }
    while (at + 4 <= data.size() &&
           !(static_cast<unsigned char>(data[at]) == 0xFF &&
             (static_cast<unsigned char>(data[at + 1]) & 0xE0) == 0xE0)) {
        ++at;
    }
    const auto header = be(data, at, 4);
    if (!header) {
        return std::nullopt;
    }
    const unsigned version = (*header >> 19) & 0x3; // 3: MPEG-1, 2: MPEG-2, 0: MPEG-2.5
    const unsigned layer = (*header >> 17) & 0x3;   // 1: Layer III
    const unsigned bitrate_index = (*header >> 12) & 0xF;
    const unsigned rate_index = (*header >> 10) & 0x3;
    const bool mono = ((*header >> 6) & 0x3) == 3;
    if (version == 1 || layer != 1 || bitrate_index == 0 || bitrate_index == 15 ||
        rate_index == 3) {
        return std::nullopt;
    }
    constexpr std::array<unsigned, 3> rates{44100, 48000, 32000};
    const unsigned rate = rates[rate_index] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    const unsigned samples = version == 3 ? 1152 : 576;
    constexpr std::array<unsigned, 15> v1{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    constexpr std::array<unsigned, 15> v2{0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const unsigned kbps = version == 3 ? v1[bitrate_index] : v2[bitrate_index];
    const std::size_t side = version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const Bytes xing = data.substr(std::min(data.size(), at + 4 + side), 4);
    if (xing == "Xing" || xing == "Info") {
        const std::size_t flags_at = at + 4 + side + 4;
        if (be(data, flags_at, 4).value_or(0) & 0x1) {
            const auto frames = be(data, flags_at + 4, 4);
            if (frames && *frames > 0) {
                return positive(static_cast<double>(*frames) * samples / rate);
            }
        }
    }
    if (data.substr(std::min(data.size(), at + 36), 4) == "VBRI") {
        const auto frames = be(data, at + 36 + 14, 4);
        if (frames && *frames > 0) {
            return positive(static_cast<double>(*frames) * samples / rate);
        }
    }
    return positive(static_cast<double>(data.size() - at) * 8.0 / (kbps * 1000.0));
}

} // namespace

std::optional<double> audio_duration_seconds(std::string_view audio) {
    if (audio.size() >= 12 && audio.substr(0, 4) == "RIFF" && audio.substr(8, 4) == "WAVE") {
        return wav_seconds(audio);
    }
    if (audio.substr(0, 4) == "fLaC") {
        return flac_seconds(audio);
    }
    if (audio.substr(0, 4) == "OggS") {
        return ogg_seconds(audio);
    }
    if (audio.substr(0, 4) == "\x1A\x45\xDF\xA3") {
        return webm_seconds(audio);
    }
    if (audio.size() >= 8 &&
        (audio.substr(4, 4) == "ftyp" || audio.substr(4, 4) == "moov" ||
         audio.substr(4, 4) == "mdat" || audio.substr(4, 4) == "free")) {
        return mp4_seconds(audio);
    }
    if (audio.substr(0, 3) == "ID3" ||
        (audio.size() >= 2 && static_cast<unsigned char>(audio[0]) == 0xFF &&
         (static_cast<unsigned char>(audio[1]) & 0xE0) == 0xE0)) {
        return mp3_seconds(audio);
    }
    return std::nullopt;
}

} // namespace lake::api
