#pragma once

namespace lake::test {

// Returns null when PostgreSQL tests are disabled. Otherwise lazily creates the
// current test's database; repeated calls in that test return the same connection.
const char* postgres_uri();

} // namespace lake::test
