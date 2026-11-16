// SPDX-License-Identifier: MIT

#pragma once

// Drive exercises shared by the memory, Postgres and workspace-store suites. Every step goes
// through lake's HTTP API.
// - lake#123: an ordinary overwrite through `upload_drive_files` keeps the previous version as a
//   normal prior version that can be compared and restored, a restore is a stable-node
//   modification, and a real deletion stays distinct.
// - lake#560: the first upload into a repository with no `main` creates `main`, and an upload
//   into a folder that does not exist yet creates the folder.

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::test {

// Sends one request to the running lake and answers the HTTP status and the raw body.
using DriveRequest = std::function<std::pair<unsigned, std::string>(
    std::string_view method, const std::string& path, const std::string& body)>;

inline std::string drive_base64(std::string_view text) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    std::size_t index = 0;
    for (; index + 2 < text.size(); index += 3) {
        const unsigned value = (static_cast<unsigned char>(text[index]) << 16U) |
                               (static_cast<unsigned char>(text[index + 1]) << 8U) |
                               static_cast<unsigned char>(text[index + 2]);
        encoded += alphabet[(value >> 18U) & 63U];
        encoded += alphabet[(value >> 12U) & 63U];
        encoded += alphabet[(value >> 6U) & 63U];
        encoded += alphabet[value & 63U];
    }
    if (index < text.size()) {
        unsigned value = static_cast<unsigned char>(text[index]) << 16U;
        if (index + 1 < text.size()) {
            value |= static_cast<unsigned char>(text[index + 1]) << 8U;
        }
        encoded += alphabet[(value >> 18U) & 63U];
        encoded += alphabet[(value >> 12U) & 63U];
        encoded += index + 1 < text.size() ? alphabet[(value >> 6U) & 63U] : '=';
        encoded += '=';
    }
    return encoded;
}

// Uploads `shared/hello.txt` twice into `repository_id`, whose main branch already exists, then
// checks history, comparison, restore and deletion.
inline void expect_replaced_drive_file_keeps_its_history(const DriveRequest& request,
                                                         const std::string& repository_id) {
    using nlohmann::json;
    const std::string path = "shared/hello.txt";
    const std::string context = "repository_id=" + repository_id + "&ref=main&path=" + path;
    const auto api = [&](std::string_view method, const std::string& capability,
                         const json& input) -> json {
        const std::string suffix =
            method == "GET" ? "/v1/" + capability + "?" + input.get<std::string>()
                            : "/v1/" + capability;
        const auto [status, body] =
            request(method, suffix, method == "GET" ? std::string() : input.dump());
        const json parsed = json::parse(body, nullptr, false);
        EXPECT_EQ(status, 200U) << capability << ": " << body;
        EXPECT_TRUE(parsed.is_object() && parsed.value("ok", false)) << capability << ": " << body;
        return parsed.is_object() ? parsed.value("value", json::object()) : json::object();
    };
    const auto upload = [&](std::string_view text) {
        return api("POST",
                   "upload_drive_files",
                   {{"repository_id", repository_id},
                    {"ref", "main"},
                    {"path", "shared"},
                    {"files", json::array({{{"name", "hello.txt"},
                                            {"content", drive_base64(text)}}})}});
    };
    const auto history = [&]() { return api("GET", "drive_history", json(context))["items"]; };

    const json first = upload("Version 1\n");
    const json second = upload("Version 2\n");
    ASSERT_FALSE(first.value("change", std::string()).empty()) << first.dump();
    ASSERT_FALSE(second.value("change", std::string()).empty()) << second.dump();
    ASSERT_NE(first["change"], second["change"]);

    // Two available versions: the replacement is current, the first upload a normal prior one.
    const json replaced = history();
    ASSERT_EQ(replaced.size(), 2U) << replaced.dump();
    const json& current = replaced[0];
    const json& previous = replaced[1];
    EXPECT_EQ(current["id"], second["change"]) << replaced.dump();
    EXPECT_EQ(previous["id"], first["change"]) << replaced.dump();
    EXPECT_EQ(current["path"], path);
    EXPECT_EQ(current["changes"], json::array({"modified"})) << replaced.dump();
    EXPECT_EQ(previous["changes"], json::array({"added"})) << replaced.dump();
    ASSERT_FALSE(current.value("new_blob", std::string()).empty()) << replaced.dump();
    ASSERT_FALSE(previous.value("new_blob", std::string()).empty()) << replaced.dump();
    EXPECT_NE(current["new_blob"], previous["new_blob"]);
    EXPECT_EQ(current["old_blob"], previous["new_blob"])
        << "the replacement names the version it replaced";
    const json preview = api("GET", "drive_preview", json(context));
    EXPECT_NE(preview.dump().find("Version 2"), std::string::npos) << preview.dump();

    // The two versions compare.
    const json compared = api("GET",
                              "diff_drive_versions",
                              json(context + "&left=" + previous["id"].get<std::string>() +
                                   "&right=" + current["id"].get<std::string>()));
    EXPECT_EQ(compared["lines"],
              json::array({{{"kind", "removed"}, {"text", "Version 1"}},
                           {{"kind", "added"}, {"text", "Version 2"}},
                           {{"kind", "context"}, {"text", ""}}}))
        << compared.dump();

    // Restoring the previous version writes a new change: a modification of the same node.
    const json restored = api("POST",
                              "restore_drive_version",
                              {{"repository_id", repository_id},
                               {"ref", "main"},
                               {"path", path},
                               {"change_id", previous["id"]}});
    ASSERT_FALSE(restored.value("id", std::string()).empty()) << restored.dump();
    const json after_restore = history();
    ASSERT_EQ(after_restore.size(), 3U) << after_restore.dump();
    EXPECT_EQ(after_restore[0]["id"], restored["id"]);
    EXPECT_EQ(after_restore[0]["changes"], json::array({"modified"})) << after_restore.dump();
    EXPECT_EQ(after_restore[0]["new_blob"], previous["new_blob"]);
    EXPECT_EQ(after_restore[0]["old_blob"], current["new_blob"]);
    const json read_back = api("GET", "drive_preview", json(context));
    EXPECT_NE(read_back.dump().find("Version 1"), std::string::npos) << read_back.dump();

    // A real deletion stays distinct and unavailable at its revision.
    api("POST",
        "delete_drive_path",
        {{"repository_id", repository_id}, {"ref", "main"}, {"path", path}});
    const json deleted = history();
    ASSERT_EQ(deleted.size(), 4U) << deleted.dump();
    EXPECT_EQ(deleted[0]["changes"], json::array({"removed"})) << deleted.dump();
    EXPECT_EQ(deleted[0]["new_blob"], "");
    EXPECT_EQ(deleted[0]["old_blob"], previous["new_blob"]);
}

// lake#560: the first upload into a repository nobody has written to (no `main`) creates `main`;
// an upload into a folder that does not exist, nested two deep, creates it as a file manager
// would; an upload under a file is still refused. `starts_without_main` false runs the folder
// steps on a repository whose `main` already exists, with none of these paths.
inline void expect_uploads_create_main_and_missing_folders(const DriveRequest& request,
                                                           const std::string& repository_id,
                                                           bool starts_without_main = true) {
    using nlohmann::json;
    const auto send = [&](std::string_view method,
                          const std::string& capability,
                          const json& input) -> std::pair<unsigned, json> {
        const std::string suffix = method == "GET"
                                       ? "/v1/" + capability + "?" + input.get<std::string>()
                                       : "/v1/" + capability;
        const auto [status, body] =
            request(method, suffix, method == "GET" ? std::string() : input.dump());
        return {status, json::parse(body, nullptr, false)};
    };
    const auto ok =
        [&](std::string_view method, const std::string& capability, const json& input) -> json {
        const auto [status, parsed] = send(method, capability, input);
        EXPECT_EQ(status, 200U) << capability << ": " << parsed.dump();
        EXPECT_TRUE(parsed.is_object() && parsed.value("ok", false))
            << capability << ": " << parsed.dump();
        return parsed.is_object() ? parsed.value("value", json::object()) : json::object();
    };
    const auto upload_input = [&](const std::string& folder,
                                  const std::string& name,
                                  std::string_view text) {
        return json{{"repository_id", repository_id},
                    {"ref", "main"},
                    {"path", folder},
                    {"files", json::array({{{"name", name}, {"content", drive_base64(text)}}})}};
    };
    const auto browse = [&](const std::string& path) {
        std::vector<std::string> paths;
        for (const json& item :
             ok("GET",
                "drive_browse",
                json("repository_id=" + repository_id + "&ref=main&path=" + path))["items"]) {
            paths.push_back(item.value("path", std::string()));
        }
        std::ranges::sort(paths);
        return paths;
    };
    const auto with = [](std::vector<std::string> paths, std::vector<std::string> added) {
        paths.insert(paths.end(), added.begin(), added.end());
        std::ranges::sort(paths);
        return paths;
    };

    std::vector<std::string> top;
    if (starts_without_main) {
        const auto [unborn, before] =
            send("GET", "drive_browse", json("repository_id=" + repository_id + "&ref=main&path="));
        ASSERT_NE(unborn, 200U) << "the repository starts with no main: " << before.dump();
    } else {
        top = browse("");
    }

    const json first = ok("POST", "upload_drive_files", upload_input("", "first.txt", "First\n"));
    ASSERT_FALSE(first.value("change", std::string()).empty()) << first.dump();
    EXPECT_EQ(browse(""), with(top, {"first.txt"})) << "the upload wrote main";

    const json nested =
        ok("POST", "upload_drive_files", upload_input("Reports/2026", "q3.csv", "id,total\n1,9\n"));
    ASSERT_FALSE(nested.value("change", std::string()).empty()) << nested.dump();
    EXPECT_EQ(browse(""), with(top, {"Reports", "first.txt"}));
    EXPECT_EQ(browse("Reports"), (std::vector<std::string>{"Reports/2026"}));
    EXPECT_EQ(browse("Reports/2026"), (std::vector<std::string>{"Reports/2026/q3.csv"}));
    const json preview =
        ok("GET",
           "drive_preview",
           json("repository_id=" + repository_id + "&ref=main&path=Reports/2026/q3.csv"));
    EXPECT_NE(preview.dump().find("<th>total</th>"), std::string::npos) << preview.dump();

    const auto [under_file, refused] =
        send("POST", "upload_drive_files", upload_input("first.txt", "inside.txt", "No\n"));
    EXPECT_EQ(under_file, 400U) << "a file is not a folder: " << refused.dump();
    EXPECT_EQ(refused["error"]["code"], "invalid_input") << refused.dump();
    EXPECT_EQ(browse(""), with(top, {"Reports", "first.txt"}));
}

} // namespace lake::test
