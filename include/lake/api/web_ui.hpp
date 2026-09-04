#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "lake/control/store.hpp"
#include "lake/search/index.hpp"

// Pure renderers: data in, HTML out. No HTTP type, no I/O. Pages are server-rendered,
// self-contained (one stylesheet at /style.css, no script) and unit-tested here.
namespace lake::api {

std::string web_ui_escape(std::string_view text);
std::string web_ui_stylesheet();

// The page frame: title, the navigation (Home, Customers, Repositories, API; Sign in/out), the
// page content. signed_in switches the session link.
std::string web_ui_shell(std::string_view title, std::string_view meta, std::string_view content,
                         bool signed_in = false);

std::string web_ui_hello(std::string_view node_name, std::string_view version, bool signed_in = false);
std::string web_ui_login(std::string_view error);
std::string web_ui_customers(const std::vector<control::Customer>& customers);
// owners: customer id → customer, for the owner column.
std::string web_ui_repositories(const std::vector<control::Repository>& repositories,
                                const std::map<std::string, control::Customer>& owners);
std::string web_ui_repository(const control::Repository& repository, const control::Customer* owner);
std::string web_ui_search(std::string_view query, const std::vector<search::Hit>& hits);

}  // namespace lake::api
