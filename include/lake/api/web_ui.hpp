#pragma once
#include <string>
#include <string_view>

// The web interface, in Zetta's shape (zetta/api/web_ui.hpp): server-rendered
// straight HTML, one self-contained document per page, inline-free styling served
// from GET /style.css, no framework, no external assets. These are PURE renderers —
// data in, HTML out. The http_server adapter does the I/O and hands the data in.
namespace lake::api {

// HTML-escape text for safe interpolation into an element body or attribute.
std::string web_ui_escape(std::string_view text);

// The stylesheet, served at GET /style.css.
std::string web_ui_stylesheet();

// Wrap `content` in the full page shell. `title` is the document title, `meta`
// the mono meta line (node · version).
std::string web_ui_shell(std::string_view title, std::string_view meta, std::string_view content);

// The hello page: what a fresh lake node says about itself.
std::string web_ui_hello(std::string_view node_name, std::string_view version);

}  // namespace lake::api
