// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

// The Python templates of methodology drafts (methodology_templates.cpp, lake#655).
namespace lake::control::methodology_templates {

extern const std::string_view helpers;
extern const std::string_view binary;
extern const std::string_view multiclass;
extern const std::string_view regression;
extern const std::string_view clustering;

} // namespace lake::control::methodology_templates
