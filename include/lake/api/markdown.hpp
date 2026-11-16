// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

// The markdown lake's Docs section is written in (docs/guide/, lake#252), rendered to HTML: #–####
// headings (each with an id from its text), paragraphs, - and 1. lists, > notes, ``` code blocks,
// | tables | with a header row, and inline `code`, **bold**, *emphasis* and [links](href). Every
// character of the source is escaped before markup is added, so a guide cannot inject HTML, and a
// link must be relative, #anchor, https:// or mailto:.
namespace lake::api {

std::string markdown_to_html(std::string_view source);

// A heading's id: lower case, words joined by '-', anything else dropped.
std::string markdown_anchor(std::string_view heading);

} // namespace lake::api
