// SPDX-License-Identifier: MIT

// A lake pod's liquid identity custody (lake#1352). Since liquid v0.3.0 (liquid#402) a new local
// identity on Linux is sealed under a passphrase; a pod has no terminal, so it names an owner-only
// passphrase file the operator mounts from a Secret. These start lake with an empty LIQUID_HOME:
// with a passphrase file it creates a sealed identity; with none it refuses to start, naming the
// variables; an identity already in file custody keeps loading without either.

#include "temporary_directory.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <string>

#include <liquid/identity.hpp>
#include <liquid/runtime.hpp>

#include "identity/taap.hpp"
#include "lake/api/server.hpp"
#include "lake/control/store.hpp"

namespace {

// Sets (or, with nullopt, unsets) one variable for the test's life and restores it afterwards.
class ScopedVariable {
public:
    ScopedVariable(const char* name, std::optional<std::string> value) : name_(name) {
        if (const char* previous = std::getenv(name)) {
            previous_ = previous;
        }
        if (value) {
            ::setenv(name, value->c_str(), 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedVariable() {
        if (previous_) {
            ::setenv(name_, previous_->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

private:
    const char* name_;
    std::optional<std::string> previous_;
};

// A new, empty directory, removed afterwards.
class Directory {
public:
    explicit Directory(const std::string& label)
        : path_(lake::test::fixture_path(label + "-" + lake::control::new_uuid_v4())) {
        std::filesystem::create_directories(path_);
    }
    ~Directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

lake::api::ServerOptions options_for(const std::filesystem::path& store_root) {
    lake::api::ServerOptions options;
    options.port = 0;
    options.node_name = "identity-custody-test";
    options.admin_token = "custody-admin";
    options.instance_kek = "6964656e746974792d637573746f64792d746573742d6b656b2d6865782121";
    options.store_root = store_root.string();
    return options;
}

// What the start-time refusal must name.
void expect_names_the_variables(const std::string& reason) {
    EXPECT_NE(reason.find("LIQUID_IDENTITY_PASSPHRASE_FILE"), std::string::npos) << reason;
    EXPECT_NE(reason.find("LIQUID_IDENTITY_KEY_CUSTODY"), std::string::npos) << reason;
    EXPECT_NE(reason.find("lake cannot create its liquid identity"), std::string::npos) << reason;
}

} // namespace

TEST(IdentityCustody, AnEmptyHomeWithAPassphraseFileGetsASealedIdentity) {
    Directory home("custody-home");
    Directory secret("custody-secret");
    Directory store("custody-store");
    const auto passphrase = secret.path() / "passphrase";
    {
        std::ofstream file(passphrase);
        file << "a passphrase the operator generated once\n";
    }
    std::filesystem::permissions(passphrase, std::filesystem::perms::owner_read);
    ScopedVariable liquid_home("LIQUID_HOME", home.path().string());
    ScopedVariable custody("LIQUID_IDENTITY_KEY_CUSTODY", "passphrase");
    ScopedVariable file("LIQUID_IDENTITY_PASSPHRASE_FILE", passphrase.string());

    lake::api::ApiServer server(options_for(store.path()));
    ASSERT_TRUE(server.start()) << server.reason();
    const auto status = liquid::local_identity_custody();
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->kind, "passphrase");
    EXPECT_FALSE(status->file_permissions_only);
    EXPECT_FALSE(status->clear_copy_present);
    EXPECT_FALSE(std::filesystem::exists(home.path() / "identity" / ".wrapping.key"));
    server.stop();

    // A later use (another process on the same home) opens it with the same file.
    const auto started = std::chrono::steady_clock::now();
    auto reopened = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    ASSERT_TRUE(reopened) << reopened.error().message;
    std::println("opening the passphrase-sealed identity took {} ms", elapsed.count());
}

TEST(IdentityCustody, AnEmptyHomeWithoutAPassphraseFileRefusesToStart) {
    Directory home("custody-home");
    Directory store("custody-store");
    ScopedVariable liquid_home("LIQUID_HOME", home.path().string());
    ScopedVariable file("LIQUID_IDENTITY_PASSPHRASE_FILE", std::nullopt);
    {
        // The custody a pod gets by naming it.
        ScopedVariable custody("LIQUID_IDENTITY_KEY_CUSTODY", "passphrase");
        lake::api::ApiServer server(options_for(store.path()));
        EXPECT_FALSE(server.start());
        expect_names_the_variables(server.reason());
    }
#if !defined(__APPLE__)
    {
        // And the one it gets by default on Linux.
        ScopedVariable custody("LIQUID_IDENTITY_KEY_CUSTODY", std::nullopt);
        lake::api::ApiServer server(options_for(store.path()));
        EXPECT_FALSE(server.start());
        expect_names_the_variables(server.reason());
    }
#endif
    const auto status = liquid::local_identity_custody();
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->kind, "none") << "a refused start creates no identity";
}

// An identity created before liquid v0.3.0, or with file custody, needs no passphrase: a pod
// whose home already holds one keeps starting with neither variable set.
TEST(IdentityCustody, AnExistingFileCustodyIdentityKeepsLoadingWithoutEither) {
    Directory home("custody-home");
    Directory store("custody-store");
    ScopedVariable liquid_home("LIQUID_HOME", home.path().string());
    ScopedVariable file("LIQUID_IDENTITY_PASSPHRASE_FILE", std::nullopt);
    {
        ScopedVariable custody("LIQUID_IDENTITY_KEY_CUSTODY", "file");
        auto created = liquid::runtime::block_on(liquid::identity_detail::local_signer());
        ASSERT_TRUE(created) << created.error().message;
    }
    ScopedVariable custody("LIQUID_IDENTITY_KEY_CUSTODY", std::nullopt);
    lake::api::ApiServer server(options_for(store.path()));
    ASSERT_TRUE(server.start()) << server.reason();
    const auto status = liquid::local_identity_custody();
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status->kind, "file");
    server.stop();
}
