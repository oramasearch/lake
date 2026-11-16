// SPDX-License-Identifier: MIT

#pragma once

// What a lake image needs from whoever runs it (lake#1119, orama-operator#936), printed by
// `lake manifest`: its version and revision, the schema it carries and how to apply it
// (`lake migrate`), the Stripe webhook events it handles and the products it bills under, and
// every environment variable it reads. The operator reads this from the image a Lake names, so a
// lake release is only that image. docs/control-plane/schema.md.

#include <span>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lake::api {

// One environment variable lake reads. `required`: a Lake run by the operator does not work
// without it. `secret`: its value is a credential, so it comes from a Secret.
struct EnvironmentVariable {
    std::string_view name;
    bool required = false;
    bool secret = false;
    std::string_view description;
};

// Every environment variable src/main.cpp reads, sorted by name. LakeManifest.* fails when
// main.cpp reads one not listed, or one listed is no longer read.
std::span<const EnvironmentVariable> environment_variables();

// The manifest document (manifest_version 1).
nlohmann::json operator_manifest();

} // namespace lake::api
