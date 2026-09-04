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

    std::string uri_;
    std::mutex mutex_;
    std::unique_ptr<pqxx::connection> connection_;
};

}  // namespace

std::unique_ptr<Store> make_postgres_store(std::string connection_uri) {
    return std::make_unique<PostgresStore>(std::move(connection_uri));
}

}  // namespace lake::control
