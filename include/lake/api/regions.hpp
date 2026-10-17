#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Regions (lake#757, decision 64, docs/api/regions.md): production runs one Lake per region
// (eu.lake.live, us.lake.live), and the apex host (lake.live) is a region picker. The operator
// supplies every value as an environment variable; Lake has no region setting of its own. With none
// set (nonprod, local) Lake behaves as before and shows no picker.
namespace lake::api {

// One region of this deployment, an entry of LAKE_REGIONS.
struct Region {
    std::string name;  // the cloud region, e.g. swedencentral
    std::string label; // what a person reads, e.g. Europe
    std::string host;  // the region's Lake, e.g. eu.lake.live
    double latitude = 0;
    double longitude = 0;
};

// What the operator told this Lake about regions.
struct Regions {
    std::string name;         // LAKE_REGION: this Lake's own region; empty when unset
    std::string label;        // LAKE_REGION_LABEL, else the label of `name` in the list
    std::vector<Region> list; // LAKE_REGIONS, this Lake's own region included, in order
    std::string apex_host;    // LAKE_APEX_HOST, lowercased; empty: this Lake serves no picker

    // Whether a region is set at all. False on nonprod and locally.
    bool configured() const {
        return !name.empty() || !list.empty();
    }
    // The entry of `list` named `region_name`, or null.
    const Region* find(std::string_view region_name) const;
};

// Reads the four variables' values (empty = unset). An error names the variable it is about, and
// startup stops on it: invalid JSON in LAKE_REGIONS, an entry without a name, label or host, a
// coordinate that is not a number in range, two entries with one name, LAKE_REGION missing from
// LAKE_REGIONS, or LAKE_APEX_HOST without LAKE_REGIONS.
std::expected<Regions, std::string> regions_from_environment(std::string_view region,
                                                             std::string_view region_label,
                                                             std::string_view regions_json,
                                                             std::string_view apex_host);

struct GeoPoint {
    double latitude = 0;
    double longitude = 0;
};

// The centroid of an ISO 3166-1 alpha-2 country code, any case; nullopt for a code not in the
// table (including Cloudflare's XX for unknown and T1 for Tor).
std::optional<GeoPoint> country_centroid(std::string_view iso_alpha2);

// The great-circle distance between two points, in kilometres (haversine, mean Earth radius).
double great_circle_km(GeoPoint from, GeoPoint to);

// The region nearest the centroid of `country`. With no country, or one not in the table, the
// first region. Null only when `regions` is empty.
const Region* closest_region(std::span<const Region> regions, std::string_view country);

// The region the apex page preselects: the one the visitor chose before (`preference`, the
// preference cookie's value) when it names a region, else the closest to `country`.
const Region*
preselected_region(const Regions& regions, std::string_view preference, std::string_view country);

// The preference cookie: the region name only, never a session.
inline constexpr std::string_view region_cookie_name = "lake_region";
// How long the apex remembers a choice.
inline constexpr long region_cookie_seconds = 365L * 24 * 60 * 60;
std::string region_preference_cookie(std::string_view region_name, bool secure);

// Where continuing goes: https://<region host><target>, the apex request's path and query kept.
// A target that is not an origin-form path ("/...") becomes "/".
std::string region_continue_url(const Region& region, std::string_view target);

// server_info's `region` ({name, label} or null) and `regions` ([{name, label, host}] or []).
nlohmann::json region_json(const Regions* regions);
nlohmann::json regions_json(const Regions* regions);

} // namespace lake::api
