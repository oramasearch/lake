// SPDX-License-Identifier: MIT

#include "lake/api/regions.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <numbers>
#include <set>

// Regions (lake#757, docs/api/regions.md): the four environment variables, the closest region to a
// visitor's country, and the apex page's preference cookie.
namespace lake::api {

namespace {

struct Centroid {
    std::string_view code;
    double latitude;
    double longitude;
};

// The centroid of each country, by ISO 3166-1 alpha-2 code, for the apex page's closest-region
// choice. Source: Google's Dataset Publishing Language canonical "countries" table,
// https://developers.google.com/public-data/docs/canonical/countries_csv (retrieved 2026-10-17).
// Kept to current ISO 3166-1 alpha-2 codes: the table's AN (withdrawn in 2010), GZ and XK (not
// ISO 3166-1 codes) and UM (no coordinates) are left out. A code missing here falls back to the
// first region. Sorted by code for the binary search below.
constexpr std::array country_centroids = std::to_array<Centroid>({
    {"AD", 42.546245, 1.601554},     // Andorra
    {"AE", 23.424076, 53.847818},    // United Arab Emirates
    {"AF", 33.93911, 67.709953},     // Afghanistan
    {"AG", 17.060816, -61.796428},   // Antigua and Barbuda
    {"AI", 18.220554, -63.068615},   // Anguilla
    {"AL", 41.153332, 20.168331},    // Albania
    {"AM", 40.069099, 45.038189},    // Armenia
    {"AO", -11.202692, 17.873887},   // Angola
    {"AQ", -75.250973, -0.071389},   // Antarctica
    {"AR", -38.416097, -63.616672},  // Argentina
    {"AS", -14.270972, -170.132217}, // American Samoa
    {"AT", 47.516231, 14.550072},    // Austria
    {"AU", -25.274398, 133.775136},  // Australia
    {"AW", 12.52111, -69.968338},    // Aruba
    {"AZ", 40.143105, 47.576927},    // Azerbaijan
    {"BA", 43.915886, 17.679076},    // Bosnia and Herzegovina
    {"BB", 13.193887, -59.543198},   // Barbados
    {"BD", 23.684994, 90.356331},    // Bangladesh
    {"BE", 50.503887, 4.469936},     // Belgium
    {"BF", 12.238333, -1.561593},    // Burkina Faso
    {"BG", 42.733883, 25.48583},     // Bulgaria
    {"BH", 25.930414, 50.637772},    // Bahrain
    {"BI", -3.373056, 29.918886},    // Burundi
    {"BJ", 9.30769, 2.315834},       // Benin
    {"BM", 32.321384, -64.75737},    // Bermuda
    {"BN", 4.535277, 114.727669},    // Brunei
    {"BO", -16.290154, -63.588653},  // Bolivia
    {"BR", -14.235004, -51.92528},   // Brazil
    {"BS", 25.03428, -77.39628},     // Bahamas
    {"BT", 27.514162, 90.433601},    // Bhutan
    {"BV", -54.423199, 3.413194},    // Bouvet Island
    {"BW", -22.328474, 24.684866},   // Botswana
    {"BY", 53.709807, 27.953389},    // Belarus
    {"BZ", 17.189877, -88.49765},    // Belize
    {"CA", 56.130366, -106.346771},  // Canada
    {"CC", -12.164165, 96.870956},   // Cocos [Keeling] Islands
    {"CD", -4.038333, 21.758664},    // Congo [DRC]
    {"CF", 6.611111, 20.939444},     // Central African Republic
    {"CG", -0.228021, 15.827659},    // Congo [Republic]
    {"CH", 46.818188, 8.227512},     // Switzerland
    {"CI", 7.539989, -5.54708},      // Côte d'Ivoire
    {"CK", -21.236736, -159.777671}, // Cook Islands
    {"CL", -35.675147, -71.542969},  // Chile
    {"CM", 7.369722, 12.354722},     // Cameroon
    {"CN", 35.86166, 104.195397},    // China
    {"CO", 4.570868, -74.297333},    // Colombia
    {"CR", 9.748917, -83.753428},    // Costa Rica
    {"CU", 21.521757, -77.781167},   // Cuba
    {"CV", 16.002082, -24.013197},   // Cape Verde
    {"CX", -10.447525, 105.690449},  // Christmas Island
    {"CY", 35.126413, 33.429859},    // Cyprus
    {"CZ", 49.817492, 15.472962},    // Czech Republic
    {"DE", 51.165691, 10.451526},    // Germany
    {"DJ", 11.825138, 42.590275},    // Djibouti
    {"DK", 56.26392, 9.501785},      // Denmark
    {"DM", 15.414999, -61.370976},   // Dominica
    {"DO", 18.735693, -70.162651},   // Dominican Republic
    {"DZ", 28.033886, 1.659626},     // Algeria
    {"EC", -1.831239, -78.183406},   // Ecuador
    {"EE", 58.595272, 25.013607},    // Estonia
    {"EG", 26.820553, 30.802498},    // Egypt
    {"EH", 24.215527, -12.885834},   // Western Sahara
    {"ER", 15.179384, 39.782334},    // Eritrea
    {"ES", 40.463667, -3.74922},     // Spain
    {"ET", 9.145, 40.489673},        // Ethiopia
    {"FI", 61.92411, 25.748151},     // Finland
    {"FJ", -16.578193, 179.414413},  // Fiji
    {"FK", -51.796253, -59.523613},  // Falkland Islands [Islas Malvinas]
    {"FM", 7.425554, 150.550812},    // Micronesia
    {"FO", 61.892635, -6.911806},    // Faroe Islands
    {"FR", 46.227638, 2.213749},     // France
    {"GA", -0.803689, 11.609444},    // Gabon
    {"GB", 55.378051, -3.435973},    // United Kingdom
    {"GD", 12.262776, -61.604171},   // Grenada
    {"GE", 42.315407, 43.356892},    // Georgia
    {"GF", 3.933889, -53.125782},    // French Guiana
    {"GG", 49.465691, -2.585278},    // Guernsey
    {"GH", 7.946527, -1.023194},     // Ghana
    {"GI", 36.137741, -5.345374},    // Gibraltar
    {"GL", 71.706936, -42.604303},   // Greenland
    {"GM", 13.443182, -15.310139},   // Gambia
    {"GN", 9.945587, -9.696645},     // Guinea
    {"GP", 16.995971, -62.067641},   // Guadeloupe
    {"GQ", 1.650801, 10.267895},     // Equatorial Guinea
    {"GR", 39.074208, 21.824312},    // Greece
    {"GS", -54.429579, -36.587909},  // South Georgia and the South Sandwich Islands
    {"GT", 15.783471, -90.230759},   // Guatemala
    {"GU", 13.444304, 144.793731},   // Guam
    {"GW", 11.803749, -15.180413},   // Guinea-Bissau
    {"GY", 4.860416, -58.93018},     // Guyana
    {"HK", 22.396428, 114.109497},   // Hong Kong
    {"HM", -53.08181, 73.504158},    // Heard Island and McDonald Islands
    {"HN", 15.199999, -86.241905},   // Honduras
    {"HR", 45.1, 15.2},              // Croatia
    {"HT", 18.971187, -72.285215},   // Haiti
    {"HU", 47.162494, 19.503304},    // Hungary
    {"ID", -0.789275, 113.921327},   // Indonesia
    {"IE", 53.41291, -8.24389},      // Ireland
    {"IL", 31.046051, 34.851612},    // Israel
    {"IM", 54.236107, -4.548056},    // Isle of Man
    {"IN", 20.593684, 78.96288},     // India
    {"IO", -6.343194, 71.876519},    // British Indian Ocean Territory
    {"IQ", 33.223191, 43.679291},    // Iraq
    {"IR", 32.427908, 53.688046},    // Iran
    {"IS", 64.963051, -19.020835},   // Iceland
    {"IT", 41.87194, 12.56738},      // Italy
    {"JE", 49.214439, -2.13125},     // Jersey
    {"JM", 18.109581, -77.297508},   // Jamaica
    {"JO", 30.585164, 36.238414},    // Jordan
    {"JP", 36.204824, 138.252924},   // Japan
    {"KE", -0.023559, 37.906193},    // Kenya
    {"KG", 41.20438, 74.766098},     // Kyrgyzstan
    {"KH", 12.565679, 104.990963},   // Cambodia
    {"KI", -3.370417, -168.734039},  // Kiribati
    {"KM", -11.875001, 43.872219},   // Comoros
    {"KN", 17.357822, -62.782998},   // Saint Kitts and Nevis
    {"KP", 40.339852, 127.510093},   // North Korea
    {"KR", 35.907757, 127.766922},   // South Korea
    {"KW", 29.31166, 47.481766},     // Kuwait
    {"KY", 19.513469, -80.566956},   // Cayman Islands
    {"KZ", 48.019573, 66.923684},    // Kazakhstan
    {"LA", 19.85627, 102.495496},    // Laos
    {"LB", 33.854721, 35.862285},    // Lebanon
    {"LC", 13.909444, -60.978893},   // Saint Lucia
    {"LI", 47.166, 9.555373},        // Liechtenstein
    {"LK", 7.873054, 80.771797},     // Sri Lanka
    {"LR", 6.428055, -9.429499},     // Liberia
    {"LS", -29.609988, 28.233608},   // Lesotho
    {"LT", 55.169438, 23.881275},    // Lithuania
    {"LU", 49.815273, 6.129583},     // Luxembourg
    {"LV", 56.879635, 24.603189},    // Latvia
    {"LY", 26.3351, 17.228331},      // Libya
    {"MA", 31.791702, -7.09262},     // Morocco
    {"MC", 43.750298, 7.412841},     // Monaco
    {"MD", 47.411631, 28.369885},    // Moldova
    {"ME", 42.708678, 19.37439},     // Montenegro
    {"MG", -18.766947, 46.869107},   // Madagascar
    {"MH", 7.131474, 171.184478},    // Marshall Islands
    {"MK", 41.608635, 21.745275},    // Macedonia [FYROM]
    {"ML", 17.570692, -3.996166},    // Mali
    {"MM", 21.913965, 95.956223},    // Myanmar [Burma]
    {"MN", 46.862496, 103.846656},   // Mongolia
    {"MO", 22.198745, 113.543873},   // Macau
    {"MP", 17.33083, 145.38469},     // Northern Mariana Islands
    {"MQ", 14.641528, -61.024174},   // Martinique
    {"MR", 21.00789, -10.940835},    // Mauritania
    {"MS", 16.742498, -62.187366},   // Montserrat
    {"MT", 35.937496, 14.375416},    // Malta
    {"MU", -20.348404, 57.552152},   // Mauritius
    {"MV", 3.202778, 73.22068},      // Maldives
    {"MW", -13.254308, 34.301525},   // Malawi
    {"MX", 23.634501, -102.552784},  // Mexico
    {"MY", 4.210484, 101.975766},    // Malaysia
    {"MZ", -18.665695, 35.529562},   // Mozambique
    {"NA", -22.95764, 18.49041},     // Namibia
    {"NC", -20.904305, 165.618042},  // New Caledonia
    {"NE", 17.607789, 8.081666},     // Niger
    {"NF", -29.040835, 167.954712},  // Norfolk Island
    {"NG", 9.081999, 8.675277},      // Nigeria
    {"NI", 12.865416, -85.207229},   // Nicaragua
    {"NL", 52.132633, 5.291266},     // Netherlands
    {"NO", 60.472024, 8.468946},     // Norway
    {"NP", 28.394857, 84.124008},    // Nepal
    {"NR", -0.522778, 166.931503},   // Nauru
    {"NU", -19.054445, -169.867233}, // Niue
    {"NZ", -40.900557, 174.885971},  // New Zealand
    {"OM", 21.512583, 55.923255},    // Oman
    {"PA", 8.537981, -80.782127},    // Panama
    {"PE", -9.189967, -75.015152},   // Peru
    {"PF", -17.679742, -149.406843}, // French Polynesia
    {"PG", -6.314993, 143.95555},    // Papua New Guinea
    {"PH", 12.879721, 121.774017},   // Philippines
    {"PK", 30.375321, 69.345116},    // Pakistan
    {"PL", 51.919438, 19.145136},    // Poland
    {"PM", 46.941936, -56.27111},    // Saint Pierre and Miquelon
    {"PN", -24.703615, -127.439308}, // Pitcairn Islands
    {"PR", 18.220833, -66.590149},   // Puerto Rico
    {"PS", 31.952162, 35.233154},    // Palestinian Territories
    {"PT", 39.399872, -8.224454},    // Portugal
    {"PW", 7.51498, 134.58252},      // Palau
    {"PY", -23.442503, -58.443832},  // Paraguay
    {"QA", 25.354826, 51.183884},    // Qatar
    {"RE", -21.115141, 55.536384},   // Réunion
    {"RO", 45.943161, 24.96676},     // Romania
    {"RS", 44.016521, 21.005859},    // Serbia
    {"RU", 61.52401, 105.318756},    // Russia
    {"RW", -1.940278, 29.873888},    // Rwanda
    {"SA", 23.885942, 45.079162},    // Saudi Arabia
    {"SB", -9.64571, 160.156194},    // Solomon Islands
    {"SC", -4.679574, 55.491977},    // Seychelles
    {"SD", 12.862807, 30.217636},    // Sudan
    {"SE", 60.128161, 18.643501},    // Sweden
    {"SG", 1.352083, 103.819836},    // Singapore
    {"SH", -24.143474, -10.030696},  // Saint Helena
    {"SI", 46.151241, 14.995463},    // Slovenia
    {"SJ", 77.553604, 23.670272},    // Svalbard and Jan Mayen
    {"SK", 48.669026, 19.699024},    // Slovakia
    {"SL", 8.460555, -11.779889},    // Sierra Leone
    {"SM", 43.94236, 12.457777},     // San Marino
    {"SN", 14.497401, -14.452362},   // Senegal
    {"SO", 5.152149, 46.199616},     // Somalia
    {"SR", 3.919305, -56.027783},    // Suriname
    {"ST", 0.18636, 6.613081},       // São Tomé and Príncipe
    {"SV", 13.794185, -88.89653},    // El Salvador
    {"SY", 34.802075, 38.996815},    // Syria
    {"SZ", -26.522503, 31.465866},   // Swaziland
    {"TC", 21.694025, -71.797928},   // Turks and Caicos Islands
    {"TD", 15.454166, 18.732207},    // Chad
    {"TF", -49.280366, 69.348557},   // French Southern Territories
    {"TG", 8.619543, 0.824782},      // Togo
    {"TH", 15.870032, 100.992541},   // Thailand
    {"TJ", 38.861034, 71.276093},    // Tajikistan
    {"TK", -8.967363, -171.855881},  // Tokelau
    {"TL", -8.874217, 125.727539},   // Timor-Leste
    {"TM", 38.969719, 59.556278},    // Turkmenistan
    {"TN", 33.886917, 9.537499},     // Tunisia
    {"TO", -21.178986, -175.198242}, // Tonga
    {"TR", 38.963745, 35.243322},    // Turkey
    {"TT", 10.691803, -61.222503},   // Trinidad and Tobago
    {"TV", -7.109535, 177.64933},    // Tuvalu
    {"TW", 23.69781, 120.960515},    // Taiwan
    {"TZ", -6.369028, 34.888822},    // Tanzania
    {"UA", 48.379433, 31.16558},     // Ukraine
    {"UG", 1.373333, 32.290275},     // Uganda
    {"US", 37.09024, -95.712891},    // United States
    {"UY", -32.522779, -55.765835},  // Uruguay
    {"UZ", 41.377491, 64.585262},    // Uzbekistan
    {"VA", 41.902916, 12.453389},    // Vatican City
    {"VC", 12.984305, -61.287228},   // Saint Vincent and the Grenadines
    {"VE", 6.42375, -66.58973},      // Venezuela
    {"VG", 18.420695, -64.639968},   // British Virgin Islands
    {"VI", 18.335765, -64.896335},   // U.S. Virgin Islands
    {"VN", 14.058324, 108.277199},   // Vietnam
    {"VU", -15.376706, 166.959158},  // Vanuatu
    {"WF", -13.768752, -177.156097}, // Wallis and Futuna
    {"WS", -13.759029, -172.104629}, // Samoa
    {"YE", 15.552727, 48.516388},    // Yemen
    {"YT", -12.8275, 45.166244},     // Mayotte
    {"ZA", -30.559482, 22.937506},   // South Africa
    {"ZM", -13.133897, 27.849332},   // Zambia
    {"ZW", -19.015438, 29.154857},   // Zimbabwe
});

static_assert(std::ranges::is_sorted(country_centroids, {}, &Centroid::code));

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// A host as a browser would send it: letters, digits, dots, hyphens and an optional :port. Nothing
// that could leave a URL's authority or an HTML attribute.
bool valid_host(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-') {
        return false;
    }
    return std::ranges::all_of(host, [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '.' || c == '-' || c == ':';
    });
}

// A coordinate as the operator writes it (a string, e.g. "60.67488"), or a JSON number.
std::expected<double, std::string>
coordinate(const nlohmann::json& value, std::string_view field, double limit) {
    double parsed = 0;
    if (value.is_number()) {
        parsed = value.get<double>();
    } else if (value.is_string()) {
        const std::string& text = value.get_ref<const std::string&>();
        // strtod, not from_chars: the floating-point from_chars needs macOS 26. A leading space or
        // sign word is refused by the end check and the first character.
        char* end = nullptr;
        parsed = text.empty() ? 0 : std::strtod(text.c_str(), &end);
        const bool starts_well =
            !text.empty() && (std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '-' ||
                              text[0] == '+' || text[0] == '.');
        if (!starts_well || end != text.c_str() + text.size()) {
            return std::unexpected(std::format("{} must be a number, not \"{}\"", field, text));
        }
    } else {
        return std::unexpected(std::format("{} must be a number", field));
    }
    if (!std::isfinite(parsed) || parsed < -limit || parsed > limit) {
        return std::unexpected(std::format("{} must be between -{} and {}", field, limit, limit));
    }
    return parsed;
}

std::expected<std::string, std::string>
text_field(const nlohmann::json& entry, std::string_view key, std::string_view field) {
    const auto found = entry.find(std::string(key));
    if (found == entry.end() || !found->is_string() || found->get<std::string>().empty()) {
        return std::unexpected(std::format("{} must be a non-empty string", field));
    }
    return found->get<std::string>();
}

} // namespace

const Region* Regions::find(std::string_view region_name) const {
    const auto found = std::ranges::find(list, region_name, &Region::name);
    return found == list.end() ? nullptr : &*found;
}

std::expected<Regions, std::string> regions_from_environment(std::string_view region,
                                                             std::string_view region_label,
                                                             std::string_view regions_json,
                                                             std::string_view apex_host) {
    Regions regions;
    regions.name = std::string(region);
    regions.label = std::string(region_label);
    if (!regions_json.empty()) {
        const nlohmann::json parsed = nlohmann::json::parse(regions_json, nullptr, false);
        if (parsed.is_discarded()) {
            return std::unexpected("LAKE_REGIONS is not valid JSON");
        }
        if (!parsed.is_array()) {
            return std::unexpected("LAKE_REGIONS must be a JSON array of regions");
        }
        std::set<std::string, std::less<>> names;
        for (std::size_t index = 0; index < parsed.size(); ++index) {
            const nlohmann::json& entry = parsed[index];
            const std::string at = std::format("LAKE_REGIONS[{}]", index);
            if (!entry.is_object()) {
                return std::unexpected(at + " must be an object");
            }
            Region item;
            for (auto [key, target] : {std::pair{"name", &item.name},
                                       std::pair{"label", &item.label},
                                       std::pair{"host", &item.host}}) {
                auto value = text_field(entry, key, at + "." + key);
                if (!value) {
                    return std::unexpected(value.error());
                }
                *target = std::move(*value);
            }
            if (!std::ranges::all_of(item.name, [](unsigned char c) {
                    return std::isalnum(c) != 0 || c == '-' || c == '_';
                })) {
                return std::unexpected(std::format(
                    "{}.name must be letters, digits, hyphens and underscores, not \"{}\"",
                    at,
                    item.name));
            }
            item.host = lower(item.host);
            if (!valid_host(item.host)) {
                return std::unexpected(std::format(
                    "{}.host must be a host name such as eu.lake.example.com, not \"{}\"", at, item.host));
            }
            if (!names.insert(item.name).second) {
                return std::unexpected(
                    std::format("LAKE_REGIONS names the region \"{}\" twice", item.name));
            }
            const auto latitude =
                coordinate(entry.value("latitude", nlohmann::json()), at + ".latitude", 90);
            if (!latitude) {
                return std::unexpected(latitude.error());
            }
            const auto longitude =
                coordinate(entry.value("longitude", nlohmann::json()), at + ".longitude", 180);
            if (!longitude) {
                return std::unexpected(longitude.error());
            }
            item.latitude = *latitude;
            item.longitude = *longitude;
            regions.list.push_back(std::move(item));
        }
    }
    if (!regions.name.empty() && !regions.list.empty()) {
        const Region* own = regions.find(regions.name);
        if (own == nullptr) {
            return std::unexpected(std::format(
                "LAKE_REGION \"{}\" is not one of the regions in LAKE_REGIONS", regions.name));
        }
        if (regions.label.empty()) {
            regions.label = own->label;
        }
    }
    if (regions.label.empty()) {
        regions.label = regions.name;
    }
    if (!apex_host.empty()) {
        regions.apex_host = lower(apex_host);
        if (!valid_host(regions.apex_host)) {
            return std::unexpected("LAKE_APEX_HOST must be a host name such as lake.example.com");
        }
        if (regions.list.empty()) {
            return std::unexpected("LAKE_APEX_HOST needs LAKE_REGIONS: the picker lists them");
        }
    }
    return regions;
}

std::optional<GeoPoint> country_centroid(std::string_view iso_alpha2) {
    if (iso_alpha2.size() != 2) {
        return std::nullopt;
    }
    const std::string code{
        static_cast<char>(std::toupper(static_cast<unsigned char>(iso_alpha2[0]))),
        static_cast<char>(std::toupper(static_cast<unsigned char>(iso_alpha2[1])))};
    const auto found =
        std::ranges::lower_bound(country_centroids, std::string_view(code), {}, &Centroid::code);
    if (found == country_centroids.end() || found->code != code) {
        return std::nullopt;
    }
    return GeoPoint{found->latitude, found->longitude};
}

double great_circle_km(GeoPoint from, GeoPoint to) {
    constexpr double earth_radius_km = 6371.0088; // the IUGG mean radius
    constexpr double radians = std::numbers::pi / 180.0;
    const double lat1 = from.latitude * radians;
    const double lat2 = to.latitude * radians;
    const double half_lat = (to.latitude - from.latitude) * radians / 2;
    const double half_lon = (to.longitude - from.longitude) * radians / 2;
    const double a = std::sin(half_lat) * std::sin(half_lat) +
                     std::cos(lat1) * std::cos(lat2) * std::sin(half_lon) * std::sin(half_lon);
    return 2 * earth_radius_km * std::asin(std::min(1.0, std::sqrt(a)));
}

const Region* closest_region(std::span<const Region> regions, std::string_view country) {
    if (regions.empty()) {
        return nullptr;
    }
    const auto centroid = country_centroid(country);
    if (!centroid) {
        return &regions.front();
    }
    const Region* best = &regions.front();
    double best_km = great_circle_km(*centroid, {best->latitude, best->longitude});
    for (const Region& candidate : regions.subspan(1)) {
        const double km = great_circle_km(*centroid, {candidate.latitude, candidate.longitude});
        if (km < best_km) {
            best = &candidate;
            best_km = km;
        }
    }
    return best;
}

const Region*
preselected_region(const Regions& regions, std::string_view preference, std::string_view country) {
    if (const Region* chosen = regions.find(preference); chosen != nullptr) {
        return chosen;
    }
    return closest_region(regions.list, country);
}

std::string region_preference_cookie(std::string_view region_name, bool secure) {
    std::string cookie = std::format("{}={}; Path=/; Max-Age={}; HttpOnly; SameSite=Lax",
                                     region_cookie_name,
                                     region_name,
                                     region_cookie_seconds);
    if (secure) {
        cookie += "; Secure";
    }
    return cookie;
}

std::string region_continue_url(const Region& region, std::string_view target) {
    const bool origin_form =
        !target.empty() && target.front() == '/' &&
        std::ranges::none_of(target, [](unsigned char c) { return c < 0x21 || c == 0x7f; });
    return "https://" + region.host + (origin_form ? std::string(target) : std::string("/"));
}

nlohmann::json region_json(const Regions* regions) {
    if (regions == nullptr || regions->name.empty()) {
        return nullptr;
    }
    return {{"name", regions->name}, {"label", regions->label}};
}

nlohmann::json regions_json(const Regions* regions) {
    nlohmann::json list = nlohmann::json::array();
    if (regions != nullptr) {
        for (const Region& region : regions->list) {
            list.push_back({{"name", region.name}, {"label", region.label}, {"host", region.host}});
        }
    }
    return list;
}

nlohmann::json order_sheet_url_json(const Regions* regions) {
    if (regions == nullptr || regions->order_sheet_url.empty()) {
        return nullptr;
    }
    return regions->order_sheet_url;
}

} // namespace lake::api
