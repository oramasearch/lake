// Every bucket and filesystem provider a store drive browses (lake#664, docs/api/storage.md): one
// conformance suite, run against each provider's StoreContents over a local fake of the provider —
// an S3 server that checks SigV4, an Azure Blob server that checks Shared Key or a SAS, the
// workspace Zetta's records and object routes, and a liquid repository branch. The signers are
// pinned to published or independently computed vectors.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "api/store_repository.hpp"
#include "lake/control/drive_files.hpp"
#include "lake/control/store_buckets.hpp"
#include "lake/control/store_kept.hpp"
#include "lake/control/workspace_records.hpp"
#include "record_fixture.hpp"
#include "zetta_object_fixture.hpp"

#include "store_provider_fakes.hpp"

namespace {

using namespace lake::test::stores;
using json = nlohmann::json;
using lake::control::StoreContents;
using lake::control::StoreItem;

constexpr std::string_view kek = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

// ---- The conformance suite ----

template <class T> T done(lake::control::ZettaControlResult<T> result, const std::string& what) {
    EXPECT_TRUE(result) << what << ": " << (result ? "" : result.error().code + " " + result.error().message);
    if constexpr (!std::is_void_v<T>) {
        return result ? std::move(*result) : T{};
    }
}

template <class Task> auto run(Task task) {
    auto completed = liquid::runtime::block_on(std::move(task));
    EXPECT_TRUE(completed);
    return std::move(*completed);
}

std::vector<std::string> names(StoreContents& contents, const std::string& folder) {
    std::vector<std::string> out;
    for (const StoreItem& item : done(run(contents.list(folder)), "list " + folder)) {
        out.push_back((item.folder ? "d:" : "f:") + item.path);
    }
    return out;
}

std::string store_name_of(const std::string& path) {
    return lake::control::store_name(path);
}

// What every provider's contents must do (store_contents.hpp): the same script, the same
// answers.
void conforms(StoreContents& contents, const std::string& label) {
    SCOPED_TRACE(label);
    EXPECT_TRUE(names(contents, "").empty());
    const auto top = done(run(contents.stat("")), "stat top");
    ASSERT_TRUE(top);
    EXPECT_TRUE(top->folder);

    // Folders, explicit and implied.
    done(run(contents.make_folder("docs")), "make docs");
    done(run(contents.make_folder("docs")), "make docs again");
    const auto docs = done(run(contents.stat("docs")), "stat docs");
    ASSERT_TRUE(docs);
    EXPECT_TRUE(docs->folder);
    EXPECT_EQ(names(contents, ""), std::vector<std::string>{"d:docs"});

    // Files: write, stat, ranged read, replace.
    const StoreItem written = done(run(contents.write("docs/a.txt", "hello", "text/plain")), "write");
    EXPECT_EQ(written.path, "docs/a.txt");
    EXPECT_EQ(written.size, 5U);
    const auto a = done(run(contents.stat("docs/a.txt")), "stat a");
    ASSERT_TRUE(a);
    EXPECT_FALSE(a->folder);
    EXPECT_EQ(a->size, 5U);
    EXPECT_EQ(done(run(contents.read("docs/a.txt", 0, 5)), "read"), "hello");
    EXPECT_EQ(done(run(contents.read("docs/a.txt", 1, 3)), "read range"), "ell");
    done(run(contents.write("docs/a.txt", "hello world", "text/plain")), "replace");
    EXPECT_EQ(done(run(contents.stat("docs/a.txt")), "stat replaced")->size, 11U);
    EXPECT_EQ(done(run(contents.read("docs/a.txt", 6, 100)), "read tail"), "world");
    done(run(contents.write("notes/deep/b.bin", std::string("\0\1\2", 3), "")), "write deep");
    EXPECT_EQ(names(contents, ""), (std::vector<std::string>{"d:docs", "d:notes"}));
    EXPECT_EQ(names(contents, "notes"), std::vector<std::string>{"d:notes/deep"});
    EXPECT_EQ(names(contents, "notes/deep"), std::vector<std::string>{"f:notes/deep/b.bin"});
    EXPECT_EQ(done(run(contents.read("notes/deep/b.bin", 0, 3)), "read binary"), std::string("\0\1\2", 3));
    const std::string named = "docs/my file é & <co>.txt";
    done(run(contents.write(named, "x", "text/plain")), "write a name with spaces and symbols");
    EXPECT_EQ(names(contents, "docs"), (std::vector<std::string>{"f:docs/a.txt", "f:" + named}));
    EXPECT_FALSE(done(run(contents.stat("docs/a")), "stat a prefix of a name"));
    EXPECT_FALSE(done(run(contents.stat("nothing")), "stat nothing"));

    // Conflicts: a file over a folder, a folder over a file, a file under a file.
    const auto refused = [&](auto result, const std::string& what) {
        ASSERT_FALSE(result) << what;
        EXPECT_EQ(result.error().code, "conflict") << what << ": " << result.error().message;
    };
    refused(run(contents.write("docs", "x", "")), "a file over a folder");
    refused(run(contents.make_folder("docs/a.txt")), "a folder over a file");
    refused(run(contents.write("docs/a.txt/x", "x", "")), "a file under a file");

    // Moves: a file, a folder with everything in it, never over something.
    done(run(contents.move("docs/a.txt", "docs/c.txt")), "move a file");
    EXPECT_FALSE(done(run(contents.stat("docs/a.txt")), "stat moved away"));
    EXPECT_EQ(done(run(contents.read("docs/c.txt", 0, 11)), "read moved"), "hello world");
    done(run(contents.move("notes", "archive/notes")), "move a folder");
    EXPECT_FALSE(done(run(contents.stat("notes")), "stat moved folder"));
    EXPECT_EQ(names(contents, "archive/notes/deep"), std::vector<std::string>{"f:archive/notes/deep/b.bin"});
    EXPECT_EQ(done(run(contents.read("archive/notes/deep/b.bin", 0, 3)), "read moved deep"),
              std::string("\0\1\2", 3));
    const auto over = run(contents.move("docs/c.txt", "docs/" + store_name_of(named)));
    EXPECT_FALSE(over) << "a move never replaces what is there";

    // Removal: a folder with everything in it, then a file; the emptied folder stays.
    done(run(contents.remove("archive")), "remove a folder");
    EXPECT_FALSE(done(run(contents.stat("archive")), "stat removed folder"));
    EXPECT_FALSE(done(run(contents.stat("archive/notes/deep/b.bin")), "stat removed file"));
    done(run(contents.remove("docs/c.txt")), "remove a file");
    done(run(contents.remove(named)), "remove the named file");
    EXPECT_EQ(names(contents, ""), std::vector<std::string>{"d:docs"});
    EXPECT_TRUE(names(contents, "docs").empty());
    const auto gone = run(contents.remove("docs/nothing"));
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().code, "not_found");
}

lake::control::StoreClock fixed_clock() {
    return [] { return at(2026, 10, 11, 12); };
}

} // namespace

TEST(StoreProviders, SigV4MatchesTheAwsExamples) {
    // "GET Object" and "GET Bucket (List Objects)" from the AWS Signature Version 4 examples for
    // Amazon S3 (sig-v4-header-based-auth), with the example credentials.
    const lake::control::SigV4Credentials credentials{
        "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", "", "us-east-1"};
    const std::string empty = sha256_hex("");
    const auto object = lake::control::sigv4_sign(
        {"GET", "examplebucket.s3.amazonaws.com", "/test.txt", {}, {{"range", "bytes=0-9"}}, empty},
        credentials, at(2013, 5, 24));
    const auto authorization = std::ranges::find_if(object, [](const auto& pair) { return pair.first == "authorization"; });
    ASSERT_NE(authorization, object.end());
    EXPECT_EQ(authorization->second,
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
              "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    const auto listing = lake::control::sigv4_sign(
        {"GET", "examplebucket.s3.amazonaws.com", "/", {{"max-keys", "2"}, {"prefix", "J"}}, {}, empty},
        credentials, at(2013, 5, 24));
    const auto signed_listing = std::ranges::find_if(listing, [](const auto& pair) { return pair.first == "authorization"; });
    ASSERT_NE(signed_listing, listing.end());
    EXPECT_TRUE(signed_listing->second.ends_with(
        "Signature=34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"))
        << signed_listing->second;
    EXPECT_EQ(lake::control::sigv4_encode("a b/é~", true), "a%20b/%C3%A9~");
    EXPECT_EQ(lake::control::sigv4_encode("a/b", false), "a%2Fb");
}

TEST(StoreProviders, AzureSharedKeyMatchesAnIndependentSigner) {
    // Computed by a separate implementation of the Shared Key string to sign (version
    // 2015-02-21 and later: an empty Content-Length for zero) with the storage emulator's key.
    const std::string key =
        "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==";
    EXPECT_EQ(lake::control::azure_shared_key_signature(
                  "devstoreaccount1", key, "GET", "/photos",
                  {{"restype", "container"}, {"comp", "list"}, {"prefix", "docs/"}, {"delimiter", "/"},
                   {"maxresults", "1000"}},
                  {{"x-ms-date", "Sun, 11 Oct 2026 12:00:00 GMT"}, {"x-ms-version", "2021-08-06"},
                   {"content-length", "0"}}),
              "5i/gkkjyN9yGIxt9mzcPXUpeWL0FWnabgSRYaklTezk=");
    EXPECT_EQ(lake::control::azure_shared_key_signature(
                  "devstoreaccount1", key, "PUT", "/photos/docs/a%20b.txt", {},
                  {{"x-ms-date", "Sun, 11 Oct 2026 12:00:00 GMT"}, {"x-ms-version", "2021-08-06"},
                   {"content-length", "5"}, {"content-type", "text/plain"}, {"x-ms-blob-type", "BlockBlob"}}),
              "1B9iMuKVTFQeNV0B35l4Bum8j2yuO8vA1Ut8pDL4Q9g=");
}

TEST(StoreProviders, AnS3BucketConformsPathStyleAndOnItsOwnHost) {
    FakeS3 r2;
    auto path_style = lake::control::s3_store_contents(
        r2.client("https://account.r2.cloudflarestorage.com"), r2.http(), fixed_clock());
    conforms(*path_style, "path-style (R2, B2, GCS)");
    EXPECT_TRUE(r2.signed_failures.empty()) << r2.signed_failures.front();
    EXPECT_TRUE(r2.objects.contains("docs/")) << "an empty folder keeps a marker object";

    FakeS3 s3;
    s3.virtual_host = true;
    s3.region = "eu-west-1";
    auto hosted = lake::control::s3_store_contents(
        s3.client("https://s3.eu-west-1.amazonaws.com", "team/shared"), s3.http(), fixed_clock());
    conforms(*hosted, "virtual host (AWS S3), under a prefix");
    EXPECT_TRUE(s3.signed_failures.empty()) << s3.signed_failures.front();
    for (const auto& [key, object] : s3.objects) {
        EXPECT_TRUE(key.starts_with("team/shared/")) << key << " is outside the store's prefix";
    }
}

TEST(StoreProviders, AnS3ListingFollowsContinuationAndSkipsNamesADriveCannotShow) {
    FakeS3 bucket;
    for (int index = 0; index < 1205; ++index) {
        bucket.objects[std::format("big/{:05}.csv", index)] = {"x", ""};
    }
    bucket.objects["big/a\\b"] = {"x", ""};
    auto contents = lake::control::s3_store_contents(
        bucket.client("https://s3.us-west-002.backblazeb2.com"), bucket.http(), fixed_clock());
    const auto listed = done(run(contents->list("big")), "list a big folder");
    EXPECT_EQ(listed.size(), 1205U) << "two pages, and a backslash is not a drive name";
    EXPECT_GE(bucket.requests, 2);
}

TEST(StoreProviders, AnS3BucketThatRefusesTheCredentialSaysSo) {
    FakeS3 bucket;
    auto client = bucket.client("https://storage.googleapis.com");
    client.secret_access_key = "wrong";
    auto contents = lake::control::s3_store_contents(client, bucket.http(), fixed_clock());
    const auto refused = run(contents->list(""));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, "store_unavailable");
    EXPECT_NE(refused.error().message.find("refused the workspace's credential"), std::string::npos)
        << refused.error().message;
    EXPECT_NE(refused.error().message.find("SignatureDoesNotMatch"), std::string::npos);
}

TEST(StoreProviders, AnAzureContainerConformsWithASharedKeyAndWithASas) {
    FakeAzure keyed;
    auto shared = lake::control::azure_store_contents(
        {keyed.endpoint(), "", keyed.container, "", keyed.account_key, ""}, keyed.http(), fixed_clock());
    conforms(*shared, "Shared Key");

    FakeAzure signed_url;
    signed_url.sas = "c2lnbmF0dXJl";
    auto sas = lake::control::azure_store_contents(
        {signed_url.endpoint(), "", signed_url.container, "uploads",
         "", "?sv=2021-08-06&sp=racwdl&sig=" + signed_url.sas},
        signed_url.http(), fixed_clock());
    conforms(*sas, "a SAS, under a prefix");
    for (const auto& [key, object] : signed_url.objects) {
        EXPECT_TRUE(key.starts_with("uploads/")) << key;
    }
}

TEST(StoreProviders, AStoreKeptInTheWorkspaceZettaConformsAndSealsItsBytes) {
    auto tables = std::make_shared<lake::test::RecordTables>();
    const std::string workspace = "6f1c7a52-6a07-4b52-9a39-0c6e5a1f4d10";
    auto records = lake::control::WorkspaceRecords::open(
        [tables](std::string capability, bool, json input)
            -> liquid::runtime::task<lake::control::ZettaControlResult<json>> {
            co_return tables->run(capability, input);
        },
        workspace, kek);
    ASSERT_TRUE(records) << records.error().message;
    auto objects = std::make_shared<lake::test::FakeZettaObjects>();
    objects->add_node("http://workspace.test", "token");
    auto cipher = lake::control::DriveFileCipher::open(kek, workspace);
    ASSERT_TRUE(cipher);
    const std::string bucket = lake::control::kept_store_bucket("st-0123456789ab");
    EXPECT_EQ(bucket, "lake_st_0123456789ab");
    auto contents = lake::control::zetta_store_contents(
        *records,
        lake::control::DriveFileStore(
            std::make_shared<lake::control::ZettaObjectClient>(
                objects, lake::control::ObjectEndpoint{"http://workspace.test", "token"}, bucket),
            std::move(*cipher)),
        "store-1");
    conforms(*contents, "workspace Zetta");

    done(run(contents->write("secret/plan.txt", "the plaintext", "text/plain")), "write");
    const auto stored = objects->objects("http://workspace.test", bucket);
    ASSERT_EQ(stored.size(), 1U);
    for (const auto& [key, bytes] : stored) {
        EXPECT_TRUE(key.starts_with("f/")) << "an opaque key: " << key;
        EXPECT_EQ(bytes.find("the plaintext"), std::string::npos) << "sealed";
    }
    for (const auto& [name, collection] : tables->collections) {
        for (const auto& [key, record] : collection.records) {
            EXPECT_EQ(record.dump().find("plan.txt"), std::string::npos) << "a sealed record";
        }
    }
    done(run(contents->move("secret", "public")), "move");
    EXPECT_EQ(objects->objects("http://workspace.test", bucket), stored) << "a move never copies bytes";
}

TEST(StoreProviders, AStoreKeptInALiquidRepositoryConformsAsChanges) {
    FakeBranch branch;
    auto contents = lake::api::repository_store_contents(branch.branch());
    conforms(*contents, "liquid repository");
    EXPECT_GT(branch.changes, 5) << "each write, folder, move and removal is a change";
    EXPECT_TRUE(names(*contents, "").size() == 1U) << "the folder marker is never shown";
}

TEST(StoreProviders, TheCloudBrowserOpensEachProviderWithTheWorkspaceSecret) {
    FakeS3 r2;
    const auto browser = lake::control::cloud_store_browser(r2.http(), fixed_clock());
    lake::control::StoreOpening opening;
    opening.store.type = "object";
    opening.store.provider = "r2";
    opening.store.bucket = "media";
    opening.store.endpoint_url = "https://account.r2.cloudflarestorage.com";
    opening.store.credential_name = "r2-keys";
    opening.credential = {{"access_key_id", r2.access_key_id}};
    const auto missing = browser(opening);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, "invalid_input");
    EXPECT_NE(missing.error().message.find("secret_access_key"), std::string::npos);
    opening.credential["secret_access_key"] = r2.secret_access_key;
    const auto opened = browser(opening);
    ASSERT_TRUE(opened) << opened.error().message;
    done(run((*opened)->write("a.txt", "a", "text/plain")), "write through the browser");
    EXPECT_TRUE(r2.objects.contains("a.txt"));
    EXPECT_TRUE(r2.signed_failures.empty());

    opening.store.provider = "azure";
    opening.credential = {{"access_key_id", "x"}};
    EXPECT_EQ(browser(opening).error().code, "invalid_input") << "Azure needs a key or a SAS";
    opening.store.provider = "s3";
    opening.store.endpoint_url = "http://insecure.test";
    opening.credential = {{"access_key_id", "x"}, {"secret_access_key", "y"}};
    EXPECT_EQ(browser(opening).error().code, "invalid_input") << "https only";
    opening.store.type = "volume";
    EXPECT_EQ(browser(opening).error().code, "not_supported");

    EXPECT_EQ(lake::control::default_store_endpoint("s3", "eu-west-1"), "https://s3.eu-west-1.amazonaws.com");
    EXPECT_EQ(lake::control::default_store_endpoint("gcs", ""), "https://storage.googleapis.com");
    EXPECT_EQ(lake::control::default_store_endpoint("b2", "us-west-004"), "https://s3.us-west-004.backblazeb2.com");
    EXPECT_EQ(lake::control::default_store_endpoint("r2", ""), "");
    EXPECT_EQ(lake::control::store_endpoint_problem("https://x.r2.cloudflarestorage.com"), "");
    EXPECT_NE(lake::control::store_endpoint_problem("https://x.test/path"), "");
    EXPECT_NE(lake::control::store_endpoint_problem("https://user@x.test"), "");
    EXPECT_EQ(lake::control::store_bucket_problem("s3", "my.bucket-1"), "");
    EXPECT_NE(lake::control::store_bucket_problem("azure", "my.container"), "");
    EXPECT_NE(lake::control::store_bucket_problem("s3", "UPPER"), "");
    EXPECT_NE(lake::control::store_prefix_problem("/a"), "");
    EXPECT_EQ(lake::control::store_prefix_problem("a/b"), "");
}
