#include "identity_proof_test_support.hpp"
#include "postgres_fixture.hpp"

#include <barrier>
#include <future>
#include <latch>
#include <pqxx/pqxx>
#include <thread>

#include <gtest/gtest.h>

#include "lake/api/authentication_work.hpp"
#include "lake/control/authentication_limits.hpp"
#include "lake/control/identity_crypto.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/postgres_store.hpp"

namespace {
constexpr auto kek = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

TEST(AuthenticationLimits, ExpiryAndCapacityNeverEvictAnActiveBudget) {
    using Attempts = lake::control::AuthenticationAttempts;
    using Outcome = lake::control::AuthenticationOutcome;
    Attempts attempts(2);
    const auto now = Attempts::Clock::now();
    const auto first = attempts.reserve("a", 2, "a-before", now);
    ASSERT_TRUE(first);
    EXPECT_TRUE(attempts.reserve("a", 2, "unused", now));
    EXPECT_FALSE(attempts.reserve("a", 2, "unused", now));
    attempts.finish("a", *first, Outcome::failure, true, now);
    EXPECT_FALSE(attempts.reserve("a", 2, "unused", now));
    EXPECT_TRUE(attempts.reserve("b", 2, "b-before", now));
    EXPECT_FALSE(attempts.reserve("c", 2, "c-before", now));
    const auto next_window = now + std::chrono::minutes(15);
    EXPECT_TRUE(attempts.reserve("c", 2, "c-after", next_window));
    EXPECT_TRUE(attempts.reserve("a", 2, "a-after", next_window));
    attempts.finish("a", *first, Outcome::success, true, next_window);
    EXPECT_TRUE(attempts.reserve("a", 2, "unused", next_window));
    EXPECT_FALSE(attempts.reserve("a", 2, "unused", next_window))
        << "an old completion cannot clear a new window";
}

class AuthenticationStores : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (GetParam()) {
            const auto uri = lake::test::postgres_uri();
            if (!uri)
                GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
            store = lake::control::make_postgres_store(uri, kek);
        } else {
            store = std::make_unique<lake::control::InMemoryStore>();
        }
        auto identity = lake::test::test_identity("authentication-limits");
        lake::control::NewPasswordUser input{
            "lake", email, password, "Limits", "test", identity.document};
        if (GetParam())
            input.identity_proof = lake::test::prove_identity(*store, "lake", identity);
        auto signed_up = store->sign_up_password(input);
        ASSERT_TRUE(signed_up) << signed_up.error().message;
        user_id = signed_up->session.user_id;
        session_id = signed_up->session.id;
        auto enrolled = store->enroll_mfa(user_id, "lake", {session_id, password});
        ASSERT_TRUE(enrolled) << enrolled.error().message;
        codes = enrolled->recovery_codes;
    }
    std::unique_ptr<lake::control::Store> store;
    std::string email = "limits@example.test", password = "a long test password";
    std::string user_id, session_id;
    std::vector<std::string> codes;
};

TEST_P(AuthenticationStores, SelectorsAndOneUseApplyAcrossEveryRecoveryPath) {
    ASSERT_EQ(codes.size(), 8);
    for (const auto& code : codes) {
        EXPECT_EQ(code.size(), 36);
        EXPECT_TRUE(code.starts_with("rc1_"));
        EXPECT_FALSE(lake::control::identity_crypto::recovery_selector(code).empty());
    }
    EXPECT_TRUE(lake::control::identity_crypto::recovery_selector("0123456789ab").empty());
    EXPECT_TRUE(
        lake::control::identity_crypto::recovery_selector("RC1_" + codes[0].substr(4)).empty());
    ASSERT_TRUE(store->verify_mfa_code(user_id, "lake", codes[0]));
    EXPECT_FALSE(store->recover_with_code("lake", email, codes[0], "test"));
    ASSERT_TRUE(store->recover_with_code("lake", email, codes[3], "test"));
    EXPECT_FALSE(store->recover_with_code("lake", email, codes[3], "test"));
    ASSERT_TRUE(store->sign_in_totp({"lake", email, password, codes[1], "test"}));
    EXPECT_FALSE(store->verify_mfa_code(user_id, "lake", codes[1]));
    ASSERT_TRUE(store->disable_mfa(user_id, "lake", codes[2]));
    EXPECT_TRUE(store->sign_in_password({"lake", email, password, "", "test"}));
}

TEST_P(AuthenticationStores, AccountBudgetSpansCaseVariantsAndAllEntryPoints) {
    for (int i = 0; i < 10; ++i) {
        if (i % 3 == 0)
            EXPECT_FALSE(
                store->sign_in_password({"lake", "LIMITS@EXAMPLE.TEST", "wrong", "", "test"}));
        else if (i % 3 == 1)
            EXPECT_FALSE(store->sign_in_totp({"lake", email, "wrong", "000000", "test"}));
        else
            EXPECT_FALSE(store->recover_with_code("lake", email, "wrong", "test"));
    }
    const auto password_result = store->sign_in_password({"lake", email, password, "", "test"});
    ASSERT_FALSE(password_result);
    EXPECT_EQ(password_result.error().code, "unauthenticated");
    const auto recovery = store->recover_with_code("lake", email, codes[0], "test");
    ASSERT_FALSE(recovery);
    EXPECT_EQ(recovery.error().code, "unauthenticated");
    const auto totp = store->sign_in_totp({"lake", email, password, codes[0], "test"});
    ASSERT_FALSE(totp);
    EXPECT_EQ(totp.error().code, "unauthenticated");
    const auto fresh = store->verify_mfa_code(user_id, "lake", codes[0]);
    ASSERT_FALSE(fresh);
    EXPECT_EQ(fresh.error().code, "unauthenticated");
    const auto disabled = store->disable_mfa(user_id, "lake", codes[0]);
    ASSERT_FALSE(disabled);
    EXPECT_EQ(disabled.error().code, "unauthenticated");
}

// An unknown email answers with the same 401 as a wrong password and, so that the response time
// does not tell them apart either, runs one Argon2id check against the dummy hash (lake#1101).
TEST_P(AuthenticationStores, UnknownAccountsRunTheSameArgon2idCheckAsAWrongPassword) {
    namespace crypto = lake::control::identity_crypto;
    const std::string unknown = "nobody-1101@example.test";

    auto before = crypto::dummy_secret_verifications();
    const auto known = store->sign_in_password({"lake", email, "wrong", "", "test"});
    ASSERT_FALSE(known);
    EXPECT_EQ(crypto::dummy_secret_verifications(), before)
        << "a known account checks its own hash, not the dummy";

    before = crypto::dummy_secret_verifications();
    const auto missing = store->sign_in_password({"lake", unknown, "wrong", "", "test"});
    ASSERT_FALSE(missing);
    EXPECT_EQ(crypto::dummy_secret_verifications(), before + 1);
    EXPECT_EQ(missing.error().code, known.error().code);
    EXPECT_EQ(missing.error().message, known.error().message);
    EXPECT_EQ(missing.error().code, "unauthenticated");

    before = crypto::dummy_secret_verifications();
    const auto known_totp = store->sign_in_totp({"lake", email, "wrong", "000000", "test"});
    const auto missing_totp = store->sign_in_totp({"lake", unknown, "wrong", "000000", "test"});
    ASSERT_FALSE(known_totp);
    ASSERT_FALSE(missing_totp);
    EXPECT_EQ(crypto::dummy_secret_verifications(), before + 1);
    EXPECT_EQ(missing_totp.error().code, known_totp.error().code);
    EXPECT_EQ(missing_totp.error().message, known_totp.error().message);

    // Recovery runs one check on every refusal: an unknown email, and a code matching no
    // selector on a known one.
    before = crypto::dummy_secret_verifications();
    const auto known_recovery = store->recover_with_code("lake", email, "wrong", "test");
    const auto missing_recovery = store->recover_with_code("lake", unknown, codes[0], "test");
    ASSERT_FALSE(known_recovery);
    ASSERT_FALSE(missing_recovery);
    EXPECT_EQ(crypto::dummy_secret_verifications(), before + 2);
    EXPECT_EQ(missing_recovery.error().code, known_recovery.error().code);
    EXPECT_EQ(missing_recovery.error().message, known_recovery.error().message);

    // The real account still signs in: the dummy never matches and the budget is per subject.
    EXPECT_FALSE(crypto::verify_secret_against_dummy(password));
    EXPECT_TRUE(store->sign_in_totp({"lake", email, password, codes[1], "test"}));
}

TEST_P(AuthenticationStores, ConcurrentRecoveryConsumesExactlyOnce) {
    std::barrier start(3);
    auto recover = [&] {
        start.arrive_and_wait();
        return store->recover_with_code("lake", email, codes[0], "test");
    };
    auto first = std::async(std::launch::async, recover);
    auto second = std::async(std::launch::async, recover);
    start.arrive_and_wait();
    const bool first_ok = first.get().has_value();
    const bool second_ok = second.get().has_value();
    EXPECT_NE(first_ok, second_ok);
}

TEST(AuthenticationLimits, LegacyRecoveryCredentialsAreNeverScanned) {
    const auto uri = lake::test::postgres_uri();
    if (!uri)
        GTEST_SKIP() << "LAKE_TEST_DB_URI is not configured";
    auto store = lake::control::make_postgres_store(uri, kek);
    auto identity = lake::test::test_identity("legacy-recovery-refusal");
    lake::control::NewPasswordUser input{
        "lake", "legacy@example.test", "long test password", "Legacy", "test", identity.document};
    input.identity_proof = lake::test::prove_identity(*store, "lake", identity);
    auto signed_up = store->sign_up_password(input);
    ASSERT_TRUE(signed_up);
    auto enrolled = store->enroll_mfa(
        signed_up->session.user_id, "lake", {signed_up->session.id, input.password});
    ASSERT_TRUE(enrolled);
    const std::string legacy = "0123456789ab";
    {
        pqxx::connection connection(uri);
        pqxx::work tx(connection);
        tx.exec_params("INSERT INTO credentials (user_id, pool_id, kind, subject, secret_hash) "
                       "VALUES ($1::uuid, 'lake', 'recovery', 'legacy-random-subject', $2)",
                       signed_up->session.user_id,
                       lake::control::identity_crypto::hash_secret(legacy));
        tx.commit();
    }
    EXPECT_FALSE(store->recover_with_code("lake", input.email, legacy, "test"));
    EXPECT_FALSE(store->sign_in_totp({"lake", input.email, input.password, legacy, "test"}));
    EXPECT_FALSE(store->verify_mfa_code(signed_up->session.user_id, "lake", legacy));
    EXPECT_FALSE(store->disable_mfa(signed_up->session.user_id, "lake", legacy));
    EXPECT_TRUE(store->recover_with_code("lake", input.email, enrolled->recovery_codes[0], "test"));
}

TEST(AuthenticationLimits, WorkerAdmissionIsBoundedAndReleasesAfterCompletion) {
    std::promise<void> release;
    auto released = release.get_future().share();
    std::latch entered(2);
    auto run = [&] {
        auto operation = [&]() -> liquid::runtime::task<lake::control::Result<void>> {
            co_return co_await lake::api::authentication_work([&]() -> lake::control::Result<void> {
                entered.count_down();
                released.wait();
                return {};
            });
        };
        return liquid::runtime::block_on(operation());
    };
    auto first = std::async(std::launch::async, run);
    auto second = std::async(std::launch::async, run);
    entered.wait();
    auto overflow = []() -> liquid::runtime::task<lake::control::Result<void>> {
        co_return co_await lake::api::authentication_work([]() -> lake::control::Result<void> {
            ADD_FAILURE() << "overflow must never execute";
            return {};
        });
    };
    auto refused = liquid::runtime::block_on(overflow());
    EXPECT_TRUE(refused);
    if (refused) {
        EXPECT_FALSE(*refused);
        if (!*refused)
            EXPECT_EQ(refused->error().code, "rate_limited");
    }
    release.set_value();
    const auto first_result = first.get();
    const auto second_result = second.get();
    EXPECT_TRUE(first_result && *first_result);
    EXPECT_TRUE(second_result && *second_result);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (lake::api::authentication_work_count.load() != 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(lake::api::authentication_work_count.load(), 0);
}

INSTANTIATE_TEST_SUITE_P(Stores,
                         AuthenticationStores,
                         testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) {
                             return info.param ? "Postgres" : "Memory";
                         });
} // namespace
