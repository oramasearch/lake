// SPDX-License-Identifier: MIT

#include "lake/api/markdown.hpp"

#include <cctype>
#include <map>
#include <vector>

namespace lake::api {
namespace {

std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

bool safe_href(std::string_view href) {
    if (href.empty()) {
        return false;
    }
    if (href.starts_with("https://") || href.starts_with("mailto:") || href.starts_with('/') ||
        href.starts_with('#')) {
        return !href.starts_with("//");
    }
    // A relative link: no scheme before the first '/', '?' or '#'.
    const std::size_t colon = href.find(':');
    return colon == std::string_view::npos || colon > href.find_first_of("/?#");
}

// Inline markup over one line of source: `code` first (its content is literal), then **bold**,
// *emphasis* and [text](href).
std::string inline_html(std::string_view text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '`') {
            const std::size_t end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                out += "<code>" + escape(text.substr(i + 1, end - i - 1)) + "</code>";
                i = end + 1;
                continue;
            }
        }
        if (c == '*' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t end = text.find("**", i + 2);
            if (end != std::string_view::npos) {
                out += "<strong>" + inline_html(text.substr(i + 2, end - i - 2)) + "</strong>";
                i = end + 2;
                continue;
            }
        }
        if (c == '*' && i + 1 < text.size() && text[i + 1] != ' ') {
            const std::size_t end = text.find('*', i + 1);
            if (end != std::string_view::npos && end > i + 1) {
                out += "<em>" + inline_html(text.substr(i + 1, end - i - 1)) + "</em>";
                i = end + 1;
                continue;
            }
        }
        if (c == '[') {
            const std::size_t close = text.find("](", i + 1);
            const std::size_t end =
                close == std::string_view::npos ? close : text.find(')', close + 2);
            if (end != std::string_view::npos) {
                const std::string_view label = text.substr(i + 1, close - i - 1);
                const std::string_view href = text.substr(close + 2, end - close - 2);
                if (safe_href(href)) {
                    out += "<a href=\"" + escape(href) + "\">" + inline_html(label) + "</a>";
                } else {
                    out += inline_html(label);
                }
                i = end + 1;
                continue;
            }
        }
        out += escape(std::string_view(&text[i], 1));
        ++i;
    }
    return out;
}

std::string_view trim(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    return line;
}

std::vector<std::string_view> cells(std::string_view row) {
    row = trim(row);
    if (row.starts_with('|')) {
        row.remove_prefix(1);
    }
    if (row.ends_with('|')) {
        row.remove_suffix(1);
    }
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= row.size(); ++i) {
        if (i == row.size() || (row[i] == '|' && (i == 0 || row[i - 1] != '\\'))) {
            out.push_back(trim(row.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

bool ordered_item(std::string_view line) {
    std::size_t digits = 0;
    while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits]))) {
        ++digits;
    }
    return digits > 0 && line.substr(digits).starts_with(". ");
}

} // namespace

std::string markdown_anchor(std::string_view heading) {
    std::string out;
    bool dash = false;
    for (const char c : heading) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            if (dash && !out.empty()) {
                out += '-';
            }
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            dash = false;
        } else if (c == ' ' || c == '-') {
            dash = true;
        }
    }
    return out;
}

std::string markdown_to_html(std::string_view source) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= source.size()) {
        const std::size_t end = source.find('\n', start);
        lines.push_back(source.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    std::string out, paragraph;
    std::map<std::string, int> anchors; // a repeated heading gets -2, -3, …
    const auto flush = [&] {
        if (!paragraph.empty()) {
            out += "<p>" + inline_html(paragraph) + "</p>\n";
            paragraph.clear();
        }
    };
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string_view line = trim(lines[i]);
        if (line.empty()) {
            flush();
            continue;
        }
        if (line.starts_with("```")) {
            flush();
            std::string code;
            for (++i; i < lines.size() && !trim(lines[i]).starts_with("```"); ++i) {
                std::string_view raw = lines[i];
                if (raw.ends_with('\r')) {
                    raw.remove_suffix(1);
                }
                code += escape(raw) + "\n";
            }
            out += "<pre><code>" + code + "</code></pre>\n";
            continue;
        }
        if (line.starts_with('#')) {
            flush();
            std::size_t level = 0;
            while (level < line.size() && line[level] == '#') {
                ++level;
            }
            if (level <= 4 && level < line.size() && line[level] == ' ') {
                const std::string_view text = trim(line.substr(level));
                const std::string tag = "h" + std::to_string(level);
                std::string anchor = markdown_anchor(text);
                if (const int seen = ++anchors[anchor]; seen > 1) {
                    anchor += "-" + std::to_string(seen);
                }
                out += "<" + tag + " id=\"" + anchor + "\">" + inline_html(text) +
                       "</" + tag + ">\n";
                continue;
            }
        }
        if (line.starts_with('|') && i + 1 < lines.size() &&
            trim(lines[i + 1]).starts_with("|") &&
            trim(lines[i + 1]).find("---") != std::string_view::npos) {
            flush();
            out += "<table><thead><tr>";
            for (const std::string_view cell : cells(line)) {
                out += "<th>" + inline_html(cell) + "</th>";
            }
            out += "</tr></thead><tbody>";
            for (i += 2; i < lines.size() && trim(lines[i]).starts_with('|'); ++i) {
                out += "<tr>";
                for (const std::string_view cell : cells(lines[i])) {
                    out += "<td>" + inline_html(cell) + "</td>";
                }
                out += "</tr>";
            }
            --i;
            out += "</tbody></table>\n";
            continue;
        }
        const bool bullet = line.starts_with("- ") || line.starts_with("* ");
        if (bullet || ordered_item(line)) {
            flush();
            const std::string tag = bullet ? "ul" : "ol";
            out += "<" + tag + ">";
            for (; i < lines.size(); ++i) {
                const std::string_view item = trim(lines[i]);
                const bool same = bullet ? (item.starts_with("- ") || item.starts_with("* "))
                                         : ordered_item(item);
                if (same) {
                    const std::size_t text = item.find(' ') + 1;
                    // Continuation lines of the same item join it before inline markup, so a
                    // `code` span (a JSON example, lake#439) may wrap across source lines.
                    std::string joined(item.substr(text));
                    while (i + 1 < lines.size() && lines[i + 1].starts_with("  ") &&
                           !trim(lines[i + 1]).empty()) {
                        joined += " " + std::string(trim(lines[++i]));
                    }
                    out += "<li>" + inline_html(joined) + "</li>";
                } else {
                    break;
                }
            }
            --i;
            out += "</" + tag + ">\n";
            continue;
        }
        if (line.starts_with("> ")) {
            flush();
            std::string note;
            for (; i < lines.size() && trim(lines[i]).starts_with('>'); ++i) {
                std::string_view text = trim(lines[i]).substr(1);
                note += (note.empty() ? "" : " ") + std::string(trim(text));
            }
            --i;
            out += "<div class=\"panel notice\"><p>" + inline_html(note) + "</p></div>\n";
            continue;
        }
        paragraph += (paragraph.empty() ? "" : " ") + std::string(line);
    }
    flush();
    return out;
}

} // namespace lake::api
