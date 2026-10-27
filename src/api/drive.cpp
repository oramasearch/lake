#include "lake/api/drive.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <set>
#include <span>
#include <sstream>

#include <nlohmann/json.hpp>

#include "lake/api/web_ui.hpp"

namespace lake::api {
namespace {

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string extension_of(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) ||
        dot + 1 == path.size()) {
        return {};
    }
    return lower(std::string(path.substr(dot)));
}

std::string file_text(const RepositoryFile& file) {
    if (file.raw_contents.empty()) {
        return file.contents;
    }
    return std::string(reinterpret_cast<const char*>(file.raw_contents.data()),
                       file.raw_contents.size());
}

std::string base64(std::span<const std::byte> bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const unsigned first = std::to_integer<unsigned>(bytes[offset]);
        const unsigned second =
            offset + 1 < bytes.size() ? std::to_integer<unsigned>(bytes[offset + 1]) : 0;
        const unsigned third =
            offset + 2 < bytes.size() ? std::to_integer<unsigned>(bytes[offset + 2]) : 0;
        const unsigned value = first << 16U | second << 8U | third;
        output += alphabet[(value >> 18U) & 0x3fU];
        output += alphabet[(value >> 12U) & 0x3fU];
        output += offset + 1 < bytes.size() ? alphabet[(value >> 6U) & 0x3fU] : '=';
        output += offset + 2 < bytes.size() ? alphabet[value & 0x3fU] : '=';
    }
    return output;
}

DrivePreviewResult text_preview(const RepositoryFile& file) {
    return DrivePreview{"text",
                        "text/plain",
                        "<pre class=\"file-preview\">" + web_ui_escape(file_text(file)) + "</pre>"};
}

DrivePreviewResult markdown_preview(const RepositoryFile& file) {
    std::istringstream input(file_text(file));
    std::string line;
    std::string html = "<article class=\"markdown-preview\">";
    bool list_open = false;
    while (std::getline(input, line)) {
        if (line.starts_with("# ")) {
            if (list_open) {
                html += "</ul>";
                list_open = false;
            }
            html += "<h2>" + web_ui_escape(line.substr(2)) + "</h2>";
        } else if (line.starts_with("## ")) {
            if (list_open) {
                html += "</ul>";
                list_open = false;
            }
            html += "<h3>" + web_ui_escape(line.substr(3)) + "</h3>";
        } else if (line.starts_with("- ")) {
            if (!list_open) {
                html += "<ul>";
                list_open = true;
            }
            html += "<li>" + web_ui_escape(line.substr(2)) + "</li>";
        } else if (!line.empty()) {
            if (list_open) {
                html += "</ul>";
                list_open = false;
            }
            html += "<p>" + web_ui_escape(line) + "</p>";
        }
    }
    if (list_open) {
        html += "</ul>";
    }
    html += "</article>";
    return DrivePreview{"markdown", "text/markdown", std::move(html)};
}

std::vector<std::vector<std::string>> parse_csv(std::string_view text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '"') {
            if (quoted && index + 1 < text.size() && text[index + 1] == '"') {
                field += '"';
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            row.push_back(std::move(field));
            field.clear();
        } else if ((character == '\n' || character == '\r') && !quoted) {
            if (character == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
                ++index;
            }
            row.push_back(std::move(field));
            field.clear();
            rows.push_back(std::move(row));
            row.clear();
        } else {
            field += character;
        }
    }
    if (!field.empty() || !row.empty()) {
        row.push_back(std::move(field));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string table_html(const std::vector<std::vector<std::string>>& rows) {
    if (rows.empty()) {
        return "<p>No rows.</p>";
    }
    std::string html = "<div class=\"table-wrap\"><table><thead><tr>";
    for (const std::string& field : rows.front()) {
        html += "<th>" + web_ui_escape(field) + "</th>";
    }
    html += "</tr></thead><tbody>";
    for (auto row = rows.begin() + 1; row != rows.end(); ++row) {
        html += "<tr>";
        for (const std::string& field : *row) {
            html += "<td>" + web_ui_escape(field) + "</td>";
        }
        html += "</tr>";
    }
    html += "</tbody></table></div>";
    return html;
}

DrivePreviewResult csv_preview(const RepositoryFile& file) {
    return DrivePreview{"csv", "text/csv", table_html(parse_csv(file_text(file)))};
}

DrivePreviewResult json_preview(const RepositoryFile& file) {
    const nlohmann::json value = nlohmann::json::parse(file_text(file), nullptr, false);
    if (value.is_discarded()) {
        return DrivePreviewResult::failure("file is not valid JSON");
    }
    if (!value.is_array() || value.empty() || !value.front().is_object()) {
        return DrivePreview{"json",
                            "application/json",
                            "<pre class=\"file-preview\">" + web_ui_escape(value.dump(2)) +
                                "</pre>"};
    }
    std::set<std::string> keys;
    for (const auto& item : value) {
        if (item.is_object()) {
            for (const auto& [key, unused] : item.items()) {
                static_cast<void>(unused);
                keys.insert(key);
            }
        }
    }
    std::vector<std::vector<std::string>> rows;
    rows.emplace_back(keys.begin(), keys.end());
    for (const auto& item : value) {
        std::vector<std::string> row;
        for (const std::string& key : keys) {
            const auto found = item.find(key);
            row.push_back(found == item.end()  ? ""
                          : found->is_string() ? found->get<std::string>()
                                               : found->dump());
        }
        rows.push_back(std::move(row));
    }
    return DrivePreview{"json", "application/json", table_html(rows)};
}

DrivePreviewResult image_preview(const RepositoryFile& file, std::string media_type) {
    std::vector<std::byte> bytes = file.raw_contents;
    if (bytes.empty()) {
        bytes.reserve(file.contents.size());
        for (const char character : file.contents) {
            bytes.push_back(static_cast<std::byte>(character));
        }
    }
    return DrivePreview{"image",
                        media_type,
                        "<figure><img class=\"image-preview\" alt=\"" + web_ui_escape(file.path) +
                            "\" src=\"data:" + media_type + ";base64," + base64(bytes) +
                            "\"><figcaption>" + web_ui_escape(file.path) + " · " +
                            std::to_string(file.size) + " bytes</figcaption></figure>"};
}

DrivePreviewResult pdf_preview(const RepositoryFile& file) {
    std::vector<std::byte> bytes = file.raw_contents;
    if (bytes.empty()) {
        bytes.reserve(file.contents.size());
        for (const char character : file.contents) {
            bytes.push_back(static_cast<std::byte>(character));
        }
    }
    return DrivePreview{
        "pdf",
        "application/pdf",
        "<object class=\"document-preview\" data=\"data:application/pdf;base64," + base64(bytes) +
            "\" type=\"application/pdf\"><p>This browser cannot display the PDF.</p></object>"};
}

DrivePreviewResult
media_preview(const RepositoryFile& file, std::string format, std::string media_type) {
    std::vector<std::byte> bytes = file.raw_contents;
    if (bytes.empty()) {
        bytes.reserve(file.contents.size());
        for (const char character : file.contents) {
            bytes.push_back(static_cast<std::byte>(character));
        }
    }
    const std::string element = format == "video" ? "video" : "audio";
    return DrivePreview{std::move(format),
                        media_type,
                        "<" + element +
                            " class=\"media-preview\" controls preload=\"metadata\" "
                            "src=\"data:" +
                            media_type + ";base64," + base64(bytes) + "\"></" + element + ">"};
}

std::vector<std::string> text_lines(std::string_view text) {
    std::vector<std::string> output;
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t newline = text.find('\n', offset);
        const std::size_t size =
            newline == std::string_view::npos ? text.size() - offset : newline - offset;
        output.emplace_back(text.substr(offset, size));
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }
    if (text.empty() || text.back() == '\n') {
        output.emplace_back();
    }
    return output;
}

} // namespace

DrivePreviewResult::DrivePreviewResult(DrivePreview preview) : preview_(std::move(preview)) {}

DrivePreviewResult DrivePreviewResult::failure(std::string message) {
    DrivePreviewResult result;
    result.error_ = std::move(message);
    return result;
}

DrivePreviewResult::operator bool() const {
    return preview_.has_value();
}

const DrivePreview& DrivePreviewResult::operator*() const {
    return *preview_;
}

const DrivePreview* DrivePreviewResult::operator->() const {
    return &*preview_;
}

const std::string& DrivePreviewResult::error() const {
    return error_;
}

bool DriveFormatRegistry::add(std::string extension, DrivePreviewRenderer renderer) {
    extension = lower(std::move(extension));
    if (extension.empty() || extension.front() != '.' || !renderer ||
        renderers_.contains(extension)) {
        return false;
    }
    extensions_.push_back(extension);
    renderers_.emplace(std::move(extension), std::move(renderer));
    return true;
}

DrivePreviewResult DriveFormatRegistry::preview(const RepositoryFile& file) const {
    const std::string extension = extension_of(file.path);
    if (!renderers_.contains(extension)) {
        return DrivePreview{
            "download", "application/octet-stream", "<p>No preview is available.</p>"};
    }
    DrivePreviewResult rendered = renderers_.at(extension)(file);
    // A text format shows the file's text exactly, so a file that is not UTF-8 has no preview
    // rather than an altered one (lake#1238); it is downloaded instead.
    if (rendered && drive_preview_is_text(*rendered) && !repository_file_text(file)) {
        return DrivePreview{"download",
                            "application/octet-stream",
                            "<p>This file is not UTF-8 text, so it has no preview. Download it "
                            "to see it.</p>"};
    }
    return rendered;
}

std::vector<std::string> DriveFormatRegistry::extensions() const {
    return extensions_;
}

DriveFormatRegistry DriveFormatRegistry::defaults() {
    DriveFormatRegistry registry;
    registry.add(".txt", text_preview);
    registry.add(".md", markdown_preview);
    registry.add(".markdown", markdown_preview);
    registry.add(".c", text_preview);
    registry.add(".cc", text_preview);
    registry.add(".cpp", text_preview);
    registry.add(".css", text_preview);
    registry.add(".h", text_preview);
    registry.add(".hpp", text_preview);
    registry.add(".html", text_preview);
    registry.add(".js", text_preview);
    registry.add(".log", text_preview);
    registry.add(".toml", text_preview);
    registry.add(".xml", text_preview);
    registry.add(".yaml", text_preview);
    registry.add(".yml", text_preview);
    registry.add(".csv", csv_preview);
    registry.add(".json", json_preview);
    registry.add(".png",
                 [](const RepositoryFile& file) { return image_preview(file, "image/png"); });
    registry.add(".jpg",
                 [](const RepositoryFile& file) { return image_preview(file, "image/jpeg"); });
    registry.add(".jpeg",
                 [](const RepositoryFile& file) { return image_preview(file, "image/jpeg"); });
    registry.add(".gif",
                 [](const RepositoryFile& file) { return image_preview(file, "image/gif"); });
    registry.add(".webp",
                 [](const RepositoryFile& file) { return image_preview(file, "image/webp"); });
    registry.add(".pdf", pdf_preview);
    registry.add(".mp4", [](const RepositoryFile& file) {
        return media_preview(file, "video", "video/mp4");
    });
    registry.add(".webm", [](const RepositoryFile& file) {
        return media_preview(file, "video", "video/webm");
    });
    registry.add(".ogv", [](const RepositoryFile& file) {
        return media_preview(file, "video", "video/ogg");
    });
    registry.add(".mp3", [](const RepositoryFile& file) {
        return media_preview(file, "audio", "audio/mpeg");
    });
    registry.add(".m4a", [](const RepositoryFile& file) {
        return media_preview(file, "audio", "audio/mp4");
    });
    registry.add(".ogg", [](const RepositoryFile& file) {
        return media_preview(file, "audio", "audio/ogg");
    });
    registry.add(".wav", [](const RepositoryFile& file) {
        return media_preview(file, "audio", "audio/wav");
    });
    return registry;
}

bool drive_preview_is_text(const DrivePreview& preview) {
    return preview.format == "text" || preview.format == "markdown" || preview.format == "csv" ||
           preview.format == "json";
}

DriveVersionDiff drive_text_diff(RepositoryFile left, RepositoryFile right) {
    const std::vector<std::string> left_lines = text_lines(file_text(left));
    const std::vector<std::string> right_lines = text_lines(file_text(right));
    std::size_t prefix = 0;
    while (prefix < left_lines.size() && prefix < right_lines.size() &&
           left_lines[prefix] == right_lines[prefix]) {
        ++prefix;
    }
    std::size_t left_suffix = left_lines.size();
    std::size_t right_suffix = right_lines.size();
    while (left_suffix > prefix && right_suffix > prefix &&
           left_lines[left_suffix - 1] == right_lines[right_suffix - 1]) {
        --left_suffix;
        --right_suffix;
    }
    std::vector<DriveDiffLine> lines;
    for (std::size_t index = 0; index < prefix; ++index) {
        lines.push_back({"context", left_lines[index]});
    }
    for (std::size_t index = prefix; index < left_suffix; ++index) {
        lines.push_back({"removed", left_lines[index]});
    }
    for (std::size_t index = prefix; index < right_suffix; ++index) {
        lines.push_back({"added", right_lines[index]});
    }
    for (std::size_t index = left_suffix; index < left_lines.size(); ++index) {
        lines.push_back({"context", left_lines[index]});
    }
    return DriveVersionDiff{std::move(left), std::move(right), std::move(lines)};
}

} // namespace lake::api
