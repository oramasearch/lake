#pragma once
#include <string>

namespace lake::control {

// The customer handle: one global namespace, lowercase letters, digits and inner hyphens, 1–256
// chars (db/schema.sql customer_name_format). Generated as {adjective}-{noun}-{number}.
bool valid_handle(const std::string& name);
std::string generate_handle();

}  // namespace lake::control
