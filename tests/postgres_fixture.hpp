#pragma once

#include <string>

namespace lake::test {

// Returns null when PostgreSQL tests are disabled. Otherwise lazily creates the
// current test's database; repeated calls in that test return the same connection.
const char* postgres_uri();

// The start-up sweep's drop of one stale `lake_fixture_*` database, on a connection of its own:
// true when it is gone, false (reported on stderr, never thrown) when it is in use, already being
// dropped or the connection is lost (lake#1254). `invalid` is PostgreSQL's datconnlimit -2, a
// database whose DROP started and did not finish.
bool drop_stale_fixture_database(const std::string& admin_uri,
                                 const std::string& name,
                                 bool is_template,
                                 bool invalid);

} // namespace lake::test
