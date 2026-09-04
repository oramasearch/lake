// The one translation unit that names libpqxx. Each call opens (or reuses) one connection and runs
// one transaction, inline on the caller's thread; SQL errors map to the store's error codes.
#include "lake/control/postgres_store.hpp"

#include <memory>
#include <mutex>
#include <optional>

#include <pqxx/pqxx>

namespace lake::control {
namespace {

std::optional<std::string> nullable(const std::string& s) {
    if (s.empty()) return std::nullopt;
    return s;
}

constexpr const char* user_columns =
    "id::text, coalesce(taap_id,''), coalesce(email,''), display_name, created_at::text, updated_at::text";
constexpr const char* organization_columns =
    "id::text, coalesce(taap_id,''), display_name, created_at::text, updated_at::text";
constexpr const char* customer_columns =
    "id::text, customer_type, coalesce(user_id::text,''), coalesce(organization_id::text,''), name, "
    "created_at::text, updated_at::text";

constexpr const char* repository_columns =
    "id::text, customer_id::text, name, description, visibility, created_at::text, updated_at::text";
constexpr const char* identity_columns =
    "id::text, taap_id, public_key, wrapped_seed, created_at::text, coalesce(rotation_document::text,''), current";
constexpr const char* audit_event_columns =
    "id::text, to_char(occurred_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
    "coalesce(customer_id::text,''), kind, principal, object, action, outcome, request_id, "
    "coalesce(method,''), coalesce(device,''), details::text, coalesce(previous_digest,''), signer, signature, digest";

Repository repository_of(const pqxx::row& r) {
    return Repository{r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(), r[3].as<std::string>(),
                      r[4].as<std::string>(), r[5].as<std::string>(), r[6].as<std::string>()};
}

User user_of(const pqxx::row& r) {
    return User{r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(),
                r[3].as<std::string>(), r[4].as<std::string>(), r[5].as<std::string>()};
}
Organization organization_of(const pqxx::row& r) {
    return Organization{r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(),
                        r[3].as<std::string>(), r[4].as<std::string>()};
}
Customer customer_of(const pqxx::row& r) {
    return Customer{r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(), r[3].as<std::string>(),
                    r[4].as<std::string>(), r[5].as<std::string>(), r[6].as<std::string>()};
}

audit::Identity identity_of(const pqxx::row& r) {
    return audit::Identity{
        .id = r[0].as<std::string>(),
        .taap_id = r[1].as<std::string>(),
        .public_key = r[2].as<std::string>(),
        .wrapped_seed = r[3].as<std::string>(),
        .created_at = r[4].as<std::string>(),
        .rotation_document = r[5].as<std::string>(),
        .current = r[6].as<bool>(),
    };
}

audit::Event audit_event_of(const pqxx::row& r) {
    return audit::Event{
        .id = r[0].as<std::string>(),
        .occurred_at = r[1].as<std::string>(),
        .draft = audit::EventDraft{
            .customer_id = r[2].as<std::string>(),
            .kind = r[3].as<std::string>(),
            .principal = r[4].as<std::string>(),
            .object = r[5].as<std::string>(),
            .action = r[6].as<std::string>(),
            .outcome = r[7].as<std::string>(),
            .request_id = r[8].as<std::string>(),
            .method = r[9].as<std::string>(),
            .device = r[10].as<std::string>(),
            .details = nlohmann::json::parse(r[11].as<std::string>()),
        },
        .previous_digest = r[12].as<std::string>(),
        .signer = r[13].as<std::string>(),
        .signature = r[14].as<std::string>(),
        .digest = r[15].as<std::string>(),
    };
}

class PostgresStore final : public Store {
public:
    explicit PostgresStore(std::string uri) : uri_(std::move(uri)) {}
    std::string kind() const override { return "postgres"; }

    Result<void> ping() override {
        Result<void> result = with_tx<void>([](pqxx::work& tx) -> Result<void> {
            tx.exec("SET LOCAL statement_timeout = '1000ms'");
            tx.exec("SELECT 1");
            return {};
        });
        if (!result) return std::unexpected(StoreError{"store_unavailable", result.error().message});
        return {};
    }

    Result<User> create_user(const NewUser& in) override {
        return with_tx<User>([&](pqxx::work& tx) -> Result<User> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO users (taap_id, email, display_name) VALUES ($1, $2, $3) RETURNING ") + user_columns,
                nullable(in.taap_id), nullable(in.email), in.display_name);
            return user_of(rows[0]);
        });
    }
    Result<std::vector<User>> list_users(int limit) override {
        return with_tx<std::vector<User>>([&](pqxx::work& tx) -> Result<std::vector<User>> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + user_columns + " FROM users ORDER BY created_at DESC LIMIT $1", limit);
            std::vector<User> out;
            for (const auto& r : rows) out.push_back(user_of(r));
            return out;
        });
    }
    Result<User> get_user(const std::string& id) override {
        return with_tx<User>([&](pqxx::work& tx) -> Result<User> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + user_columns + " FROM users WHERE id = $1::uuid", id);
            if (rows.empty()) return std::unexpected(StoreError{"not_found", "no user " + id});
            return user_of(rows[0]);
        });
    }

    Result<Organization> create_organization(const NewOrganization& in) override {
        return with_tx<Organization>([&](pqxx::work& tx) -> Result<Organization> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO organizations (taap_id, display_name) VALUES ($1, $2) RETURNING ") + organization_columns,
                nullable(in.taap_id), in.display_name);
            return organization_of(rows[0]);
        });
    }
    Result<std::vector<Organization>> list_organizations(int limit) override {
        return with_tx<std::vector<Organization>>([&](pqxx::work& tx) -> Result<std::vector<Organization>> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + organization_columns + " FROM organizations ORDER BY created_at DESC LIMIT $1", limit);
            std::vector<Organization> out;
            for (const auto& r : rows) out.push_back(organization_of(r));
            return out;
        });
    }
    Result<Organization> get_organization(const std::string& id) override {
        return with_tx<Organization>([&](pqxx::work& tx) -> Result<Organization> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + organization_columns + " FROM organizations WHERE id = $1::uuid", id);
            if (rows.empty()) return std::unexpected(StoreError{"not_found", "no organization " + id});
            return organization_of(rows[0]);
        });
    }

    Result<Customer> create_customer(const NewCustomer& in) override {
        return with_tx<Customer>([&](pqxx::work& tx) -> Result<Customer> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO customers (customer_type, user_id, organization_id, name) "
                            "VALUES ($1, $2::uuid, $3::uuid, $4) RETURNING ") + customer_columns,
                in.customer_type, nullable(in.user_id), nullable(in.organization_id), in.name);
            return customer_of(rows[0]);
        });
    }
    Result<std::vector<Customer>> list_customers(int limit) override {
        return with_tx<std::vector<Customer>>([&](pqxx::work& tx) -> Result<std::vector<Customer>> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + customer_columns + " FROM customers ORDER BY created_at DESC LIMIT $1", limit);
            std::vector<Customer> out;
            for (const auto& r : rows) out.push_back(customer_of(r));
            return out;
        });
    }
    Result<Customer> get_customer(const std::string& id) override {
        return with_tx<Customer>([&](pqxx::work& tx) -> Result<Customer> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + customer_columns + " FROM customers WHERE id = $1::uuid", id);
            if (rows.empty()) return std::unexpected(StoreError{"not_found", "no customer " + id});
            return customer_of(rows[0]);
        });
    }

    Result<Repository> create_repository(const NewRepository& in) override {
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO repositories (customer_id, name, description, visibility) "
                            "VALUES ($1::uuid, $2, $3, $4) RETURNING ") + repository_columns,
                in.customer_id, in.name, in.description, in.visibility.empty() ? std::string("private") : in.visibility);
            return repository_of(rows[0]);
        });
    }
    Result<std::vector<Repository>> list_repositories(const std::string& customer_id, int limit) override {
        return with_tx<std::vector<Repository>>([&](pqxx::work& tx) -> Result<std::vector<Repository>> {
            const pqxx::result rows = customer_id.empty()
                ? tx.exec_params(std::string("SELECT ") + repository_columns + " FROM repositories ORDER BY created_at DESC LIMIT $1", limit)
                : tx.exec_params(std::string("SELECT ") + repository_columns +
                                     " FROM repositories WHERE customer_id = $1::uuid ORDER BY created_at DESC LIMIT $2",
                                 customer_id, limit);
            std::vector<Repository> out;
            for (const auto& r : rows) out.push_back(repository_of(r));
            return out;
        });
    }
    Result<Repository> get_repository(const std::string& id) override {
        return with_tx<Repository>([&](pqxx::work& tx) -> Result<Repository> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + repository_columns + " FROM repositories WHERE id = $1::uuid", id);
            if (rows.empty()) return std::unexpected(StoreError{"not_found", "no repository " + id});
            return repository_of(rows[0]);
        });
    }

    audit::Result<audit::Identity> current_lake_identity() override {
        return with_audit_tx<audit::Identity>([](pqxx::work& tx) -> audit::Result<audit::Identity> {
            const pqxx::result rows = tx.exec(std::string("SELECT ") + identity_columns +
                                              " FROM lake_identity WHERE current ORDER BY created_at DESC LIMIT 1");
            if (rows.empty()) {
                return std::unexpected(audit::Error{"not_found", "no lake identity exists"});
            }
            return identity_of(rows[0]);
        });
    }

    audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) override {
        return with_audit_tx<audit::Identity>([&](pqxx::work& tx) -> audit::Result<audit::Identity> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + identity_columns + " FROM lake_identity WHERE taap_id = $1", taap_id);
            if (rows.empty()) {
                return std::unexpected(audit::Error{"not_found", "no lake identity for " + taap_id});
            }
            return identity_of(rows[0]);
        });
    }

    audit::Result<void> create_lake_identity(const audit::Identity& identity) override {
        return with_audit_tx<void>([&](pqxx::work& tx) -> audit::Result<void> {
            tx.exec_params("INSERT INTO lake_identity "
                           "(id, taap_id, public_key, wrapped_seed, created_at, rotation_document, current) "
                           "VALUES ($1::uuid, $2, $3, $4, $5::timestamptz, NULLIF($6, '')::jsonb, true)",
                           identity.id, identity.taap_id, identity.public_key, identity.wrapped_seed, identity.created_at,
                           identity.rotation_document);
            return {};
        });
    }

    audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                             const audit::Identity& replacement) override {
        return with_audit_tx<void>([&](pqxx::work& tx) -> audit::Result<void> {
            const pqxx::result updated = tx.exec_params("UPDATE lake_identity SET current = false "
                                                        "WHERE id = $1::uuid AND current", previous.id);
            if (updated.affected_rows() != 1) {
                return std::unexpected(audit::Error{"conflict", "the lake identity changed before rotation"});
            }
            tx.exec_params("INSERT INTO lake_identity "
                           "(id, taap_id, public_key, wrapped_seed, created_at, rotation_document, current) "
                           "VALUES ($1::uuid, $2, $3, $4, $5::timestamptz, $6::jsonb, true)",
                           replacement.id, replacement.taap_id, replacement.public_key, replacement.wrapped_seed,
                           replacement.created_at, replacement.rotation_document);
            return {};
        });
    }

    Result<void> append_audit_event(const audit::Event& event) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            tx.exec("SELECT pg_advisory_xact_lock(hashtext('lake_audit_events'))");
            const pqxx::result previous = tx.exec("SELECT digest FROM audit_events ORDER BY sequence DESC LIMIT 1");
            const std::string digest = previous.empty() ? "" : previous[0][0].as<std::string>();
            if (event.previous_digest != digest) {
                return std::unexpected(StoreError{"conflict", "audit event predecessor changed before append"});
            }
            tx.exec_params("INSERT INTO audit_events "
                           "(id, occurred_at, customer_id, kind, principal, object, action, outcome, request_id, method, "
                           "device, details, previous_digest, signer, signature, digest) "
                           "VALUES ($1::uuid, $2::timestamptz, NULLIF($3, '')::uuid, $4, $5, $6, $7, $8, $9::uuid, "
                           "NULLIF($10, ''), NULLIF($11, ''), $12::jsonb, NULLIF($13, ''), $14, $15, $16)",
                           event.id, event.occurred_at, event.draft.customer_id, event.draft.kind, event.draft.principal,
                           event.draft.object, event.draft.action, event.draft.outcome, event.draft.request_id,
                           event.draft.method, event.draft.device, event.draft.details.dump(), event.previous_digest,
                           event.signer, event.signature, event.digest);
            return {};
        });
    }

    Result<audit::Event> latest_audit_event() override {
        return with_tx<audit::Event>([](pqxx::work& tx) -> Result<audit::Event> {
            const pqxx::result rows =
                tx.exec(std::string("SELECT ") + audit_event_columns + " FROM audit_events ORDER BY sequence DESC LIMIT 1");
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no audit events exist"});
            }
            return audit_event_of(rows[0]);
        });
    }

    Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) override {
        return with_tx<std::vector<audit::Event>>([&](pqxx::work& tx) -> Result<std::vector<audit::Event>> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + audit_event_columns + " FROM audit_events "
                "WHERE ($1 = '' OR principal = $1) AND ($2 = '' OR object = $2) AND ($3 = '' OR outcome = $3) "
                "AND ($4 = '' OR occurred_at >= $4::timestamptz) AND ($5 = '' OR occurred_at <= $5::timestamptz) "
                "ORDER BY sequence ASC LIMIT $6",
                filter.principal, filter.object, filter.outcome, filter.from, filter.to, filter.limit);
            std::vector<audit::Event> events;
            for (const pqxx::row& row : rows) {
                events.push_back(audit_event_of(row));
            }
            return events;
        });
    }

    Result<void> set_audit_retention_days(const std::string& customer_id, int days) override {
        return with_tx<void>([&](pqxx::work& tx) -> Result<void> {
            const pqxx::result updated = tx.exec_params(
                "INSERT INTO audit_retention (customer_id, retention_days) VALUES ($1::uuid, $2) "
                "ON CONFLICT (customer_id) DO UPDATE SET retention_days = EXCLUDED.retention_days "
                "WHERE audit_retention.retention_days <= EXCLUDED.retention_days", customer_id, days);
            if (updated.affected_rows() != 1) {
                return std::unexpected(StoreError{"conflict", "audit retention cannot be reduced"});
            }
            return {};
        });
    }

private:
    template <class T, class F>
    Result<T> with_tx(F&& body) {
        std::lock_guard lock(mutex_);
        try {
            if (!connection_ || !connection_->is_open()) connection_ = std::make_unique<pqxx::connection>(uri_);
            pqxx::work tx(*connection_);
            Result<T> result = body(tx);
            if (result) tx.commit();
            return result;
        } catch (const pqxx::broken_connection& error) {
            connection_.reset();
            return std::unexpected(StoreError{"store_unavailable", error.what()});
        } catch (const pqxx::unique_violation& error) {
            return std::unexpected(StoreError{"conflict", error.what()});
        } catch (const pqxx::check_violation& error) {
            return std::unexpected(StoreError{"invalid_input", error.what()});
        } catch (const pqxx::foreign_key_violation& error) {
            return std::unexpected(StoreError{"invalid_input", error.what()});
        } catch (const pqxx::data_exception& error) {
            return std::unexpected(StoreError{"invalid_input", error.what()});
        } catch (const std::exception& error) {
            return std::unexpected(StoreError{"store_error", error.what()});
        }
    }

    template <class T, class F>
    audit::Result<T> with_audit_tx(F&& body) {
        std::lock_guard lock(mutex_);
        try {
            if (!connection_ || !connection_->is_open()) {
                connection_ = std::make_unique<pqxx::connection>(uri_);
            }
            pqxx::work tx(*connection_);
            audit::Result<T> result = body(tx);
            if (result) {
                tx.commit();
            }
            return result;
        } catch (const pqxx::unique_violation& error) {
            return std::unexpected(audit::Error{"conflict", error.what()});
        } catch (const pqxx::broken_connection& error) {
            connection_.reset();
            return std::unexpected(audit::Error{"store_unavailable", error.what()});
        } catch (const std::exception& error) {
            return std::unexpected(audit::Error{"store_error", error.what()});
        }
    }

    std::string uri_;
    std::mutex mutex_;
    std::unique_ptr<pqxx::connection> connection_;
};

}  // namespace

std::unique_ptr<Store> make_postgres_store(std::string connection_uri) {
    return std::make_unique<PostgresStore>(std::move(connection_uri));
}

}  // namespace lake::control

// Audit cryptography is intentionally compiled through this provider-bound translation unit. The
// audit API remains provider-neutral while external-library names stay confined to the designated
// server and control-store translation units.
#include "../audit/audit.cpp"
