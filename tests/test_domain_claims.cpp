// Custom-domain claims (lake#949, migration 0106_domain_claims,
// docs/api/platform.md#custom-domains). The store half runs over the in-memory store and, with
// LAKE_TEST_DB_URI, over PostgreSQL: one claim per host across accounts, the same environment
// asking again keeps its token, only the claim's own token verifies it, and releasing frees the
// host. The DNS half checks the TXT question lake asks and how it reads the answer, without a
// network. The capability flow (create_domain, verify_domain, delete_domain, nothing written for
// the operator until verified) is in tests/test_platform.cpp.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lake/control/dns_txt.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"
#include "lake/control/store.hpp"
#include "postgres_fixture.hpp"
#include "workspace_fixture.hpp"

namespace {

constexpr std::string_view kek = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string token(char digit) {
    return std::string(64, digit);
}

class DomainClaims : public testing::TestWithParam<bool> {
protected:
    struct Owner {
        std::string customer_id, workspace_id, repository_id;
    };

    void SetUp() override {
        if (GetParam()) {
            const char* uri = lake::test::postgres_uri();
            if (uri == nullptr || *uri == '\0') {
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            }
            uri_ = uri;
            store_ = lake::control::make_postgres_store(uri_, std::string(kek));
        } else {
            store_ = std::make_shared<lake::control::InMemoryStore>();
        }
        workspaces_ = std::make_unique<lake::test::RecordWorkspaces>(store_, uri_);
        first_ = owner("first");
        second_ = owner("second");
    }

    Owner owner(const std::string& name) {
        const std::string suffix = lake::control::new_uuid_v4().substr(0, 8);
        const auto user = store_->create_user({"taap:1:ed25519:domain-" + name + "-" + suffix,
                                               name + "-" + suffix + "@example.test",
                                               name});
        EXPECT_TRUE(user) << user.error().message;
        const auto customer =
            store_->create_customer({"individual", user->id, "", "dc" + name + suffix});
        EXPECT_TRUE(customer) << customer.error().message;
        Owner out;
        out.customer_id = customer->id;
        out.workspace_id = workspaces_->create(out.customer_id, "home");
        const auto repository = store_->create_repository(
            {out.customer_id, "site-" + suffix, "", "private", out.workspace_id});
        EXPECT_TRUE(repository) << repository.error().message;
        out.repository_id = repository->id;
        return out;
    }

    lake::control::NewDomainClaim claim(const Owner& who,
                                        const std::string& host,
                                        const std::string& environment,
                                        char digit) const {
        return {.host = host,
                .customer_id = who.customer_id,
                .workspace_id = who.workspace_id,
                .repository_id = who.repository_id,
                .environment = environment,
                .tls = "acme",
                .token = token(digit)};
    }

    std::string uri_;
    std::shared_ptr<lake::control::Store> store_;
    std::unique_ptr<lake::test::RecordWorkspaces> workspaces_;
    Owner first_, second_;
};

INSTANTIATE_TEST_SUITE_P(Stores,
                         DomainClaims,
                         testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Postgres" : "Memory";
                         });

TEST_P(DomainClaims, OneEnvironmentHoldsAHostAcrossAccounts) {
    const auto made = store_->claim_domain(claim(first_, "shop.example.test", "production", 'a'));
    ASSERT_TRUE(made) << made.error().message;
    EXPECT_EQ(made->state, "pending");
    EXPECT_EQ(made->token, token('a'));
    EXPECT_TRUE(made->verified_at.empty());
    EXPECT_FALSE(made->created_at.empty());

    // Another account: conflict, and the first claim is unchanged.
    const auto taken = store_->claim_domain(claim(second_, "shop.example.test", "production", 'b'));
    ASSERT_FALSE(taken);
    EXPECT_EQ(taken.error().code, "conflict");
    // Another environment of the same repository: conflict too.
    const auto staging = store_->claim_domain(claim(first_, "shop.example.test", "staging", 'c'));
    ASSERT_FALSE(staging);
    EXPECT_EQ(staging.error().code, "conflict");
    const auto held = store_->get_domain_claim("shop.example.test");
    ASSERT_TRUE(held);
    EXPECT_EQ(held->repository_id, first_.repository_id);
    EXPECT_EQ(held->customer_id, first_.customer_id);
    EXPECT_EQ(held->token, token('a'));

    // The same environment asking again keeps its token and takes the new tls.
    auto again = claim(first_, "shop.example.test", "production", 'd');
    again.tls = "provided";
    const auto repeated = store_->claim_domain(again);
    ASSERT_TRUE(repeated) << repeated.error().message;
    EXPECT_EQ(repeated->token, token('a'));
    EXPECT_EQ(repeated->tls, "provided");
}

TEST_P(DomainClaims, OnlyTheClaimsOwnTokenVerifiesIt) {
    ASSERT_TRUE(store_->claim_domain(claim(first_, "www.example.test", "production", 'a')));
    const auto wrong = store_->verify_domain_claim(
        "www.example.test", first_.repository_id, "production", token('b'));
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, "not_found");
    const auto other = store_->verify_domain_claim(
        "www.example.test", second_.repository_id, "production", token('a'));
    ASSERT_FALSE(other);
    EXPECT_EQ(other.error().code, "not_found");
    EXPECT_EQ(store_->get_domain_claim("www.example.test")->state, "pending");

    const auto verified = store_->verify_domain_claim(
        "www.example.test", first_.repository_id, "production", token('a'));
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified->state, "verified");
    EXPECT_FALSE(verified->verified_at.empty());
    // Verifying again changes nothing.
    const auto twice = store_->verify_domain_claim(
        "www.example.test", first_.repository_id, "production", token('a'));
    ASSERT_TRUE(twice);
    EXPECT_EQ(twice->verified_at, verified->verified_at);
}

TEST_P(DomainClaims, ReleasingFreesTheHostAndOnlyItsOwnerCanRelease) {
    ASSERT_TRUE(store_->claim_domain(claim(first_, "app.example.test", "production", 'a')));
    // Another account's release does nothing.
    ASSERT_TRUE(
        store_->release_domain_claim("app.example.test", second_.repository_id, "production"));
    ASSERT_TRUE(store_->get_domain_claim("app.example.test"));
    ASSERT_TRUE(
        store_->release_domain_claim("app.example.test", first_.repository_id, "production"));
    const auto gone = store_->get_domain_claim("app.example.test");
    ASSERT_FALSE(gone);
    EXPECT_EQ(gone.error().code, "not_found");
    const auto claimed =
        store_->claim_domain(claim(second_, "app.example.test", "production", 'b'));
    ASSERT_TRUE(claimed) << claimed.error().message;
    EXPECT_EQ(claimed->repository_id, second_.repository_id);
    EXPECT_EQ(claimed->state, "pending");
}

// The TXT question for _lake-challenge.www.example.test with id 0x1234.
TEST(DnsTxt, EncodesOneTxtQuestionWithRecursion) {
    const auto query = lake::control::encode_txt_query("_lake-challenge.www.example.test", 0x1234);
    ASSERT_TRUE(query) << query.error();
    const std::vector<std::uint8_t> header{
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    ASSERT_GT(query->size(), header.size());
    EXPECT_TRUE(std::equal(header.begin(), header.end(), query->begin()));
    const std::vector<std::uint8_t> tail{0x00, 0x00, 0x10, 0x00, 0x01};
    EXPECT_TRUE(std::equal(tail.rbegin(), tail.rend(), query->rbegin()));
    EXPECT_EQ((*query)[12], 15); // "_lake-challenge"
    EXPECT_FALSE(lake::control::encode_txt_query("a..b", 1));
    EXPECT_FALSE(lake::control::encode_txt_query(std::string(64, 'a') + ".test", 1));
}

// An answer to that question: a CNAME on the way, then two TXT records, one in two pieces.
TEST(DnsTxt, ReadsEveryTxtRecordAndJoinsItsPieces) {
    auto message = *lake::control::encode_txt_query("_lake-challenge.www.example.test", 0x1234);
    message[2] = 0x81; // an answer, recursion desired
    message[3] = 0x80; // recursion available, rcode 0
    message[7] = 3;    // three answers
    const auto add = [&](std::uint16_t type, const std::vector<std::uint8_t>& data) {
        message.insert(message.end(), {0xC0, 0x0C}); // the question's name
        message.insert(message.end(),
                       {static_cast<std::uint8_t>(type >> 8),
                        static_cast<std::uint8_t>(type & 0xFF),
                        0x00,
                        0x01,
                        0x00,
                        0x00,
                        0x01,
                        0x2C,
                        static_cast<std::uint8_t>(data.size() >> 8),
                        static_cast<std::uint8_t>(data.size() & 0xFF)});
        message.insert(message.end(), data.begin(), data.end());
    };
    add(5, {0x03, 'w', 'w', 'w', 0x00});
    add(16, {0x03, 'a', 'b', 'c', 0x02, 'd', 'e'});
    add(16, {0x05, 't', 'o', 'k', 'e', 'n'});
    const auto answer = lake::control::decode_txt_answer(message, 0x1234);
    ASSERT_TRUE(answer) << answer.error();
    EXPECT_FALSE(answer->truncated);
    EXPECT_EQ(answer->records, (std::vector<std::string>{"abcde", "token"}));

    // Another question's answer is refused; a truncated one says so; NXDOMAIN holds nothing.
    EXPECT_FALSE(lake::control::decode_txt_answer(message, 0x4321));
    auto truncated = message;
    truncated[2] |= 0x02;
    const auto cut = lake::control::decode_txt_answer(truncated, 0x1234);
    ASSERT_TRUE(cut);
    EXPECT_TRUE(cut->truncated);
    auto missing = message;
    missing[3] = 0x83;
    const auto none = lake::control::decode_txt_answer(missing, 0x1234);
    ASSERT_TRUE(none);
    EXPECT_TRUE(none->records.empty());
    auto refused = message;
    refused[3] = 0x85;
    EXPECT_FALSE(lake::control::decode_txt_answer(refused, 0x1234));
    // A record that runs past the message is malformed.
    auto short_message = message;
    short_message.resize(short_message.size() - 2);
    EXPECT_FALSE(lake::control::decode_txt_answer(short_message, 0x1234));
}

TEST(DnsTxt, ReadsTheNameServersOfResolvConf) {
    const auto servers = lake::control::resolv_conf_nameservers(
        "# cluster DNS\nsearch lake.svc.cluster.local\nnameserver 10.96.0.10\n"
        "nameserver fe80::1%eth0\nnameserver 2001:db8::53\noptions ndots:5\n");
    EXPECT_EQ(servers, (std::vector<std::string>{"10.96.0.10", "2001:db8::53"}));
}

} // namespace
