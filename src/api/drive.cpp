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
        return ".txt";
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
    const std::string text = file_text(file);
    const std::size_t version_start = text.find("%PDF-");
    const std::string version =
        version_start == std::string::npos || version_start + 8 > text.size()
            ? "unknown"
            : text.substr(version_start + 5, 3);
    std::size_t pages = 0;
    std::size_t offset = 0;
    while ((offset = text.find("/Type /Page", offset)) != std::string::npos) {
        const std::size_t suffix = offset + 11;
        if (suffix >= text.size() || text[suffix] != 's') {
            ++pages;
        }
        offset = suffix;
    }
    std::string title;
    const std::size_t title_start = text.find("/Title (");
    if (title_start != std::string::npos) {
        const std::size_t value_start = title_start + 8;
        const std::size_t value_end = text.find(')', value_start);
        if (value_end != std::string::npos) {
            title = text.substr(value_start, value_end - value_start);
        }
    }
    std::string html = "<dl><dt>Format</dt><dd>PDF " + web_ui_escape(version) +
                       "</dd><dt>Pages</dt><dd>" + std::to_string(pages) +
                       "</dd><dt>Size</dt><dd>" + std::to_string(file.size) + " bytes</dd>";
    if (!title.empty()) {
        html += "<dt>Title</dt><dd>" + web_ui_escape(title) + "</dd>";
    }
    html += "</dl>";
    return DrivePreview{"pdf", "application/pdf", std::move(html)};
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
        return DrivePreviewResult::failure("no preview renderer is registered for " + extension);
    }
    return renderers_.at(extension)(file);
}

std::vector<std::string> DriveFormatRegistry::extensions() const {
    return extensions_;
}

DriveFormatRegistry DriveFormatRegistry::defaults() {
    DriveFormatRegistry registry;
    registry.add(".txt", text_preview);
    registry.add(".md", markdown_preview);
    registry.add(".markdown", markdown_preview);
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
    return registry;
}

} // namespace lake::api
