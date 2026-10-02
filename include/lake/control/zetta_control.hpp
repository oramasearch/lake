#pragma once

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

namespace lake::control {

inline constexpr std::string_view operator_control_database = "orama_control";
inline constexpr std::string_view deployments_table = "deployments";
inline constexpr std::string_view environment_env_table = "environment_env";
inline constexpr std::string_view domains_table = "domains";

inline constexpr std::array<std::string_view, 16> deployment_columns = {
    "operator",
    "lake",
    "repository",
    "environment",
    "kind",
    "change",
    "image",
    "deployment_id",
    "requested_by",
    "requested_at",
    "phase",
    "namespace",
    "url",
    "message",
    "updated_at",
    "ordering_token",
};
inline constexpr std::array<std::string_view, 9> environment_env_columns = {
    "lake",
    "repository",
    "environment",
    "key",
    "value",
    "deleted",
    "set_by",
    "updated_at",
    "ordering_token",
};
inline constexpr std::array<std::string_view, 11> domain_columns = {
    "lake",
    "repository",
    "environment",
    "host",
    "tls",
    "status",
    "message",
    "deleted",
    "requested_by",
    "updated_at",
    "ordering_token",
};

struct ZettaControlError {
    std::string code;
    std::string message;
    // The JSON pointer of the input field at fault, when the engine names one (zetta#1217).
    std::string path = {};
};

template <class T> using ZettaControlResult = std::expected<T, ZettaControlError>;
using ZettaRows = std::vector<nlohmann::json>;

class ZettaControl {
public:
    virtual ~ZettaControl() = default;
    virtual liquid::runtime::task<ZettaControlResult<ZettaRows>> sql(std::string statement,
                                                                     std::string database) = 0;
    virtual liquid::runtime::task<ZettaControlResult<void>> append(std::string table,
                                                                   nlohmann::json row) = 0;
};

class ZettaControlClient final : public ZettaControl {
public:
    ZettaControlClient(std::string endpoint, std::string bearer_token);
    ~ZettaControlClient() override;
    ZettaControlClient(ZettaControlClient&&) noexcept;
    ZettaControlClient& operator=(ZettaControlClient&&) noexcept;
    ZettaControlClient(const ZettaControlClient&) = delete;
    ZettaControlClient& operator=(const ZettaControlClient&) = delete;

    liquid::runtime::task<ZettaControlResult<ZettaRows>> sql(std::string statement,
                                                             std::string database) override;
    liquid::runtime::task<ZettaControlResult<void>> append(std::string table,
                                                           nlohmann::json row) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

ZettaControlResult<std::string> quote_zetta_literal(const nlohmann::json& value);
void install_zetta_control_transport();
void shutdown_zetta_control_transport();

} // namespace lake::control
