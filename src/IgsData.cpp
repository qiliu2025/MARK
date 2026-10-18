#include "IgsData.h"
#include "ObservationStationDates.h"

#include "DateUtil.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"
#include "ObservationSearchDb.h"
#include "NetworkAvailabilityDb.h"
#include "SpaceWeather.h"
#include "SiteLogDb.h"

#include <windows.h>

#include "../resources/resource.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <format>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

struct CatalogEntry {
    std::string id;
    std::string country;
    double latitude = 0.0;
    double longitude = 0.0;
};

struct Catalog {
    std::unordered_map<std::string, CatalogEntry> byId;
    std::unordered_map<std::string, std::vector<std::string>> byFourChar;
};

struct FileCandidate {
    std::string stationId;
    std::string fourCharId;
    std::string fileName;
    // Optional path below the date directory.  PANGA stores observations in
    // interval-specific subdirectories (30sec/, 10sec/, rinex3/, ...), while
    // most archives list files directly in the date directory.
    std::string downloadPath;
    RinexFormat format = RinexFormat::Rinex34;
    std::string interval;
    char source = 'R';
};

struct SourceResult {
    std::vector<StationFile> stations;
    std::vector<StationFile> files;
    std::wstring error;
    std::wstring warning;
    bool fromLocalIndex = false;
    bool fromIgsLocalIndex = false;
    int igsSourcesAttempted = 0;
    int igsSourcesSuccessful = 0;
    int igsNetworkFailures = 0;
    std::vector<std::string> igsFailedSources;
    bool networkTransportFailure = false;
    long transportErrorCode = 0;
    bool emptyResultIsComplete = false;
};

// GFZ/BKG coverage windows avoid probes outside published high-rate holdings.
// Empty results inside continuous windows are eligible for rescanning.
bool GfzHighrateMayExist(const QueryDate& date) {
    if (date.year < 2018) return false;
    if (date.year == 2018) return date.dayOfYear == 121;
    if (date.year == 2019) return date.dayOfYear == 287;
    if (date.year >= 2020 && date.year <= 2025) return true;
    if (date.year == 2026) return date.dayOfYear <= 254;
    return false;
}

bool BkgHighrateMayExist(const QueryDate& date) {
    if (date.year < 2020) return false;
    if (date.year == 2020) return date.dayOfYear >= 128
        && date.dayOfYear <= 250;
    if (date.year == 2021) return date.dayOfYear >= 24
        && date.dayOfYear <= 268;
    if (date.year == 2022) return date.dayOfYear >= 7;
    if (date.year >= 2023 && date.year <= 2025) return true;
    if (date.year == 2026) return date.dayOfYear <= 254;
    return false;
}

bool IsNetworkTransportFailure(const HttpTextResult& response) {
    switch (response.transportErrorCode) {
        case 5:   // proxy resolution
        case 6:   // host resolution
        case 7:   // connect
        case 28:  // timeout
        case 35:  // TLS handshake
        case 52:  // empty response
        case 55:  // send failure
        case 56:  // receive failure
            return true;
        default:
            return false;
    }
}

void RecordTransportFailure(
    SourceResult& result,
    const HttpTextResult& response) {
    if (!IsNetworkTransportFailure(response)) {
        return;
    }
    result.networkTransportFailure = true;
    if (result.transportErrorCode == 0) {
        result.transportErrorCode = response.transportErrorCode;
    }
}

std::wstring Utf8ToWide(const std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (count <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        count);
    return result;
}

std::string ToUpperAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return value;
}

std::string ToLowerAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string TrimAscii(std::string value) {
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

bool IsSafeArchiveFileName(const std::string_view fileName) {
    if (fileName.empty() || fileName.size() > 220
        || fileName.find("..") != std::string_view::npos) {
        return false;
    }
    return std::all_of(
        fileName.begin(),
        fileName.end(),
        [](const unsigned char character) {
            return std::isalnum(character) != 0
                || character == '_'
                || character == '-'
                || character == '.';
        });
}

bool IsSafeEposDownloadUrl(const std::string_view url) {
    constexpr std::string_view https = "https://";
    constexpr std::string_view http = "http://";
    const std::size_t authorityStart = url.starts_with(https)
        ? https.size()
        : (url.starts_with(http) ? http.size() : std::string_view::npos);
    if (authorityStart == std::string_view::npos
        || url.size() > 2048
        || url.find("..") != std::string_view::npos) {
        return false;
    }
    const auto pathStart = url.find('/', authorityStart);
    const auto authority = url.substr(
        authorityStart,
        pathStart == std::string_view::npos
            ? url.size() - authorityStart
            : pathStart - authorityStart);
    if (authority.empty() || authority.find('@') != std::string_view::npos) {
        return false;
    }
    return std::none_of(
        url.begin(),
        url.end(),
        [](const unsigned char character) {
            return character <= 0x20 || character == 0x7f
                || character == '\\';
        });
}

std::string ExtractQuotedValue(const std::string& line) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return {};
    }
    const auto firstQuote = line.find('"', colon + 1);
    if (firstQuote == std::string::npos) {
        return TrimAscii(line.substr(colon + 1));
    }
    const auto secondQuote = line.find('"', firstQuote + 1);
    if (secondQuote == std::string::npos) {
        return {};
    }
    return line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

std::string LoadEmbeddedResource(const int resourceId) {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        module,
        MAKEINTRESOURCEW(resourceId),
        RT_RCDATA);
    if (resource == nullptr) {
        return {};
    }
    const HGLOBAL loaded = LoadResource(module, resource);
    if (loaded == nullptr) {
        return {};
    }
    const DWORD size = SizeofResource(module, resource);
    const void* data = LockResource(loaded);
    if (data == nullptr || size == 0) {
        return {};
    }
    return std::string(static_cast<const char*>(data), size);
}

void AddCatalogEntry(Catalog& catalog, CatalogEntry entry) {
    entry.id = ToUpperAscii(std::move(entry.id));
    if (entry.id.size() != 4 && entry.id.size() != 9) {
        return;
    }
    const std::string four = entry.id.substr(0, 4);
    catalog.byId.insert_or_assign(entry.id, entry);
    catalog.byFourChar[four].push_back(entry.id);
}

void FinishCatalog(Catalog& catalog) {
    for (auto& [fourChar, ids] : catalog.byFourChar) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
}

Catalog ParseCatalogJson(const std::string& json) {
    Catalog catalog;
    std::istringstream stream(json);
    std::string line;
    std::string currentId;
    std::string country;
    double latitude = 0.0;
    bool hasLatitude = false;

    while (std::getline(stream, line)) {
        const auto firstNonSpace = line.find_first_not_of(' ');
        if (firstNonSpace == std::string::npos) {
            continue;
        }

        if (firstNonSpace == 4 && line.size() > 17 && line[4] == '"') {
            const auto closingQuote = line.find('"', 5);
            const auto objectStart = line.find('{', closingQuote);
            if (closingQuote != std::string::npos
                && objectStart != std::string::npos
                && closingQuote - 5 == 9) {
                currentId = ToUpperAscii(line.substr(5, 9));
                country.clear();
                latitude = 0.0;
                hasLatitude = false;
                continue;
            }
        }

        if (currentId.empty()) {
            continue;
        }
        if (line.find("\"CountryOrRegion\"") != std::string::npos) {
            country = ExtractQuotedValue(line);
        } else if (line.find("\"Latitude\"") != std::string::npos) {
            const auto value = ExtractQuotedValue(line);
            char* end = nullptr;
            latitude = std::strtod(value.c_str(), &end);
            hasLatitude = end != value.c_str();
        } else if (line.find("\"Longitude\"") != std::string::npos) {
            const auto value = ExtractQuotedValue(line);
            char* end = nullptr;
            const double longitude = std::strtod(value.c_str(), &end);
            if (hasLatitude && end != value.c_str()
                && std::isfinite(latitude) && std::isfinite(longitude)) {
                AddCatalogEntry(
                    catalog,
                    CatalogEntry{
                        currentId,
                        country,
                        latitude,
                        longitude
                    });
            }
        }
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog ParseCoordinateTsv(const std::string& text) {
    Catalog catalog;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream row(line);
        CatalogEntry entry;
        if (std::getline(row, entry.id, '\t')
            && std::getline(row, entry.country, '\t')) {
            std::string latitude;
            std::string longitude;
            if (!std::getline(row, latitude, '\t')
                || !std::getline(row, longitude, '\t')) {
                continue;
            }
            char* latitudeEnd = nullptr;
            char* longitudeEnd = nullptr;
            entry.latitude = std::strtod(latitude.c_str(), &latitudeEnd);
            entry.longitude = std::strtod(longitude.c_str(), &longitudeEnd);
            if (latitudeEnd != latitude.c_str()
                && longitudeEnd != longitude.c_str()
                && std::isfinite(entry.latitude)
                && std::isfinite(entry.longitude)) {
                AddCatalogEntry(catalog, std::move(entry));
            }
        }
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog MergeCatalogs(
    const std::initializer_list<const Catalog*> catalogs) {
    Catalog merged;
    for (const Catalog* catalog : catalogs) {
        if (catalog == nullptr) {
            continue;
        }
        for (const auto& [id, entry] : catalog->byId) {
            AddCatalogEntry(merged, entry);
        }
    }
    FinishCatalog(merged);
    return merged;
}

Catalog ParseRamsacGeoJsonCoordinates(const std::string& json) {
    Catalog catalog;
    const std::regex featurePattern(
        R"REGEX("coordinates"\s*:\s*\[\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\][\s\S]{0,1200}?"codigo_estacion"\s*:\s*"([A-Za-z0-9]{4})")REGEX",
        std::regex::icase);
    for (std::sregex_iterator iterator(
             json.begin(),
             json.end(),
             featurePattern),
         end;
         iterator != end;
        ++iterator) {
        const auto& match = *iterator;
        const std::string longitudeText = match[1].str();
        const std::string latitudeText = match[2].str();
        char* longitudeEnd = nullptr;
        char* latitudeEnd = nullptr;
        const double longitude =
            std::strtod(longitudeText.c_str(), &longitudeEnd);
        const double latitude =
            std::strtod(latitudeText.c_str(), &latitudeEnd);
        if (longitudeEnd == longitudeText.c_str()
            || latitudeEnd == latitudeText.c_str()
            || !std::isfinite(latitude)
            || !std::isfinite(longitude)) {
            continue;
        }
        const std::string code = ToUpperAscii(match[3].str());
        std::string country = "ARG";
        if (code.starts_with("UY")) {
            country = "URY";
        } else if (code.starts_with("PY")) {
            country = "PRY";
        } else if (latitude < -60.0) {
            country = "ATA";
        }
        AddCatalogEntry(
            catalog,
            {code, std::move(country), latitude, longitude});
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog ParseGeoNetCoordinateGeoJson(const std::string& json) {
    Catalog catalog;
    // GeoNet's official /network/station endpoint returns one GeoJSON
    // feature per sensor site.  Code precedes the point geometry inside each
    // feature, so this remains independent of optional receiver/antenna data.
    const std::regex featurePattern(
        R"REGEX("Code"\s*:\s*"([A-Za-z0-9]{4})"[\s\S]{0,900}?"coordinates"\s*:\s*\[\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\])REGEX",
        std::regex::icase);
    for (std::sregex_iterator iterator(
             json.begin(), json.end(), featurePattern),
         end;
         iterator != end;
         ++iterator) {
        const auto& match = *iterator;
        const std::string longitudeText = match[2].str();
        const std::string latitudeText = match[3].str();
        char* longitudeEnd = nullptr;
        char* latitudeEnd = nullptr;
        const double longitude =
            std::strtod(longitudeText.c_str(), &longitudeEnd);
        const double latitude =
            std::strtod(latitudeText.c_str(), &latitudeEnd);
        if (longitudeEnd == longitudeText.c_str()
            || latitudeEnd == latitudeText.c_str()
            || !std::isfinite(latitude)
            || !std::isfinite(longitude)
            || std::abs(latitude) > 90.0
            || std::abs(longitude) > 180.0) {
            continue;
        }
        AddCatalogEntry(
            catalog,
            {ToUpperAscii(match[1].str()), "NZL", latitude, longitude});
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog ParseRgpCoordinateCsv(const std::string& text) {
    Catalog catalog;
    // IGN coordRGP.php publishes signed DD.MMSS, not decimal degrees.
    const auto degrees = [](const double encoded) -> std::optional<double> {
        if (!std::isfinite(encoded)) return std::nullopt;
        const double absolute = std::abs(encoded);
        const double whole = std::floor(absolute);
        const double packedMinutes = (absolute - whole) * 100.0;
        const double minutes = std::floor(packedMinutes + 1e-10);
        const double seconds = (packedMinutes - minutes) * 100.0;
        if (minutes < 0.0 || minutes >= 60.0
            || seconds < -1e-7 || seconds >= 60.0000001) return std::nullopt;
        return std::copysign(whole + minutes / 60.0
            + std::max(0.0, seconds) / 3600.0, encoded);
    };
    std::istringstream stream(text);
    std::string line;
    bool firstLine = true;
    while (std::getline(stream, line)) {
        if (firstLine) {
            firstLine = false;
            continue;
        }
        std::vector<std::string> fields;
        std::string field;
        bool quoted = false;
        for (std::size_t index = 0; index <= line.size(); ++index) {
            const char character =
                index < line.size() ? line[index] : ';';
            if (character == '"') {
                quoted = !quoted;
            } else if (character == ';' && !quoted) {
                fields.push_back(TrimAscii(std::move(field)));
                field.clear();
            } else {
                field.push_back(character);
            }
        }
        if (fields.size() < 5) {
            continue;
        }
        std::string id = ToUpperAscii(fields[1]);
        if (id.size() != 9) {
            id = ToUpperAscii(fields[0]);
        }
        char* latitudeEnd = nullptr;
        char* longitudeEnd = nullptr;
        const double encodedLatitude =
            std::strtod(fields[3].c_str(), &latitudeEnd);
        const double encodedLongitude =
            std::strtod(fields[4].c_str(), &longitudeEnd);
        const auto latitude = degrees(encodedLatitude);
        const auto longitude = degrees(encodedLongitude);
        if (latitudeEnd == fields[3].c_str()
            || longitudeEnd == fields[4].c_str()
            || !latitude || !longitude
            || std::abs(*latitude) > 90.0 || std::abs(*longitude) > 180.0) {
            continue;
        }
        std::string country =
            id.size() == 9 ? id.substr(6, 3) : "FRA";
        AddCatalogEntry(
            catalog,
            {std::move(id), std::move(country), *latitude, *longitude});
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog ParseSopacAprCoordinates(const std::string& text) {
    Catalog catalog;
    std::istringstream stream(text);
    std::string line;
    constexpr double pi = 3.14159265358979323846;
    constexpr double semiMajorAxis = 6'378'137.0;
    constexpr double eccentricitySquared = 6.6943799901413165e-3;
    const double semiMinorAxis =
        semiMajorAxis * std::sqrt(1.0 - eccentricitySquared);
    const double secondEccentricitySquared =
        (semiMajorAxis * semiMajorAxis
         - semiMinorAxis * semiMinorAxis)
        / (semiMinorAxis * semiMinorAxis);

    while (std::getline(stream, line)) {
        line = TrimAscii(std::move(line));
        if (line.empty() || line.front() == '#'
            || line.front() == '+') {
            continue;
        }
        std::istringstream row(line);
        std::string site;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        if (!(row >> site >> x >> y >> z)
            || site.size() < 4
            || !std::isfinite(x)
            || !std::isfinite(y)
            || !std::isfinite(z)) {
            continue;
        }
        const std::string code =
            ToUpperAscii(site.substr(0, 4));
        if (!std::all_of(
                code.begin(),
                code.end(),
                [](const unsigned char character) {
                    return std::isalnum(character) != 0;
                })) {
            continue;
        }
        const double horizontal = std::hypot(x, y);
        if (horizontal < 1.0) {
            continue;
        }
        const double theta = std::atan2(
            z * semiMajorAxis,
            horizontal * semiMinorAxis);
        const double sineTheta = std::sin(theta);
        const double cosineTheta = std::cos(theta);
        const double latitude = std::atan2(
            z + secondEccentricitySquared
                    * semiMinorAxis
                    * sineTheta * sineTheta * sineTheta,
            horizontal - eccentricitySquared
                    * semiMajorAxis
                    * cosineTheta * cosineTheta * cosineTheta);
        const double longitude = std::atan2(y, x);
        AddCatalogEntry(
            catalog,
            {
                code,
                {},
                latitude * 180.0 / pi,
                longitude * 180.0 / pi
            });
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog ParseHtmlLatitudeLongitudeCatalog(
    const std::string& html,
    const std::string_view country,
    const bool longId) {
    Catalog catalog;
    const std::string idExpression = longId
        ? R"(([A-Z0-9]{9}))"
        : R"(([A-Z0-9]{4}))";
    const std::regex rowPattern(
        idExpression
            + R"([\s\S]{0,900}?([-+]?[0-9]{1,2}[\.,][0-9]{3,})[\s\S]{0,180}?([-+]?[0-9]{1,3}[\.,][0-9]{3,}))",
        std::regex::icase);
    for (std::sregex_iterator iterator(
             html.begin(), html.end(), rowPattern),
         end;
         iterator != end;
         ++iterator) {
        std::string latitudeText = (*iterator)[2].str();
        std::string longitudeText = (*iterator)[3].str();
        std::replace(latitudeText.begin(), latitudeText.end(), ',', '.');
        std::replace(longitudeText.begin(), longitudeText.end(), ',', '.');
        char* latitudeEnd = nullptr;
        char* longitudeEnd = nullptr;
        const double latitude =
            std::strtod(latitudeText.c_str(), &latitudeEnd);
        const double longitude =
            std::strtod(longitudeText.c_str(), &longitudeEnd);
        if (latitudeEnd == latitudeText.c_str()
            || longitudeEnd == longitudeText.c_str()
            || std::abs(latitude) > 90.0
            || std::abs(longitude) > 180.0) {
            continue;
        }
        AddCatalogEntry(
            catalog,
            {ToUpperAscii((*iterator)[1].str()),
             std::string(country), latitude, longitude});
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog ParseDpgaXyzCatalog(const std::string& html) {
    Catalog catalog;
    const std::regex rowPattern(
        R"(([A-Z0-9]{4})\s+[0-9]{5}[A-Z][0-9]{3}\s+([-+0-9.]+)\s+([-+0-9.]+)\s+([-+0-9.]+))",
        std::regex::icase);
    std::string apr;
    for (std::sregex_iterator iterator(
             html.begin(), html.end(), rowPattern),
         end;
         iterator != end;
         ++iterator) {
        apr += (*iterator)[2].str() + " "
            + (*iterator)[3].str() + " "
            + (*iterator)[4].str() + " "
            + (*iterator)[5].str() + "\n";
    }
    catalog = ParseSopacAprCoordinates(apr);
    for (auto& [id, entry] : catalog.byId) {
        entry.country = "NLD";
    }
    return catalog;
}

std::vector<std::string> ExtractHrefs(const std::string& html) {
    // Apache indexes normally use double quotes, but older generated
    // institutional listings sometimes use single quotes. Accept both.
    std::vector<std::string> hrefs;
    const std::regex hrefPattern(
        R"REGEX(href\s*=\s*([\"'])([^\"']+)\1)REGEX",
        std::regex::icase);
    for (std::sregex_iterator iterator(html.begin(), html.end(), hrefPattern),
         end; iterator != end; ++iterator) {
        const std::string value = (*iterator)[2].str();
        if (!value.empty()) hrefs.push_back(value);
    }
    return hrefs;
}

std::string BuildSyntheticDirectoryHtml(
    const std::vector<std::string>& fileNames) {
    std::string html;
    for (const auto& fileName : fileNames) {
        if (IsSafeArchiveFileName(fileName)) {
            html += "<a href=\"" + fileName + "\"></a>\n";
        }
    }
    return html;
}

std::vector<std::string> ExtractLineFileNames(const std::string& listing) {
    std::vector<std::string> names;
    std::istringstream lines(listing);
    std::string line;
    while (std::getline(lines, line)) {
        line = TrimAscii(std::move(line));
        if (line.empty()) {
            continue;
        }

        // FTP LIST prefixes filenames with metadata; MLSD uses semicolon facts.
        // Take the last valid archive-name token to handle both formats and proxy URLs.
        std::string candidate;
        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            const auto equals = token.rfind('=');
            if (equals != std::string::npos && equals + 1 < token.size()) {
                token = token.substr(equals + 1);
            }
            const auto slash = token.find_last_of("/\\");
            if (slash != std::string::npos) {
                token = token.substr(slash + 1);
            }
            while (!token.empty() && (token.front() == '\"' || token.front() == '\'')) {
                token.erase(token.begin());
            }
            while (!token.empty() && (token.back() == '\"' || token.back() == '\'' || token.back() == ';')) {
                token.pop_back();
            }
            if (IsSafeArchiveFileName(token)) {
                candidate = token;
            }
        }
        if (!candidate.empty()) {
            names.push_back(std::move(candidate));
        }
    }
    return names;
}

std::vector<std::string> ExtractXmlValues(
    const std::string& xml,
    const std::string_view tag) {
    std::vector<std::string> values;
    const std::string opening = "<" + std::string(tag) + ">";
    const std::string closing = "</" + std::string(tag) + ">";
    std::size_t position = 0;
    while ((position = xml.find(opening, position)) != std::string::npos) {
        position += opening.size();
        const auto end = xml.find(closing, position);
        if (end == std::string::npos) {
            break;
        }
        values.push_back(xml.substr(position, end - position));
        position = end + closing.size();
    }
    return values;
}

std::wstring PercentEncodeAscii(const std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::wstring result;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9')
            || character == '-'
            || character == '_'
            || character == '.'
            || character == '~') {
            result.push_back(static_cast<wchar_t>(character));
        } else {
            result.push_back(L'%');
            result.push_back(static_cast<wchar_t>(hex[character >> 4]));
            result.push_back(static_cast<wchar_t>(hex[character & 0x0f]));
        }
    }
    return result;
}

std::vector<std::string> ExtractJsonStringValues(
    const std::string& json,
    const std::string_view field) {
    std::vector<std::string> values;
    const std::string marker = "\"" + std::string(field) + "\":\"";
    std::size_t position = 0;
    while ((position = json.find(marker, position)) != std::string::npos) {
        position += marker.size();
        std::string value;
        bool escaped = false;
        for (; position < json.size(); ++position) {
            const char character = json[position];
            if (escaped) {
                if (character == '/' || character == '\\' || character == '"') {
                    value.push_back(character);
                }
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                ++position;
                break;
            } else {
                value.push_back(character);
            }
        }
        values.push_back(std::move(value));
    }
    return values;
}

std::string ExtractJsonStringValue(
    const std::string_view json,
    const std::string_view field) {
    const std::string marker = "\"" + std::string(field) + "\":";
    std::size_t position = json.find(marker);
    if (position == std::string_view::npos) {
        return {};
    }
    position += marker.size();
    while (position < json.size()
           && std::isspace(static_cast<unsigned char>(json[position])) != 0) {
        ++position;
    }
    if (position >= json.size() || json[position] != '"') {
        return {};
    }
    ++position;
    std::string value;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char character = json[position];
        if (escaped) {
            if (character == '/' || character == '\\' || character == '"') {
                value.push_back(character);
            } else if (character == 'n') {
                value.push_back('\n');
            }
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (character == '"') {
            return value;
        } else {
            value.push_back(character);
        }
    }
    return {};
}

bool ExtractJsonNumberValue(
    const std::string_view json,
    const std::string_view field,
    double& value) {
    const std::string marker = "\"" + std::string(field) + "\":";
    std::size_t position = json.find(marker);
    if (position == std::string_view::npos) {
        return false;
    }
    position += marker.size();
    while (position < json.size()
           && std::isspace(static_cast<unsigned char>(json[position])) != 0) {
        ++position;
    }
    const std::string remaining(json.substr(position));
    char* end = nullptr;
    value = std::strtod(remaining.c_str(), &end);
    return end != remaining.c_str() && std::isfinite(value);
}

std::vector<std::string> ExtractTopLevelJsonObjects(
    const std::string_view json) {
    std::vector<std::string> objects;
    std::size_t start = std::string_view::npos;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t index = 0; index < json.size(); ++index) {
        const char character = json[index];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (character == '\\') {
                escaped = true;
            } else if (character == '"') {
                inString = false;
            }
            continue;
        }
        if (character == '"') {
            inString = true;
        } else if (character == '{') {
            if (depth == 0) {
                start = index;
            }
            ++depth;
        } else if (character == '}' && depth > 0) {
            --depth;
            if (depth == 0 && start != std::string_view::npos) {
                objects.emplace_back(json.substr(start, index - start + 1));
                start = std::string_view::npos;
            }
        }
    }
    return objects;
}

std::uint16_t ReadLe16(
    const std::string_view data,
    const std::size_t offset) {
    if (offset + 2 > data.size()) return 0;
    return static_cast<std::uint16_t>(
        static_cast<unsigned char>(data[offset]))
        | static_cast<std::uint16_t>(
            static_cast<unsigned char>(data[offset + 1])) << 8;
}

std::uint32_t ReadLe32(
    const std::string_view data,
    const std::size_t offset) {
    if (offset + 4 > data.size()) return 0;
    std::uint32_t value = 0;
    for (int byte = 0; byte < 4; ++byte) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + byte]))
            << (byte * 8);
    }
    return value;
}

std::uint64_t ReadLe64(
    const std::string_view data,
    const std::size_t offset) {
    if (offset + 8 > data.size()) return 0;
    std::uint64_t value = 0;
    for (int byte = 0; byte < 8; ++byte) {
        value |= static_cast<std::uint64_t>(
            static_cast<unsigned char>(data[offset + byte]))
            << (byte * 8);
    }
    return value;
}

std::size_t FindLastSignature(
    const std::string_view data,
    const std::uint32_t signature) {
    if (data.size() < 4) return std::string_view::npos;
    for (std::size_t offset = data.size() - 4;; --offset) {
        if (ReadLe32(data, offset) == signature) return offset;
        if (offset == 0) break;
    }
    return std::string_view::npos;
}

struct RemoteZipDirectoryResult {
    std::vector<std::string> paths;
    std::unordered_map<std::string, std::wstring> memberRoutes;
    std::wstring error;
    bool networkFailure = false;
    long transportErrorCode = 0;
};

RemoteZipDirectoryResult ListRemoteZipDirectory(
    const std::wstring& url,
    const std::stop_token stopToken) {
    RemoteZipDirectoryResult result;
    constexpr std::size_t tailLimit = 128U * 1024U;
    const auto tail = HttpGetByteRange(
        url, L"-131072", stopToken, tailLimit + 1024U);
    if (!tail.ok()) {
        result.error = tail.error;
        result.networkFailure = IsNetworkTransportFailure(tail);
        result.transportErrorCode = tail.transportErrorCode;
        return result;
    }
    constexpr std::uint32_t eocdSignature = 0x06054b50U;
    constexpr std::uint32_t centralSignature = 0x02014b50U;
    constexpr std::uint32_t zip64LocatorSignature = 0x07064b50U;
    constexpr std::uint32_t zip64EocdSignature = 0x06064b50U;
    const auto eocd = FindLastSignature(tail.body, eocdSignature);
    if (eocd == std::string_view::npos || eocd + 22 > tail.body.size()) {
        result.error = L"远程 ZIP 缺少有效的中央目录结束记录。";
        return result;
    }
    std::uint64_t centralSize = ReadLe32(tail.body, eocd + 12);
    std::uint64_t centralOffset = ReadLe32(tail.body, eocd + 16);
    if (centralSize == 0xffffffffULL
        || centralOffset == 0xffffffffULL) {
        const std::string_view beforeEocd(
            tail.body.data(), eocd);
        const auto locator =
            FindLastSignature(beforeEocd, zip64LocatorSignature);
        if (locator == std::string_view::npos
            || locator + 20 > tail.body.size()) {
            result.error = L"远程 ZIP64 缺少中央目录定位记录。";
            return result;
        }
        const std::uint64_t zip64Offset =
            ReadLe64(tail.body, locator + 8);
        const auto zip64 = HttpGetByteRange(
            url,
            std::format(L"{}-{}", zip64Offset, zip64Offset + 55),
            stopToken,
            4096U);
        if (!zip64.ok()
            || zip64.body.size() < 56
            || ReadLe32(zip64.body, 0) != zip64EocdSignature) {
            result.error = zip64.error.empty()
                ? L"无法读取远程 ZIP64 中央目录记录。"
                : zip64.error;
            result.networkFailure = IsNetworkTransportFailure(zip64);
            result.transportErrorCode = zip64.transportErrorCode;
            return result;
        }
        centralSize = ReadLe64(zip64.body, 40);
        centralOffset = ReadLe64(zip64.body, 48);
    }
    constexpr std::uint64_t maximumCentralDirectory =
        64ULL * 1024ULL * 1024ULL;
    if (centralSize == 0 || centralSize > maximumCentralDirectory
        || centralOffset > UINT64_MAX - centralSize) {
        result.error = L"远程 ZIP 中央目录大小无效或超过 64 MB。";
        return result;
    }
    const auto central = HttpGetByteRange(
        url,
        std::format(
            L"{}-{}", centralOffset,
            centralOffset + centralSize - 1),
        stopToken,
        static_cast<std::size_t>(centralSize + 1024));
    if (!central.ok()) {
        result.error = central.error;
        result.networkFailure = IsNetworkTransportFailure(central);
        result.transportErrorCode = central.transportErrorCode;
        return result;
    }
    std::size_t offset = 0;
    while (offset + 46 <= central.body.size()
           && ReadLe32(central.body, offset) == centralSignature) {
        const std::size_t nameLength = ReadLe16(central.body, offset + 28);
        const std::size_t extraLength = ReadLe16(central.body, offset + 30);
        const std::size_t commentLength = ReadLe16(central.body, offset + 32);
        const std::size_t next = offset + 46 + nameLength
            + extraLength + commentLength;
        if (next > central.body.size()) {
            result.paths.clear();
            result.error = L"远程 ZIP 中央目录记录被截断。";
            return result;
        }
        std::string path = central.body.substr(offset + 46, nameLength);
        if (!path.empty() && path.back() != '/'
            && path.find("..") == std::string::npos) {
            // ZIP64 stores only overflowing fields in the documented order.
            std::uint64_t rawSize = ReadLe32(central.body, offset + 24);
            std::uint64_t compressedSize = ReadLe32(central.body, offset + 20);
            std::uint64_t localOffset = ReadLe32(central.body, offset + 42);
            const auto method = ReadLe16(central.body, offset + 10);
            const auto crc = ReadLe32(central.body, offset + 16);
            bool valid = true;
            for (std::size_t extra = offset + 46 + nameLength;
                 extra + 4 <= offset + 46 + nameLength + extraLength;) {
                const auto tag = ReadLe16(central.body, extra);
                const auto size = ReadLe16(central.body, extra + 2);
                const auto end = extra + 4 + size;
                if (end > offset + 46 + nameLength + extraLength) {
                    valid = false;
                    break;
                }
                if (tag == 1) {
                    std::size_t field = extra + 4;
                    for (auto* value : {&rawSize, &compressedSize, &localOffset}) {
                        if (*value == 0xffffffffULL) {
                            if (field + 8 > end) { valid = false; break; }
                            *value = ReadLe64(central.body, field);
                            field += 8;
                        }
                    }
                }
                extra = end;
            }
            constexpr std::uint64_t memberLimit = 64ULL * 1024ULL * 1024ULL;
            if (valid && rawSize && compressedSize && rawSize <= memberLimit
                && compressedSize <= memberLimit && localOffset != 0xffffffffULL
                && (method == 0 || method == 8)) {
                const auto slash = path.find_last_of("/\\");
                const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
                result.memberRoutes.try_emplace(name, url + std::format(
                    L"#gnss-zip={},{},{},{},{}", localOffset, compressedSize, rawSize, method, crc));
            }
            result.paths.push_back(std::move(path));
        }
        offset = next;
    }
    if (result.paths.empty()) {
        result.error = L"远程 ZIP 中央目录中没有文件。";
    }
    return result;
}

Catalog ParseArcGisGeoJsonCoordinates(
    const std::string& json,
    const std::string_view idField,
    const std::string_view country) {
    Catalog catalog;
    const auto features = json.find("\"features\"");
    const auto arrayStart = features == std::string::npos
        ? std::string::npos
        : json.find('[', features);
    if (arrayStart == std::string::npos) {
        return catalog;
    }
    const std::regex coordinatePattern(
        R"REGEX("coordinates"\s*:\s*\[\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\])REGEX",
        std::regex::icase);
    for (const auto& object : ExtractTopLevelJsonObjects(
             std::string_view(json).substr(arrayStart))) {
        const std::string code = ToUpperAscii(
            ExtractJsonStringValue(object, idField));
        std::smatch coordinates;
        if (code.size() != 4
            || !std::regex_search(object, coordinates, coordinatePattern)) {
            continue;
        }
        const std::string longitudeText = coordinates[1].str();
        const std::string latitudeText = coordinates[2].str();
        char* longitudeEnd = nullptr;
        char* latitudeEnd = nullptr;
        const double longitude =
            std::strtod(longitudeText.c_str(), &longitudeEnd);
        const double latitude =
            std::strtod(latitudeText.c_str(), &latitudeEnd);
        if (longitudeEnd == longitudeText.c_str()
            || latitudeEnd == latitudeText.c_str()
            || !std::isfinite(latitude)
            || !std::isfinite(longitude)
            || std::abs(latitude) > 90.0
            || std::abs(longitude) > 180.0) {
            continue;
        }
        AddCatalogEntry(
            catalog,
            {code, std::string(country), latitude, longitude});
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog ParseCacsCoordinateJson(const std::string& json) {
    Catalog catalog;
    const auto stations = json.find("\"stations\"");
    const auto arrayStart = stations == std::string::npos
        ? std::string::npos
        : json.find('[', stations);
    if (arrayStart == std::string::npos) {
        return catalog;
    }
    for (const auto& object : ExtractTopLevelJsonObjects(
             std::string_view(json).substr(arrayStart))) {
        std::string id = ToUpperAscii(
            ExtractJsonStringValue(object, "n"));
        double latitude = 0.0;
        double longitude = 0.0;
        if ((id.size() != 4 && id.size() != 9)
            || !ExtractJsonNumberValue(object, "lat", latitude)
            || !ExtractJsonNumberValue(object, "lon", longitude)
            || std::abs(latitude) > 90.0
            || std::abs(longitude) > 180.0) {
            continue;
        }
        AddCatalogEntry(
            catalog,
            {std::move(id), "CAN", latitude, longitude});
    }
    FinishCatalog(catalog);
    return catalog;
}

Catalog PanamaCoordinateCatalog() {
    Catalog catalog;
    AddCatalogEntry(catalog, {"AZUE", "PAN", 7.955688, -80.432515});
    AddCatalogEntry(catalog, {"AZUP", "PAN", 7.780000, -80.280000});
    AddCatalogEntry(catalog, {"BDT1", "PAN", 9.340603, -82.244224});
    AddCatalogEntry(catalog, {"CHEP", "PAN", 9.186000, -79.084000});
    AddCatalogEntry(catalog, {"CHIO", "PAN", 8.447000, -81.771000});
    AddCatalogEntry(catalog, {"CHME", "PAN", 8.580000, -79.890000});
    AddCatalogEntry(catalog, {"DARI", "PAN", 8.658380, -78.154080});
    AddCatalogEntry(catalog, {"DAVI", "PAN", 8.425473, -82.433803});
    AddCatalogEntry(catalog, {"IGN1", "PAN", 8.984886, -79.535755});
    AddCatalogEntry(catalog, {"PMEC", "PAN", 8.980000, -79.540000});
    AddCatalogEntry(catalog, {"PUAR", "PAN", 8.265000, -82.867000});
    AddCatalogEntry(catalog, {"SAN3", "PAN", 8.091000, -80.945000});
    AddCatalogEntry(catalog, {"SON9", "PAN", 8.000000, -81.320000});
    FinishCatalog(catalog);
    return catalog;
}

Catalog HondurasCoordinateCatalog() {
    Catalog catalog;
    AddCatalogEntry(catalog, {"SPS1", "HND", 15.500, -88.030});
    AddCatalogEntry(catalog, {"CEIB", "HND", 15.770, -86.790});
    AddCatalogEntry(catalog, {"JUT1", "HND", 14.670, -86.220});
    AddCatalogEntry(catalog, {"COMA", "HND", 14.450, -87.640});
    AddCatalogEntry(catalog, {"SIGU", "HND", 14.600, -87.830});
    AddCatalogEntry(catalog, {"PUC1", "HND", 15.840, -87.940});
    AddCatalogEntry(catalog, {"TEG2", "HND", 14.090, -87.200});
    FinishCatalog(catalog);
    return catalog;
}

Catalog ParseStationCoordinateJson(
    const std::string& json,
    const std::string_view arrayField) {
    Catalog catalog;
    const std::string marker =
        "\"" + std::string(arrayField) + "\"";
    const auto fieldPosition = json.find(marker);
    const auto arrayPosition = fieldPosition == std::string::npos
        ? std::string::npos
        : json.find('[', fieldPosition + marker.size());
    if (arrayPosition == std::string::npos) {
        return catalog;
    }
    for (const auto& object : ExtractTopLevelJsonObjects(
             std::string_view(json).substr(arrayPosition))) {
        std::string id = ToUpperAscii(
            ExtractJsonStringValue(object, "id"));
        if (id.empty()) {
            id = ToUpperAscii(
                ExtractJsonStringValue(object, "code"));
        }
        double latitude = 0.0;
        double longitude = 0.0;
        if ((id.size() != 4 && id.size() != 9)
            || !ExtractJsonNumberValue(object, "lat", latitude)
            || !ExtractJsonNumberValue(object, "lon", longitude)
            || std::abs(latitude) > 90.0
            || std::abs(longitude) > 180.0) {
            continue;
        }
        AddCatalogEntry(
            catalog,
            {
                std::move(id),
                ExtractJsonStringValue(object, "country"),
                latitude,
                longitude
            });
    }
    FinishCatalog(catalog);
    return catalog;
}

std::vector<FileCandidate> ParseDailyObservationFiles(
    const std::string& html,
    const QueryDate& date) {
    const std::regex longName(
        R"(^([A-Z0-9]{9})_([RSU])_([0-9]{4})([0-9]{3})0000_01D_([0-9]{2}[SMH])_([A-Z]O)\.(crx|rnx)(?:\.(?:gz|Z|bz2))?$)",
        std::regex::icase);
    const std::regex shortName(
        // RINEX 2 compact names use an hour/session character after the
        // day-of-year.  Hours 00--09 are digits and 10--23 are encoded as
        // A--X (for example, abmf0790.19d.Z and abmf079A.19d.Z).
        R"(^([A-Z0-9]{4})([0-9]{3})([0-9A-X])\.([0-9]{2})([DO])(?:\.(gz|Z|bz2))?$)",
        std::regex::icase);
    const auto expectedDoy = std::format("{:03}", date.dayOfYear);
    const auto expectedYear = std::to_string(date.year);
    const int twoDigitYear = date.year % 100;

    std::vector<FileCandidate> candidates;
    for (const auto& fileName : ExtractHrefs(html)) {
        if (!IsSafeArchiveFileName(fileName)) {
            continue;
        }
        std::smatch match;
        if (std::regex_match(fileName, match, longName)) {
            if (match[3].str() != expectedYear || match[4].str() != expectedDoy) {
                continue;
            }
            const auto id = ToUpperAscii(match[1].str());
            candidates.push_back(FileCandidate{
                id,
                id.substr(0, 4),
                fileName,
                {},
                RinexFormat::Rinex34,
                ToUpperAscii(match[5].str()),
                static_cast<char>(std::toupper(
                    static_cast<unsigned char>(match[2].str().front())))
            });
        } else if (std::regex_match(fileName, match, shortName)) {
            if (match[2].str() != expectedDoy
                || std::stoi(match[4].str()) != twoDigitYear) {
                continue;
            }
            const auto four = ToUpperAscii(match[1].str());
            candidates.push_back(FileCandidate{
                {},
                four,
                fileName,
                {},
                RinexFormat::Rinex2,
                "30S",
                'R'
            });
        }
    }
    return candidates;
}

std::vector<IgsHighrateStationScan> ParseHighrateObservationFiles(
    const std::string& html,
    const QueryDate& date,
    int& fileCount) {
    static const std::regex rinex3(
        R"(^([A-Z0-9]{9})_([RSU])_([0-9]{4})([0-9]{3})([0-9]{4})_15M_01S_[A-Z]O\.(?:crx|rnx)(?:\.(?:gz|Z|bz2))?$)",
        std::regex::icase);
    static const std::regex rinex2(
        // IGS high-rate RINEX 2 names use mmmmDDDHMM.YYt.Z. H is a
        // hexadecimal UTC hour (0-9,A-X), followed by minute MM.
        R"(^([A-Z0-9]{4})([0-9]{3})([0-9A-X])([0-9]{2})\.([0-9]{2})([DO])(?:\.(?:gz|Z|bz2))?$)",
        std::regex::icase);
    static const std::regex rinex2Compact(
        // Compatibility with a few old mirrors exposing mmmmDDD H.YYt.
        R"(^([A-Z0-9]{4})([0-9]{3})([0-9A-X])\.([0-9]{2})([DO])(?:\.(?:gz|Z|bz2))?$)",
        std::regex::icase);
    // CDDIS moves older high-rate data out of the hourly tree into one
    // station/day tar archive.  The archive name still identifies the
    // station and stream, and contains the same 96 fifteen-minute 01S
    // segments.  Keep this as a day-level source route; the downloader can
    // construct the archive URL from the stored day directory later.
    static const std::regex dailyTar(
        R"(^([A-Z0-9]{9})_([RSU])_([0-9]{4})([0-9]{3})0000_01D_01S_[A-Z]O\.(?:crx|rnx)\.tar(?:\.gz)?$)",
        std::regex::icase);
    const auto expectedDoy = std::format("{:03}", date.dayOfYear);
    const auto expectedYear = std::to_string(date.year);
    const int twoDigitYear = date.year % 100;
    struct Aggregate {
        IgsHighrateStationScan value;
    };
    std::unordered_map<std::string, Aggregate> stations;
    fileCount = 0;
    for (const auto& fileName : ExtractHrefs(html)) {
        if (!IsSafeArchiveFileName(fileName)) continue;
        std::smatch match;
        std::string stationId;
        std::string fourChar;
        int formatMask = 0;
        bool matches = false;
        if (std::regex_match(fileName, match, rinex3)) {
            if (match[3].str() != expectedYear
                || match[4].str() != expectedDoy) continue;
            stationId = ToUpperAscii(match[1].str());
            fourChar = stationId.substr(0, 4);
            formatMask = 2;
            matches = true;
        } else if (std::regex_match(fileName, match, dailyTar)) {
            if (match[3].str() != expectedYear
                || match[4].str() != expectedDoy) continue;
            stationId = ToUpperAscii(match[1].str());
            fourChar = stationId.substr(0, 4);
            formatMask = 2;
            matches = true;
        } else if (std::regex_match(fileName, match, rinex2)) {
            if (match[2].str() != expectedDoy
                || std::stoi(match[5].str()) != twoDigitYear) continue;
            fourChar = ToUpperAscii(match[1].str());
            formatMask = 1;
            matches = true;
        } else if (std::regex_match(fileName, match, rinex2Compact)) {
            if (match[2].str() != expectedDoy
                || std::stoi(match[4].str()) != twoDigitYear) continue;
            fourChar = ToUpperAscii(match[1].str());
            formatMask = 1;
            matches = true;
        }
        if (!matches) continue;
        // A daily tar represents the 96 quarter-hour segments contained in
        // that archive.  This keeps counts meaningful without materialising
        // 96 rows per station/day in the compact high-rate index.
        fileCount += fileName.find(".tar") != std::string::npos ? 96 : 1;
        const std::string key = stationId.empty()
            ? std::string("FOUR:") + fourChar
            : stationId;
        auto& entry = stations[key].value;
        entry.stationId = stationId;
        entry.fourCharId = fourChar;
        entry.rinexFormatMask |= formatMask;
        entry.fileCount += fileName.find(".tar") != std::string::npos ? 96 : 1;
    }
    std::vector<IgsHighrateStationScan> result;
    result.reserve(stations.size());
    for (auto& [unused, value] : stations) {
        result.push_back(std::move(value.value));
    }
    return result;
}

// Resolve physical high-rate files only when a user requests a download.
// Do not infer filenames from yesterday and do not persist projected facts.
std::vector<StationFile> QueryOnlineHighrateFiles(
    const QueryDate& date, const std::span<const StationFile> wanted,
    const std::stop_token stopToken, std::wstring& warning) {
    struct Root { std::string source; std::wstring url; };
    const auto suffix = std::format(L"{}/{}/", date.year, FormatDoy(date.dayOfYear));
    const std::vector<Root> roots{
        {"BKG", L"https://igs.bkg.bund.de/root_ftp/IGS/highrate/" + suffix},
        {"BKG", L"https://igs.bkg.bund.de/root_ftp/IGS/highrate_v3/" + suffix},
        {"GFZ", L"https://isdc-data.gfz.de/gnss/data/highrate/" + suffix},
        {"IGN", L"ftp://igs.ign.fr/pub/igs/data/highrate/" + suffix},
        {"IGN", L"ftp://igs.ign.fr/pub/igs/data/rinex3/highrate/" + suffix},
        {"ESA", L"ftp://gssc.esa.int/gnss/data/highrate/" + suffix}};
    std::vector<StationFile> files;
    std::unordered_map<std::string, std::size_t> known;
    int failed = 0;
    for (const auto& root : roots) {
        std::vector<std::pair<std::wstring, int>> pending{{root.url, 0}};
        std::unordered_set<std::wstring> visited;
        for (std::size_t i = 0; i < pending.size() && !stopToken.stop_requested(); ++i) {
            const auto [url, depth] = pending[i];
            if (!visited.insert(url).second) continue;
            const bool ftp = url.starts_with(L"ftp:");
            const auto response = ftp ? HttpListFtpDirectory(url, stopToken, 32U * 1024U * 1024U)
                                      : HttpGetText(url, stopToken, 32U * 1024U * 1024U);
            if (!response.ok()) { ++failed; continue; }
            const auto names = ftp ? ExtractLineFileNames(response.body) : ExtractHrefs(response.body);
            for (auto name : names) {
                auto directory = name;
                if (directory.ends_with('/')) directory.pop_back();
                const bool hour = directory.size() == 2 && directory[0] >= '0'
                    && directory[0] <= '2' && directory[1] >= '0' && directory[1] <= '9'
                    && std::stoi(directory) < 24;
                if (depth < 2 && (hour || directory == "o" || directory == "u")) {
                    pending.emplace_back(url + Utf8ToWide(directory) + L"/", depth + 1);
                    continue;
                }
                if (!IsSafeArchiveFileName(name) || name.size() < 4) continue;
                const auto prefix = ToUpperAscii(name.substr(0, 4));
                if (std::none_of(wanted.begin(), wanted.end(), [&](const StationFile& item) {
                        return ToUpperAscii(item.fourCharId) == prefix;
                    })) continue;
                int count = 0;
                const auto parsed = ParseHighrateObservationFiles(BuildSyntheticDirectoryHtml({name}), date, count);
                if (parsed.empty()) continue;
                const auto& identity = parsed.front();
                const auto selected = std::find_if(wanted.begin(), wanted.end(), [&](const StationFile& item) {
                    if (!identity.stationId.empty() && item.stationId.size() == 9)
                        return ToUpperAscii(item.stationId) == identity.stationId;
                    return ToUpperAscii(item.fourCharId) == identity.fourCharId;
                });
                if (selected == wanted.end()) continue;
                const auto found = known.find(name);
                const DownloadRoute route{root.source, name, url + Utf8ToWide(name)};
                if (found != known.end()) {
                    files[found->second].downloadRoutes.push_back(route);
                    continue;
                }
                StationFile file = *selected;
                file.unverifiedDays = 0;
                file.fileName = name;
                file.downloadUrl = route.url;
                file.downloadRoutes = {route};
                file.remoteParts.clear();
                file.format = identity.rinexFormatMask == 1 ? RinexFormat::Rinex2 : RinexFormat::Rinex34;
                file.availableRinexFormats = RinexFormatBit(file.format);
                file.interval = "01S";
                file.preferredSource = root.source;
                known.emplace(name, files.size());
                files.push_back(std::move(file));
            }
        }
    }
    if (failed) warning = std::format(L"高频目录有 {} 次读取失败，已保留其他目录查到的实际文件。", failed);
    if (files.empty()) warning += L" 未查到所选测站的1秒文件。";
    return files;
}

IgsHighrateSourceScan ScanIgsHighrateSource(
    const std::string_view sourceKey,
    const int priority,
    const std::wstring& rootUrl,
    const std::vector<std::wstring>& listingUrls,
    const QueryDate& date,
    const std::stop_token stopToken,
    const bool ftpListing) {
    IgsHighrateSourceScan result;
    result.sourceKey = std::string(sourceKey);
    result.sourcePriority = priority;
    result.listingUrl = rootUrl;
    std::unordered_map<std::string, IgsHighrateStationScan> stations;
    int successfulListings = 0;
    int failedListings = 0;
    int missingListings = 0;
    // Directory endpoints are independent.  Fetch a bounded batch in
    // parallel so one unresponsive mirror cannot serialize 24 hourly
    // requests; the outer date worker still controls date-level concurrency.
    constexpr std::size_t batchSize = 12;
    for (std::size_t first = 0; first < listingUrls.size(); first += batchSize) {
        if (stopToken.stop_requested()) {
            result.error = L"扫描已取消。";
            return result;
        }
        const auto last = std::min(first + batchSize, listingUrls.size());
        std::vector<std::future<HttpTextResult>> responses;
        responses.reserve(last - first);
        for (std::size_t index = first; index < last; ++index) {
            const auto listingUrl = listingUrls[index];
            responses.emplace_back(std::async(
                std::launch::async,
                [listingUrl, stopToken, ftpListing]() {
                    return ftpListing
                        ? HttpListFtpDirectory(
                            listingUrl,
                            stopToken,
                            32U * 1024U * 1024U)
                        : HttpGetText(
                            listingUrl,
                            stopToken,
                            32U * 1024U * 1024U);
                }));
        }
        for (auto& pending : responses) {
        const auto response = pending.get();
        if (!response.ok()) {
            ++failedListings;
            // Only explicit missing-directory responses are coverage misses.
            // Authentication failures and throttling must remain failures.
            const bool missingDirectory =
                response.transportErrorCode == 9
                || response.transportErrorCode == 78
                || response.statusCode == 404
                || response.statusCode == 410;
            if (missingDirectory) {
                ++missingListings;
            }
            switch (response.transportErrorCode) {
                case 5: case 6: case 7: case 28: case 35:
                case 52: case 55: case 56:
                    result.networkFailure = true;
                    break;
                default:
                    break;
            }
            continue;
        }
        // CDDIS redirects protected historical archive paths to the
        // Earthdata login page.  That is an HTTP response, but not a usable
        // directory listing; do not report it as a successful empty source.
        if (response.body.find("Earthdata Login") != std::string::npos
            || response.body.find("earthdata.nasa.gov/login")
                != std::string::npos) {
            continue;
        }
        ++successfulListings;
        int listingFileCount = 0;
        auto listingStations = ParseHighrateObservationFiles(
            ftpListing
                ? BuildSyntheticDirectoryHtml(
                    ExtractLineFileNames(response.body))
                : response.body,
            date,
            listingFileCount);
        result.fileCount += listingFileCount;
        for (auto& station : listingStations) {
            const std::string key = station.stationId.empty()
                ? std::string("FOUR:") + station.fourCharId
                : station.stationId;
            auto& aggregate = stations[key];
            aggregate.stationId = station.stationId;
            aggregate.fourCharId = station.fourCharId;
            aggregate.rinexFormatMask |= station.rinexFormatMask;
            aggregate.fileCount += station.fileCount;
        }
        }
    }
    result.success = successfulListings > 0;
    if (!result.success) {
        if (failedListings > 0 && missingListings == failedListings) {
            result.success = true;
            result.outsideCoverage = true;
            result.error = L"OUTSIDE_COVERAGE";
        } else {
            result.error = L"高频目录均无法读取。";
        }
        return result;
    }
    result.stations.reserve(stations.size());
    for (auto& [unused, station] : stations) {
        result.stations.push_back(std::move(station));
    }
    return result;
}

int CandidateScore(const FileCandidate& candidate) {
    int score = candidate.format == RinexFormat::Rinex34 ? 100 : 10;
    if (candidate.interval == "30S") {
        score += 20;
    } else if (candidate.interval == "15S") {
        score += 15;
    } else if (candidate.interval == "01S") {
        score += 10;
    }
    if (candidate.source == 'R') {
        score += 6;
    } else if (candidate.source == 'S') {
        score += 3;
    }
    if (candidate.fileName.ends_with(".crx.gz")
        || candidate.fileName.ends_with(".d.gz")) {
        score += 2;
    }
    return score;
}

void ApplyCoordinates(StationFile& station, const Catalog& catalog) {
    const CatalogEntry* entry = nullptr;
    const auto exact = catalog.byId.find(station.stationId);
    if (exact != catalog.byId.end()) {
        entry = &exact->second;
    } else {
        const auto group = catalog.byFourChar.find(station.fourCharId);
        if (group != catalog.byFourChar.end() && group->second.size() == 1) {
            const auto found = catalog.byId.find(group->second.front());
            if (found != catalog.byId.end()) {
                entry = &found->second;
            }
        }
    }
    if (entry == nullptr) {
        return;
    }
    station.country = entry->country;
    station.latitude = entry->latitude;
    station.longitude = entry->longitude;
    station.hasCoordinate = true;
    if (station.stationId.size() != 9 && entry->id.size() == 9) {
        station.stationId = entry->id;
    }
}

double DistanceKm(
    const double latitude1,
    const double longitude1,
    const double latitude2,
    const double longitude2) {
    constexpr double pi = 3.14159265358979323846;
    constexpr double radius = 6371.0088;
    const auto radians = [pi](const double degrees) {
        return degrees * pi / 180.0;
    };
    const double phi1 = radians(latitude1);
    const double phi2 = radians(latitude2);
    const double deltaPhi = radians(latitude2 - latitude1);
    const double deltaLambda = radians(longitude2 - longitude1);
    const double a = std::sin(deltaPhi / 2.0) * std::sin(deltaPhi / 2.0)
        + std::cos(phi1) * std::cos(phi2)
            * std::sin(deltaLambda / 2.0) * std::sin(deltaLambda / 2.0);
    return radius * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

void CanonicalizeWithIgs(StationFile& station, const Catalog& igsCatalog) {
    if (!station.hasCoordinate || station.fourCharId.size() != 4) {
        return;
    }
    const auto group = igsCatalog.byFourChar.find(station.fourCharId);
    if (group == igsCatalog.byFourChar.end()) {
        return;
    }
    const CatalogEntry* closest = nullptr;
    double closestDistance = 3.0;
    for (const auto& id : group->second) {
        const auto found = igsCatalog.byId.find(id);
        if (found == igsCatalog.byId.end()) {
            continue;
        }
        const double distance = DistanceKm(
            station.latitude,
            station.longitude,
            found->second.latitude,
            found->second.longitude);
        if (distance <= closestDistance) {
            closestDistance = distance;
            closest = &found->second;
        }
    }
    if (closest != nullptr) {
        station.stationId = closest->id;
    }
}

std::wstring BuildDirectoryUrl(
    const std::wstring_view root,
    const QueryDate& date) {
    return std::format(
        L"{}{}/{}/",
        root,
        date.year,
        FormatDoy(date.dayOfYear));
}

StationFile ResolveCandidate(
    const FileCandidate& candidate,
    const Catalog& primaryCatalog,
    const Catalog& igsCatalog,
    const std::wstring& baseUrl,
    const ObservationNetworkMask memberships,
    const std::unordered_map<std::string, std::vector<std::string>>&
        observedLongIdsByFourChar) {
    StationFile station;
    station.fileName = candidate.fileName;
    station.format = candidate.format;
    station.interval = candidate.interval;
    station.fourCharId = candidate.fourCharId;
    station.stationId = candidate.stationId.empty()
        ? candidate.fourCharId
        : candidate.stationId;
    const auto& relativePath = candidate.downloadPath.empty()
        ? candidate.fileName
        : candidate.downloadPath;
    station.downloadUrl = baseUrl + Utf8ToWide(relativePath);
    station.availableNetworks = memberships;

    if (candidate.stationId.empty()) {
        const auto observed = observedLongIdsByFourChar.find(candidate.fourCharId);
        if (observed != observedLongIdsByFourChar.end()
            && observed->second.size() == 1) {
            station.stationId = observed->second.front();
        }
    }
    ApplyCoordinates(station, primaryCatalog);
    if (!station.hasCoordinate) {
        ApplyCoordinates(station, igsCatalog);
    }
    if (station.country.empty() && station.stationId.size() == 9) {
        station.country = station.stationId.substr(6, 3);
    }
    return station;
}

void AddBestCandidate(
    std::vector<StationFile>& stations,
    std::unordered_map<std::string, std::size_t>& chosenByKey,
    std::unordered_map<std::string, int>& scoreByKey,
    StationFile station,
    const int score) {
    const std::string key = station.stationId.size() == 9
        ? station.stationId
        : std::string("FOUR:") + station.fourCharId;
    const auto found = chosenByKey.find(key);
    if (found == chosenByKey.end()) {
        chosenByKey.emplace(key, stations.size());
        scoreByKey.emplace(key, score);
        stations.push_back(std::move(station));
        return;
    }
    auto& existing = stations[found->second];
    const auto memberships = static_cast<ObservationNetworkMask>(
        existing.availableNetworks | station.availableNetworks);
    if (score > scoreByKey[key]) {
        existing = std::move(station);
        scoreByKey[key] = score;
    }
    existing.availableNetworks = memberships;
}

std::vector<StationFile> SelectIndexedIgsStations(
    const std::vector<IndexedIgsAvailability>& records,
    const NetworkSelection selection) {
    const bool includeAllIgs = HasNetwork(
        selection.mask,
        ObservationNetwork::Igs);
    const bool includeMgex = HasNetwork(
        selection.mask,
        ObservationNetwork::Mgex);
    std::vector<StationFile> stations;
    std::unordered_map<std::string, std::size_t> chosen;
    std::unordered_map<std::string, int> scores;
    for (const auto& record : records) {
        if (!includeAllIgs
            && !(includeMgex
                && HasNetwork(
                    record.station.availableNetworks,
                    ObservationNetwork::Mgex))) {
            continue;
        }
        AddBestCandidate(
            stations,
            chosen,
            scores,
            record.station,
            record.preferenceScore);
    }
    return stations;
}

// Forward declaration for single-source archive adapters defined below.
std::vector<StationFile> ResolveArchiveCandidates(
    const std::vector<FileCandidate>& candidates,
    const Catalog& primaryCatalog,
    const Catalog& igsCatalog,
    const std::wstring& baseUrl,
    ObservationNetwork network,
    std::vector<StationFile>* allFiles);

SourceResult QueryIgsAndMgex(
    const QueryDate& date,
    const NetworkSelection selection,
    const Catalog& igsCatalog,
    const std::stop_token stopToken,
    const KnowledgeBaseQueryMode mode,
    const bool includeAllFiles) {
    SourceResult result;
    const auto indexed = LoadIgsAvailability(date);
    const auto recordMatchesSelection = [&](const IndexedIgsAvailability& record) {
        return HasNetwork(selection.mask, ObservationNetwork::Igs)
            || (HasNetwork(selection.mask, ObservationNetwork::Mgex)
                && HasNetwork(
                    record.station.availableNetworks,
                    ObservationNetwork::Mgex));
    };
    wchar_t forceValue[8]{};
    const bool forceOnline = mode == KnowledgeBaseQueryMode::BuildKnowledgeBaseOnline
        && GetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_FORCE_ONLINE_IGS",
            forceValue,
            static_cast<DWORD>(std::size(forceValue))) > 0
        && forceValue[0] == L'1';
    // The ordinary IGS loader intentionally exposes one preferred row per
    // physical station.  A range query asks for includeAllFiles so that a
    // non-preferred RINEX version/interval can still qualify; load the v4
    // logical-file union lazily and keep the legacy view as a safe fallback
    // for older/partially migrated dates.
    IgsAvailabilityLookup indexedAllFiles;
    const auto& recordsForFiles = [&]() -> const std::vector<IndexedIgsAvailability>& {
        if (!includeAllFiles) {
            return indexed.records;
        }
        indexedAllFiles = LoadIgsAvailabilityAllFiles(date);
        if (!indexedAllFiles.error.empty()
            || (indexedAllFiles.records.empty() && !indexed.records.empty())) {
            return indexed.records;
        }
        return indexedAllFiles.records;
    };
    if (!forceOnline && (indexed.complete
        || (mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            && !indexed.records.empty()))) {
        result.stations = SelectIndexedIgsStations(indexed.records, selection);
        if (includeAllFiles) {
            for (const auto& record : recordsForFiles()) {
                if (!recordMatchesSelection(record)) {
                    continue;
                }
                result.files.push_back(record.station);
            }
        }
        result.fromLocalIndex = true;
        result.fromIgsLocalIndex = true;
        // A complete daily scan may legitimately contain zero files (for
        // example an archive's checked-but-empty historical day).  Preserve
        // that fact so range queries count it as a checked day instead of
        // turning it into a misleading "query failed" result.
        result.emptyResultIsComplete = indexed.complete
            && result.stations.empty();
        if (indexed.provisional) {
            result.warning =
                L"IGS 本地知识库中的该日期是旧版或部分七源记录；"
                L"界面已按本地记录显示，且没有访问网络。可尝试其他已收录的数据源。";
        }
        return result;
    }
    if (mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly) {
        if (!indexed.error.empty()) {
            result.error = L"无法读取 IGS 本地知识库：" + indexed.error;
        } else {
            result.error =
                L"IGS 本地知识库尚未收录该日期。界面查询不会自动联网；"
                L"请调整查询日期范围。";
        }
        return result;
    }
    if (!indexed.error.empty()) {
        result.warning = L"IGS 本地索引暂不可用，已回退在线查询（"
            + indexed.error + L"）";
    }

    enum class ArchiveAccess {
        Html,
        FtpList
    };
    struct ArchiveDefinition {
        std::string key;
        int priority = 0;
        ArchiveAccess access = ArchiveAccess::Html;
        std::wstring listingUrl;
        std::wstring dataUrl;
    };
    const auto doy = FormatDoy(date.dayOfYear);
    const auto twoDigitYear = std::format(L"{:02}", date.year % 100);
    const std::vector<ArchiveDefinition> definitions{
        {
            "CDDIS",
            1,
            ArchiveAccess::FtpList,
            std::format(
                L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/data/daily/{}/{}/{}d/",
                date.year,
                doy,
                twoDigitYear),
            std::format(
                L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/data/daily/{}/{}/{}d/",
                date.year,
                doy,
                twoDigitYear)
        },
        {
            "BKG",
            2,
            ArchiveAccess::Html,
            BuildBkgDailyDirectoryUrl(date),
            BuildBkgDailyDirectoryUrl(date)
        },
        {
            "WHU",
            3,
            ArchiveAccess::FtpList,
            std::format(
                L"ftp://igs.gnsswhu.cn/pub/gps/data/daily/{}/{}/{}d/",
                date.year,
                doy,
                twoDigitYear),
            std::format(
                L"ftp://igs.gnsswhu.cn/pub/gps/data/daily/{}/{}/{}d/",
                date.year,
                doy,
                twoDigitYear)
        },
        {
            "KASI",
            4,
            ArchiveAccess::FtpList,
            std::format(
                L"ftp://nfs.kasi.re.kr/gps/data/daily/{}/{}/{}d/",
                date.year,
                doy,
                twoDigitYear),
            std::format(
                L"ftp://nfs.kasi.re.kr/gps/data/daily/{}/{}/{}d/",
                date.year,
                doy,
                twoDigitYear)
        },
        {
            "IGN",
            5,
            ArchiveAccess::FtpList,
            std::format(
                L"ftp://igs.ign.fr/pub/igs/data/{}/{}/",
                date.year,
                doy),
            std::format(
                L"ftp://igs.ign.fr/pub/igs/data/{}/{}/",
                date.year,
                doy)
        },
        {
            "ESA",
            6,
            ArchiveAccess::FtpList,
            std::format(
                L"ftp://gssc.esa.int/gnss/data/daily/{}/{}/",
                date.year,
                doy),
            std::format(
                L"ftp://gssc.esa.int/gnss/data/daily/{}/{}/",
                date.year,
                doy)
        },
        {
            "SOPAC",
            7,
            ArchiveAccess::Html,
            std::format(
                L"http://garner.ucsd.edu/pub/rinex/{}/{}/",
                date.year,
                doy),
            std::format(
                L"http://garner.ucsd.edu/pub/rinex/{}/{}/",
                date.year,
                doy)
        }
    };

    struct FetchedArchive {
        IgsAvailabilitySourceScan scan;
        std::vector<FileCandidate> candidates;
    };
    std::vector<FetchedArchive> fetched(definitions.size());
    const auto fetchArchive = [&](const std::size_t index) {
        const auto& definition = definitions[index];
        auto& archive = fetched[index];
        archive.scan.sourceKey = definition.key;
        archive.scan.sourcePriority = definition.priority;
        archive.scan.listingUrl = definition.listingUrl;
        archive.scan.dataUrl = definition.dataUrl;
        const auto response = definition.access == ArchiveAccess::FtpList
            ? HttpListFtpDirectory(
                definition.listingUrl,
                stopToken,
                32U * 1024U * 1024U)
            : HttpGetText(
                definition.listingUrl,
                stopToken,
                32U * 1024U * 1024U);
        if (!response.ok()) {
            archive.scan.error = response.error.empty()
                ? L"directory request failed"
                : response.error;
            archive.scan.networkFailure =
                IsNetworkTransportFailure(response);
            return;
        }
        const std::string directoryText =
            definition.access == ArchiveAccess::FtpList
            ? BuildSyntheticDirectoryHtml(ExtractLineFileNames(response.body))
            : response.body;
        archive.candidates = ParseDailyObservationFiles(directoryText, date);
        archive.scan.success = true;
        if (archive.candidates.empty()) {
            archive.scan.error =
                L"The directory was read, but no daily observation "
                L"files matched this date.";
        }
    };
    if (mode == KnowledgeBaseQueryMode::BuildKnowledgeBaseOnline) {
        // Backfills favour archive completeness and server friendliness over
        // interactive latency. Query one archive at a time so jobs=1 means
        // exactly one outbound archive request at any instant.
        for (std::size_t index = 0;
             index < definitions.size() && !stopToken.stop_requested();
             ++index) {
            fetchArchive(index);
        }
    } else {
        std::vector<std::jthread> workers;
        workers.reserve(definitions.size());
        for (std::size_t index = 0; index < definitions.size(); ++index) {
            workers.emplace_back([&, index]() { fetchArchive(index); });
        }
    }
    if (stopToken.stop_requested()) {
        result.error = L"查询已取消。";
        return result;
    }

    std::unordered_map<std::string, std::vector<std::string>> observedLongIds;
    for (const auto& archive : fetched) {
        for (const auto& candidate : archive.candidates) {
            if (candidate.format == RinexFormat::Rinex34
                && candidate.stationId.size() == 9) {
                observedLongIds[candidate.fourCharId].push_back(
                    candidate.stationId);
            }
        }
    }
    for (auto& [fourChar, ids] : observedLongIds) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }

    for (auto& archive : fetched) {
        if (!archive.scan.success) {
            continue;
        }
        std::unordered_map<std::string, IndexedIgsAvailability> best;
        std::unordered_map<std::string, IndexedIgsAvailability> allFiles;
        for (const auto& candidate : archive.candidates) {
            const bool longName = candidate.stationId.size() == 9;
            const bool knownIgs = longName
                ? igsCatalog.byId.contains(candidate.stationId)
                : igsCatalog.byFourChar.contains(candidate.fourCharId);
            if (!knownIgs) {
                continue;
            }
            const bool isMgexFile =
                candidate.format == RinexFormat::Rinex34;
            auto memberships = NetworkBit(ObservationNetwork::Igs);
            if (isMgexFile) {
                memberships = static_cast<ObservationNetworkMask>(
                    memberships | NetworkBit(ObservationNetwork::Mgex));
            }
            StationFile station = ResolveCandidate(
                candidate,
                igsCatalog,
                igsCatalog,
                archive.scan.dataUrl,
                memberships,
                observedLongIds);
            station.downloadRoutes.push_back({archive.scan.sourceKey,
                station.fileName, station.downloadUrl});
            const int score = CandidateScore(candidate);
            const std::string stationKey = station.stationId.size() == 9
                ? station.stationId
                : std::string("FOUR:") + station.fourCharId;
            station.preferredSource = archive.scan.sourceKey;
            station.availabilitySources = {archive.scan.sourceKey};
            IndexedIgsAvailability fileRecord{
                station,
                score,
                archive.scan.sourceKey,
                archive.scan.sourcePriority};
            const std::string fileKey =
                stationKey + "\x1f" + station.fileName;
            allFiles.insert_or_assign(fileKey, fileRecord);
            const auto existing = best.find(stationKey);
            if (existing == best.end()
                || score > existing->second.preferenceScore) {
                best.insert_or_assign(
                    stationKey,
                    std::move(fileRecord));
            }
        }
        archive.scan.records.reserve(best.size());
        for (auto& [stationKey, record] : best) {
            archive.scan.records.push_back(std::move(record));
        }
        archive.scan.files.reserve(allFiles.size());
        for (auto& [fileKey, record] : allFiles) {
            archive.scan.files.push_back(std::move(record));
        }
    }

    std::vector<IgsAvailabilitySourceScan> sourceScans;
    sourceScans.reserve(fetched.size());
    int successfulSources = 0;
    for (auto& archive : fetched) {
        successfulSources += archive.scan.success ? 1 : 0;
        if (!archive.scan.success) {
            result.igsFailedSources.push_back(archive.scan.sourceKey);
            result.igsNetworkFailures +=
                archive.scan.networkFailure ? 1 : 0;
        }
        sourceScans.push_back(std::move(archive.scan));
    }
    result.igsSourcesAttempted = static_cast<int>(definitions.size());
    result.igsSourcesSuccessful = successfulSources;
    if (successfulSources == 0) {
        if (indexed.provisional && !indexed.records.empty()) {
            result.stations =
                SelectIndexedIgsStations(indexed.records, selection);
            if (includeAllFiles) {
                for (const auto& record : recordsForFiles()) {
                    if (!recordMatchesSelection(record)) {
                        continue;
                    }
                    result.files.push_back(record.station);
                }
            }
            result.fromLocalIndex = true;
            result.fromIgsLocalIndex = true;
            result.warning =
                L"IGS 七个数据中心均无法刷新，暂时使用待复扫的本地记录。";
            return result;
        }
        result.error = L"IGS/MGEX：CDDIS、BKG、WHU、KASI、IGN、ESA 和 "
            L"SOPAC 当日目录均读取失败。";
        return result;
    }

    struct Preferred {
        IndexedIgsAvailability record;
        std::vector<std::string> sources;
    };
    std::unordered_map<std::string, Preferred> preferredByStation;
    for (const auto& source : sourceScans) {
        if (!source.success) {
            if (!result.warning.empty()) {
                result.warning += L"；";
            }
            result.warning += Utf8ToWide(source.sourceKey)
                + L" 目录未纳入本次并集（" + source.error + L"）";
            continue;
        }
        for (const auto& record : source.records) {
            const std::string key = record.station.stationId.size() == 9
                ? record.station.stationId
                : std::string("FOUR:") + record.station.fourCharId;
            const auto found = preferredByStation.find(key);
            if (found == preferredByStation.end()) {
                preferredByStation.emplace(
                    key,
                    Preferred{record, {source.sourceKey}});
                continue;
            }
            auto& preferred = found->second;
            if (std::find(
                    preferred.sources.begin(),
                    preferred.sources.end(),
                    source.sourceKey) == preferred.sources.end()) {
                preferred.sources.push_back(source.sourceKey);
            }
            preferred.record.station.availableNetworks =
                static_cast<ObservationNetworkMask>(
                    preferred.record.station.availableNetworks
                    | record.station.availableNetworks);
            if (!preferred.record.station.hasCoordinate
                && record.station.hasCoordinate) {
                preferred.record.station.country = record.station.country;
                preferred.record.station.latitude = record.station.latitude;
                preferred.record.station.longitude = record.station.longitude;
                preferred.record.station.hasCoordinate = true;
            }
            if (record.sourcePriority < preferred.record.sourcePriority
                || (record.sourcePriority
                        == preferred.record.sourcePriority
                    && record.preferenceScore
                        > preferred.record.preferenceScore)) {
                const auto memberships =
                    preferred.record.station.availableNetworks;
                const bool hadCoordinate =
                    preferred.record.station.hasCoordinate;
                const auto country = preferred.record.station.country;
                const double latitude =
                    preferred.record.station.latitude;
                const double longitude =
                    preferred.record.station.longitude;
                preferred.record = record;
                preferred.record.station.availableNetworks = memberships;
                if (hadCoordinate
                    && !preferred.record.station.hasCoordinate) {
                    preferred.record.station.country = country;
                    preferred.record.station.latitude = latitude;
                    preferred.record.station.longitude = longitude;
                    preferred.record.station.hasCoordinate = true;
                }
            }
        }
    }

    const bool includeAllIgs =
        HasNetwork(selection.mask, ObservationNetwork::Igs);
    const bool includeMgex =
        HasNetwork(selection.mask, ObservationNetwork::Mgex);
    for (auto& [stationKey, preferred] : preferredByStation) {
        preferred.record.station.preferredSource =
            preferred.record.sourceKey;
        preferred.record.station.availabilitySources =
            preferred.sources;
        if (includeAllIgs
            || (includeMgex
                && HasNetwork(
                    preferred.record.station.availableNetworks,
                    ObservationNetwork::Mgex))) {
            result.stations.push_back(
                std::move(preferred.record.station));
        }
    }
    if (includeAllFiles) {
        // Keep the raw source union available to callers that request the
        // file-level view.  The normal UI still consumes `stations`, while a
        // date-range query can apply its RINEX/interval predicate before
        // choosing one route for each physical station and day.
        for (const auto& source : sourceScans) {
            if (!source.success) continue;
            for (const auto& record : source.files) {
                if (recordMatchesSelection(record)) {
                    result.files.push_back(record.station);
                }
            }
        }
    }
    if (result.stations.empty()) {
        result.error =
            L"IGS/MGEX：成功读取的数据中心中没有匹配 IGS 当前/历史站表的日观测文件。";
        return result;
    }

    std::wstring storeError;
    if (mode != KnowledgeBaseQueryMode::OnlineDownloadOnly && !StoreIgsAvailability(
            date,
            sourceScans,
            storeError)) {
        if (!result.warning.empty()) {
            result.warning += L"；";
        }
        result.warning += L"IGS 在线查询成功，但写入本地索引失败（"
            + storeError + L"）";
    }
    return result;
}

IgsHighrateBuildResult BuildIgsHighrateAvailabilityImpl(
    const QueryDate& date,
    const std::stop_token stopToken) {
    IgsHighrateBuildResult result;
    // The anonymous mirrors also publish regional/project stations.  Keep
    // this compact high-rate index restricted to the IGS/MGEX station
    // catalogue, just like the normal IGS/MGEX query path.
    static const auto igsCatalog = ParseCatalogJson(
        LoadEmbeddedResource(IDR_IGS_CATALOG));
    const auto doy = FormatDoy(date.dayOfYear);
    const auto twoDigitYear = std::format(L"{:02}", date.year % 100);
    const std::wstring bkgRoot = std::format(
        L"https://igs.bkg.bund.de/root_ftp/IGS/highrate/{}/{}/",
        date.year,
        doy);
    const std::wstring bkgV3Root = std::format(
        L"https://igs.bkg.bund.de/root_ftp/IGS/highrate_v3/{}/{}/",
        date.year,
        doy);
    const auto knownOutsideCoverage = [&](
        const std::string_view sourceKey,
        const int sourcePriority,
        const std::wstring& listingUrl) {
        IgsHighrateSourceScan scan;
        scan.sourceKey = std::string(sourceKey);
        scan.sourcePriority = sourcePriority;
        scan.listingUrl = listingUrl;
        scan.success = true;
        scan.outsideCoverage = true;
        scan.error = L"OUTSIDE_COVERAGE";
        return scan;
    };
    // BKG uses parallel highrate/highrate_v3 trees.  Depending on the
    // historical mirror/date, files are either directly below `o`/`u`
    // branches or below one directory per UTC hour; probe both layouts.
    std::vector<std::wstring> bkgListings;
    bkgListings.reserve(54);
    // Probe the four historical BKG branches first. This keeps the normal
    // single-day check bounded and avoids waiting on dozens of dead hourly
    // directories before we know whether high-rate data exists.
    bkgListings.push_back(std::format(L"{}o/", bkgRoot));
    bkgListings.push_back(std::format(L"{}u/", bkgRoot));
    bkgListings.push_back(std::format(L"{}o/", bkgV3Root));
    bkgListings.push_back(std::format(L"{}u/", bkgV3Root));
    // BKG also publishes consolidated station/day tar archives directly in
    // the date directory (for older days), alongside the o/u branches.
    bkgListings.push_back(bkgRoot);
    bkgListings.push_back(bkgV3Root);

    std::vector<IgsHighrateSourceScan> scans;
    scans.reserve(4);
    auto bkgFuture = std::async(std::launch::async, [&]() {
        if (const auto cached = LoadIgsHighrateSourceCache(date, "BKG")) {
            return *cached;
        }
        // BKG has several long gaps even after the archive root begins in
        // 2020.  Skip dates outside the audited observation windows.
        if (!BkgHighrateMayExist(date)) {
            return knownOutsideCoverage("BKG", 1, bkgRoot);
        }
        auto bkgScan = ScanIgsHighrateSource(
            "BKG",
            1,
            bkgRoot,
            bkgListings,
            date,
            stopToken,
            false);
    // If the HTTPS directory is blocked by a gateway, retry the same four
    // branches through BKG's anonymous FTP endpoint.
    const std::wstring bkgFtpRoot = std::format(
        L"ftp://igs-ftp.bkg.bund.de/IGS/highrate/{}/{}/",
        date.year,
        doy);
    const std::wstring bkgFtpV3Root = std::format(
        L"ftp://igs-ftp.bkg.bund.de/IGS/highrate_v3/{}/{}/",
        date.year,
        doy);
    std::vector<std::wstring> bkgFtpListings{
        std::format(L"{}o/", bkgFtpRoot),
        std::format(L"{}u/", bkgFtpRoot),
        std::format(L"{}o/", bkgFtpV3Root),
        std::format(L"{}u/", bkgFtpV3Root),
        bkgFtpRoot,
        bkgFtpV3Root
    };
    if (!bkgScan.success || bkgScan.fileCount == 0) {
        auto bkgFtpScan = ScanIgsHighrateSource(
            "BKG", 2, bkgFtpRoot, bkgFtpListings, date, stopToken, true);
        if (bkgFtpScan.success
            && (!bkgScan.success
                || bkgFtpScan.fileCount > bkgScan.fileCount)) {
            bkgScan = std::move(bkgFtpScan);
        }
    }
        return bkgScan;
    });

    // Probe the three independent mirrors while BKG is being scanned.  This
    // removes the old per-date source serialization; a slow FTP mirror can
    // no longer hold up the other three sources.
    auto gfzFuture = std::async(std::launch::async, [&]() {
        if (const auto cached = LoadIgsHighrateSourceCache(date, "GFZ")) {
            return *cached;
        }
        // GFZ's year directories are sparse in 2018/2019 (only DOY 121 and
        // 287 respectively); from 2020 through the current archive snapshot
        // the listed windows are continuous.
        if (!GfzHighrateMayExist(date)) {
            const auto root = std::format(
                L"https://isdc-data.gfz.de/gnss/data/highrate/{}/{}/",
                date.year, doy);
            return knownOutsideCoverage("GFZ", 2, root);
        }
        const std::wstring gfzRoot = std::format(
            L"https://isdc-data.gfz.de/gnss/data/highrate/{}/{}/",
            date.year, doy);
        auto gfzScan = ScanIgsHighrateSource(
            "GFZ", 2, gfzRoot, {gfzRoot}, date, stopToken, false);
        if (gfzScan.fileCount == 0 && !gfzScan.outsideCoverage
            && !stopToken.stop_requested()) {
            std::vector<std::wstring> gfzHourly;
            gfzHourly.reserve(24);
            for (int hour = 0; hour < 24; ++hour) {
                gfzHourly.push_back(std::format(
                    L"{}{}o/{:02}/", gfzRoot, twoDigitYear, hour));
            }
            auto gfzHourlyScan = ScanIgsHighrateSource(
                "GFZ", 2, gfzRoot, gfzHourly, date, stopToken, false);
            if (gfzHourlyScan.success
                && (!gfzScan.success
                    || gfzHourlyScan.fileCount > gfzScan.fileCount)) {
                gfzScan = std::move(gfzHourlyScan);
            }
        }
        return gfzScan;
    });
    auto ignFuture = std::async(std::launch::async, [&]() {
        // A positive IGN cache entry is authoritative and avoids another FTP
        // round trip.  Empty/OUTSIDE_COVERAGE entries are deliberately
        // rechecked because the historical RINEX3 tree may contain files
        // that were absent from the consolidated listing at scan time.
        if (const auto cached = LoadIgsHighrateSourceCache(date, "IGN")) {
            if (cached->fileCount > 0 && !cached->outsideCoverage) {
                return *cached;
            }
        }
        const std::wstring ignRoot = std::format(
            L"ftp://igs.ign.fr/pub/igs/data/highrate/{}/{}/",
            date.year, doy);
        auto ignScan = ScanIgsHighrateSource(
            "IGN", 3, ignRoot, {ignRoot}, date, stopToken, true);
        // IGN also exposes a RINEX 3 high-rate tree outside the consolidated
        // data/highrate archive.  Its anonymous mirror uses
        // /pub/igs/data/rinex3/highrate/YYYY/DDD and contains historical MGEX
        // files.  Use it only when the primary tree yielded no files so a day
        // present in both trees is counted once and keeps the primary route
        // preferred.
        if (ignScan.fileCount == 0 && !stopToken.stop_requested()) {
            const std::array<std::wstring, 2> rinex3Roots{
                std::format(
                    L"ftp://igs.ign.fr/pub/igs/data/rinex3/highrate/{}/{}/",
                    date.year, doy),
                std::format(
                    L"ftp://igs.ensg.eu/pub/igs/data/rinex3/highrate/{}/{}/",
                    date.year, doy)
            };
            for (const auto& rinex3Root : rinex3Roots) {
                auto rinex3Scan = ScanIgsHighrateSource(
                    "IGN", 3, rinex3Root, {rinex3Root}, date, stopToken, true);
                if (rinex3Scan.success
                    && (!ignScan.success
                        || rinex3Scan.fileCount > ignScan.fileCount)) {
                    ignScan = std::move(rinex3Scan);
                }
            }
        }
        // IGN's historical MGEX archive used separate RINEX 2 and RINEX 3
        // campaign trees before the consolidated highrate tree was used.
        // Probe those trees only when the main date directory is empty; this
        // preserves the fast path for current IGS high-rate data while
        // recovering older MGEX 1-second files.
        if (ignScan.fileCount == 0 && date.year <= 2015
            && !stopToken.stop_requested()) {
            const std::wstring mgexRinex2 = std::format(
                L"ftp://igs.ign.fr/pub/igs/data/campaign/mgex/highrate/rinex2/{}/{}/",
                date.year, doy);
            const std::wstring mgexRinex3 = std::format(
                L"ftp://igs.ign.fr/pub/igs/data/campaign/mgex/highrate/rinex3/{}/{}/",
                date.year, doy);
            auto mgexRinex2Scan = ScanIgsHighrateSource(
                "IGN", 3, mgexRinex2, {mgexRinex2}, date, stopToken, true);
            auto mgexRinex3Scan = ScanIgsHighrateSource(
                "IGN", 3, mgexRinex3, {mgexRinex3}, date, stopToken, true);
            auto* bestMgex = &mgexRinex2Scan;
            if (mgexRinex3Scan.fileCount > bestMgex->fileCount
                || (!bestMgex->success && mgexRinex3Scan.success)) {
                bestMgex = &mgexRinex3Scan;
            }
            if (bestMgex->success
                && (!ignScan.success
                    || bestMgex->fileCount > ignScan.fileCount)) {
                ignScan = std::move(*bestMgex);
            }
        }
        return ignScan;
    });
    auto esaFuture = std::async(std::launch::async, [&]() {
        if (const auto cached = LoadIgsHighrateSourceCache(date, "ESA")) {
            return *cached;
        }
        const std::wstring esaRoot = std::format(
            L"ftp://gssc.esa.int/gnss/data/highrate/{}/{}/",
            date.year, doy);
        // ESA's day directory is a container listing (00..23), not a file
        // listing.  Reading it first adds one FTP connection to every date
        // and never yields observation files.  Scan all 24 hour directories
        // directly; this keeps complete-day coverage while removing that
        // guaranteed round trip.  The day root is retained as a fallback for
        // older archive layouts that put files directly in the date folder.
        std::vector<std::wstring> esaHourly;
        esaHourly.reserve(24);
        for (int hour = 0; hour < 24; ++hour) {
            esaHourly.push_back(std::format(
                L"{}{:02}/", esaRoot, hour));
        }
        auto esaScan = ScanIgsHighrateSource(
            "ESA", 4, esaRoot, esaHourly, date, stopToken, true);
        if (esaScan.fileCount == 0 && !stopToken.stop_requested()) {
            auto rootScan = ScanIgsHighrateSource(
                "ESA", 4, esaRoot, {esaRoot}, date, stopToken, true);
            if (rootScan.success
                && (!esaScan.success
                    || rootScan.fileCount > esaScan.fileCount)) {
                esaScan = std::move(rootScan);
            }
        }
        return esaScan;
    });
    scans.push_back(bkgFuture.get());
    scans.push_back(gfzFuture.get());
    scans.push_back(ignFuture.get());
    scans.push_back(esaFuture.get());

    for (auto& scan : scans) {
        if (!scan.success) continue;
        std::vector<IgsHighrateStationScan> filtered;
        filtered.reserve(scan.stations.size());
        int filteredFileCount = 0;
        for (const auto& station : scan.stations) {
            const bool known = !station.stationId.empty()
                ? igsCatalog.byId.contains(station.stationId)
                : igsCatalog.byFourChar.contains(station.fourCharId);
            if (!known) continue;
            filteredFileCount += station.fileCount;
            filtered.push_back(station);
        }
        scan.stations = std::move(filtered);
        scan.fileCount = filteredFileCount;
    }
    result.sourcesAttempted = static_cast<int>(scans.size());
    for (const auto& scan : scans) {
        if (!result.sourceSummary.empty()) result.sourceSummary += L"，";
        result.sourceSummary += Utf8ToWide(scan.sourceKey)
            + (scan.outsideCoverage
                ? L"=未覆盖"
                : (scan.success ? L"=" : L"失败="))
            + (scan.outsideCoverage
                ? std::wstring{}
                : std::to_wstring(scan.fileCount));
        if (scan.outsideCoverage) {
            ++result.sourcesOutsideCoverage;
        }
        if (scan.networkFailure) {
            ++result.networkFailures;
        }
        if (!scan.success) continue;
        ++result.sourcesSuccessful;
        result.files += scan.fileCount;
    }
    std::unordered_set<std::string> stationKeys;
    for (const auto& scan : scans) {
        for (const auto& station : scan.stations) {
            stationKeys.insert(station.stationId.empty()
                ? std::string("FOUR:") + station.fourCharId
                : station.stationId);
        }
    }
    result.stations = static_cast<int>(stationKeys.size());
    if (result.sourcesSuccessful == 0) {
        result.error = L"BKG、GFZ、IGN和ESA的匿名IGS高频目录均无法读取。";
        return result;
    }
    // If every source was resolved successfully but none has files, retain a
    // complete empty day marker.  This is different from a transport failure:
    // a source whose archive did not exist for that historical date must not
    // trigger SSR node recovery or make the date loop fail repeatedly.
    if (result.files == 0 || result.stations == 0) {
        const bool allSourcesResolved = std::all_of(
            scans.begin(), scans.end(),
            [](const auto& scan) { return scan.success; });
        if (!allSourcesResolved) {
            result.error = L"高频目录可访问，但没有识别到1S观测文件。";
            return result;
        }
    }
    std::wstring storeError;
    if (!StoreIgsHighrateAvailability(date, scans, storeError)) {
        result.error = L"高频信息读取成功，但写入IGS/MGEX知识库失败："
            + storeError;
        return result;
    }
    result.complete = true;
    return result;
}

SourceResult QueryNgs(
    const QueryDate& date,
    const Catalog& ngsCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    // NOAA publishes the same NCN archive as a public, anonymous S3 bucket.
    // Using the object URL avoids the legacy web server's XML directory
    // response for WinINet clients.
    const std::wstring root =
        L"https://noaa-cors-pds.s3.amazonaws.com/";
    const std::wstring relativeList = std::format(
        L"rinex/{}/{}/{}.{}.files.list",
        date.year,
        FormatDoy(date.dayOfYear),
        date.year,
        FormatDoy(date.dayOfYear));
    const auto response = HttpGetText(
        root + relativeList,
        stopToken,
        8U * 1024U * 1024U);
    std::vector<std::string> relativePaths;
    if (response.ok()) {
        std::istringstream lines(response.body);
        std::string line;
        while (std::getline(lines, line)) {
            const auto separator = line.find_first_of(" \t\r\n");
            if (separator != std::string::npos) {
                relativePaths.push_back(line.substr(0, separator));
            }
        }
    } else {
        // NOAA's convenient YYYY.DDD.files.list objects do not cover every
        // historical year.  The observation objects themselves remain in
        // the public bucket, so fall back to the official S3 ListObjects API.
        const std::string prefix = std::format(
            "rinex/{}/{:03}/",
            date.year,
            date.dayOfYear);
        std::string continuationToken;
        for (int page = 0;
             page < 50 && !stopToken.stop_requested();
             ++page) {
            std::wstring listUrl = root
                + L"?list-type=2&max-keys=1000&prefix="
                + PercentEncodeAscii(prefix);
            if (!continuationToken.empty()) {
                listUrl += L"&continuation-token="
                    + PercentEncodeAscii(continuationToken);
            }
            const auto listing = HttpGetText(
                listUrl,
                stopToken,
                8U * 1024U * 1024U);
            if (!listing.ok()) {
                RecordTransportFailure(result, listing);
                result.error = L"NGS：无法读取 NOAA CORS 当日文件清单或公开 S3 目录"
                    + (listing.error.empty()
                        ? std::wstring{}
                        : L"（" + listing.error + L"）");
                return result;
            }
            for (const auto& key : ExtractXmlValues(listing.body, "Key")) {
                if (key.starts_with(prefix)
                    && key.find("..") == std::string::npos) {
                    relativePaths.push_back(key);
                }
            }
            if (listing.body.find("<IsTruncated>true</IsTruncated>")
                == std::string::npos) {
                break;
            }
            const auto tokens = ExtractXmlValues(
                listing.body,
                "NextContinuationToken");
            if (tokens.empty() || tokens.front().empty()) {
                result.error = L"NGS：公开 S3 分页响应缺少继续令牌。";
                return result;
            }
            continuationToken = tokens.front();
        }
    }

    const auto expectedDoy = std::format("{:03}", date.dayOfYear);
    const auto expectedYear = std::to_string(date.year);
    const auto expectedTwoDigitYear = std::format("{:02}", date.year % 100);
    const std::string expectedPrefix =
        "rinex/" + expectedYear + "/" + expectedDoy + "/";
    const std::string expectedSuffix =
        expectedDoy + "0." + expectedTwoDigitYear + "d.gz";
    std::unordered_map<std::string, bool> seen;
    for (const auto& relativePath : relativePaths) {
        const std::string lowerPath = ToLowerAscii(relativePath);
        if (!lowerPath.starts_with(expectedPrefix)
            || !lowerPath.ends_with(expectedSuffix)) {
            continue;
        }
        const auto lastSlash = lowerPath.find_last_of('/');
        if (lastSlash == std::string::npos
            || lowerPath.size() - lastSlash - 1 < 4) {
            continue;
        }
        const std::string fileName = relativePath.substr(lastSlash + 1);
        const std::string four = ToUpperAscii(fileName.substr(0, 4));
        if (!seen.emplace(four, true).second) {
            continue;
        }
        StationFile station;
        station.stationId = four;
        station.fourCharId = four;
        station.fileName = fileName;
        station.downloadUrl = root + Utf8ToWide(relativePath);
        station.format = RinexFormat::Rinex2;
        station.interval = "30S";
        station.availableNetworks = NetworkBit(ObservationNetwork::Ngs);
        ApplyCoordinates(station, ngsCatalog);
        CanonicalizeWithIgs(station, igsCatalog);
        result.files.push_back(station);
        result.stations.push_back(std::move(station));
    }
    if (result.stations.empty()) {
        result.error = L"NGS：当天文件清单中没有找到 RINEX 日观测文件";
    }
    return result;
}

SourceResult QueryEpn(
    const QueryDate& date,
    const Catalog& epnCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = BuildDirectoryUrl(
        L"https://igs.bkg.bund.de/root_ftp/EUREF/obs/",
        date);
    const auto response = HttpGetText(baseUrl, stopToken, 8U * 1024U * 1024U);
    if (!response.ok()) {
        RecordTransportFailure(result, response);
        result.error = L"EPN：无法读取 BKG EUREF 当日目录"
            + (response.error.empty() ? std::wstring{} : L"（" + response.error + L"）");
        return result;
    }
    const auto candidates = ParseDailyObservationFiles(response.body, date);
    std::unordered_map<std::string, std::vector<std::string>> observedLongIds;
    for (const auto& candidate : candidates) {
        if (candidate.format == RinexFormat::Rinex34
            && candidate.stationId.size() == 9) {
            observedLongIds[candidate.fourCharId].push_back(candidate.stationId);
        }
    }
    for (auto& [fourChar, ids] : observedLongIds) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    std::unordered_map<std::string, std::size_t> chosen;
    std::unordered_map<std::string, int> scores;
    for (const auto& candidate : candidates) {
        auto station = ResolveCandidate(
            candidate,
            epnCatalog,
            igsCatalog,
            baseUrl,
            NetworkBit(ObservationNetwork::Epn),
            observedLongIds);
        result.files.push_back(station);
        AddBestCandidate(
            result.stations,
            chosen,
            scores,
            std::move(station),
            CandidateScore(candidate));
    }
    if (result.stations.empty()) {
        result.error = L"EPN：当天目录中没有找到日观测文件";
    }
    return result;
}

SourceResult QueryGref(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = BuildDirectoryUrl(
        L"https://igs.bkg.bund.de/root_ftp/GREF/obs/",
        date);
    const auto response = HttpGetText(baseUrl, stopToken, 8U * 1024U * 1024U);
    if (!response.ok()) {
        if (response.statusCode == 404) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error = L"GREF：无法读取 BKG GREF 当日目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    const auto candidates = ParseDailyObservationFiles(response.body, date);
    std::unordered_map<std::string, std::vector<std::string>> observedLongIds;
    for (const auto& candidate : candidates) {
        if (candidate.format == RinexFormat::Rinex34
            && candidate.stationId.size() == 9) {
            observedLongIds[candidate.fourCharId].push_back(candidate.stationId);
        }
    }
    for (auto& [fourChar, ids] : observedLongIds) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
    std::unordered_map<std::string, std::size_t> chosen;
    std::unordered_map<std::string, int> scores;
    for (const auto& candidate : candidates) {
        auto station = ResolveCandidate(
            candidate,
            archiveCatalog,
            igsCatalog,
            baseUrl,
            NetworkBit(ObservationNetwork::Gref),
            observedLongIds);
        if (station.country.empty()) station.country = "DEU";
        result.files.push_back(station);
        AddBestCandidate(
            result.stations,
            chosen,
            scores,
            std::move(station),
            CandidateScore(candidate));
    }
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QueryHk(
    const QueryDate& date,
    const Catalog& hkCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = BuildDirectoryUrl(
        L"https://rinex.geodetic.gov.hk/rinex3/",
        date);
    const auto response = HttpGetText(baseUrl, stopToken, 2U * 1024U * 1024U);
    if (!response.ok()) {
        RecordTransportFailure(result, response);
        result.error = L"HK：无法读取香港 SatRef 当日目录"
            + (response.error.empty() ? std::wstring{} : L"（" + response.error + L"）");
        return result;
    }
    std::unordered_map<std::string, bool> seen;
    for (const auto& href : ExtractHrefs(response.body)) {
        if (href.empty() || href.back() != '/') {
            continue;
        }
        const auto previousSlash = href.find_last_of('/', href.size() - 2);
        const std::size_t start = previousSlash == std::string::npos
            ? 0
            : previousSlash + 1;
        const std::string directory =
            href.substr(start, href.size() - start - 1);
        if (directory.size() != 4
            || !std::all_of(
                directory.begin(),
                directory.end(),
                [](const unsigned char character) {
                    return std::isalnum(character) != 0;
                })) {
            continue;
        }
        const std::string four = ToUpperAscii(directory);
        if (!hkCatalog.byId.contains(four)) {
            continue;
        }
        if (!seen.emplace(four, true).second) {
            continue;
        }
        StationFile station;
        station.fourCharId = four;
        station.stationId = four + "00HKG";
        station.fileName = std::format(
            "{}00HKG_R_{}{:03}0000_01D_30S_MO.crx.gz",
            four,
            date.year,
            date.dayOfYear);
        station.downloadUrl = baseUrl
            + Utf8ToWide(ToLowerAscii(four))
            + L"/30s/"
            + Utf8ToWide(station.fileName);
        station.format = RinexFormat::Rinex34;
        station.interval = "30S";
        station.availableNetworks = NetworkBit(ObservationNetwork::Hk);
        ApplyCoordinates(station, hkCatalog);
        CanonicalizeWithIgs(station, igsCatalog);
        if (station.country.empty()) {
            station.country = "HKG";
        }
        result.stations.push_back(std::move(station));
    }
    result.files = result.stations;
    if (result.stations.empty()) {
        result.error = L"HK：当天目录中没有找到测站目录";
    }
    return result;
}

std::vector<StationFile> ResolveArchiveCandidates(
    const std::vector<FileCandidate>& candidates,
    const Catalog& primaryCatalog,
    const Catalog& igsCatalog,
    const std::wstring& baseUrl,
    const ObservationNetworkMask memberships,
    std::vector<StationFile>* allFiles = nullptr) {
    std::unordered_map<std::string, std::vector<std::string>> observedLongIds;
    for (const auto& candidate : candidates) {
        if (candidate.format == RinexFormat::Rinex34
            && candidate.stationId.size() == 9) {
            observedLongIds[candidate.fourCharId].push_back(candidate.stationId);
        }
    }
    for (auto& [fourChar, ids] : observedLongIds) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }

    std::vector<StationFile> stations;
    std::unordered_map<std::string, std::size_t> chosen;
    std::unordered_map<std::string, int> scores;
    for (const auto& candidate : candidates) {
        auto station = ResolveCandidate(
            candidate,
            primaryCatalog,
            igsCatalog,
            baseUrl,
            memberships,
            observedLongIds);
        if (allFiles != nullptr) {
            allFiles->push_back(station);
        }
        AddBestCandidate(
            stations,
            chosen,
            scores,
            std::move(station),
            CandidateScore(candidate));
    }
    return stations;
}

std::vector<StationFile> ResolveArchiveCandidates(
    const std::vector<FileCandidate>& candidates,
    const Catalog& primaryCatalog,
    const Catalog& igsCatalog,
    const std::wstring& baseUrl,
    const ObservationNetwork network,
    std::vector<StationFile>* allFiles = nullptr) {
    return ResolveArchiveCandidates(
        candidates,
        primaryCatalog,
        igsCatalog,
        baseUrl,
        NetworkBit(network),
        allFiles);
}

SourceResult QueryGaGdc(
    const QueryDate& date,
    const Catalog& gaCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::string prefix = std::format(
        "public/daily/{}/{:03}/",
        date.year,
        date.dayOfYear);
    // Official anonymous GA S3 bucket. Use the exact bucket HTTP endpoint
    // because Windows Schannel can fail against this AWS endpoint through
    // some SOCKS nodes. HttpClient keeps the exception strictly host-scoped
    // and still validates every returned file name.
    const std::wstring endpoint =
        L"http://ga-gnss-data-rinex-v1.s3.amazonaws.com/";
    std::vector<std::string> fileNames;
    std::string continuationToken;
    for (int page = 0; page < 20 && !stopToken.stop_requested(); ++page) {
        std::wstring listUrl = endpoint
            + L"?list-type=2&max-keys=1000&prefix="
            + PercentEncodeAscii(prefix);
        if (!continuationToken.empty()) {
            listUrl += L"&continuation-token="
                + PercentEncodeAscii(continuationToken);
        }
        const auto response = HttpGetText(
            listUrl,
            stopToken,
            4U * 1024U * 1024U);
        if (!response.ok()) {
            RecordTransportFailure(result, response);
            result.error =
                L"GA-GDC：无法读取 Geoscience Australia 公开 S3 当日目录"
                + (response.error.empty()
                    ? std::wstring{}
                    : L"（" + response.error + L"）");
            return result;
        }
        for (const auto& key : ExtractXmlValues(response.body, "Key")) {
            if (!key.starts_with(prefix)) {
                continue;
            }
            const auto slash = key.find_last_of('/');
            const std::string fileName = slash == std::string::npos
                ? key
                : key.substr(slash + 1);
            if (IsSafeArchiveFileName(fileName)) {
                fileNames.push_back(fileName);
            }
        }
        if (response.body.find("<IsTruncated>true</IsTruncated>")
            == std::string::npos) {
            break;
        }
        const auto tokens = ExtractXmlValues(
            response.body,
            "NextContinuationToken");
        if (tokens.empty() || tokens.front().empty()) {
            result.error = L"GA-GDC：公开 S3 分页响应缺少继续令牌。";
            return result;
        }
        continuationToken = tokens.front();
    }
    if (stopToken.stop_requested()) {
        return result;
    }
    const auto candidates = ParseDailyObservationFiles(
        BuildSyntheticDirectoryHtml(fileNames),
        date);
    const std::wstring baseUrl = endpoint
        + Utf8ToWide(prefix);
    result.stations = ResolveArchiveCandidates(
        candidates,
        gaCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::GaGdc,
        &result.files);
    if (result.stations.empty()) {
        result.error = L"GA-GDC：当天公开 S3 中没有找到日观测文件。";
    }
    return result;
}

SourceResult QuerySonel(
    const QueryDate& date,
    const Catalog& sonelCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = std::format(
        L"ftp://ftp.sonel.org/gps/data/{}/{}/",
        date.year,
        FormatDoy(date.dayOfYear));
    const std::wstring httpsListingUrl = std::format(
        L"https://www.sonel.org/gps/data/{}/{}/",
        date.year,
        FormatDoy(date.dayOfYear));
    // SONEL's anonymous FTP passive data ports are intermittently
    // unreachable. Its public web host exposes the same Apache directory
    // listing. Prefer that listing, while keeping the canonical FTP URLs for
    // the file records and falling back to FTP if HTTPS is unavailable.
    const auto httpsResponse = HttpGetText(
        httpsListingUrl,
        stopToken,
        8U * 1024U * 1024U);
    if (httpsResponse.ok()) {
        const auto httpsCandidates = ParseDailyObservationFiles(
            httpsResponse.body,
            date);
        if (!httpsCandidates.empty()) {
            result.stations = ResolveArchiveCandidates(
                httpsCandidates,
                sonelCatalog,
                igsCatalog,
                baseUrl,
                ObservationNetwork::Sonel,
                &result.files);
            if (!result.stations.empty()) {
                return result;
            }
        }
    }
    const auto response = HttpListFtpDirectory(
        baseUrl,
        stopToken,
        4U * 1024U * 1024U);
    if (!response.ok()) {
        RecordTransportFailure(result, response);
        result.error = L"SONEL：无法读取匿名 FTP 当日目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    const auto candidates = ParseDailyObservationFiles(
        BuildSyntheticDirectoryHtml(
            ExtractLineFileNames(response.body)),
        date);
    result.stations = ResolveArchiveCandidates(
        candidates,
        sonelCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::Sonel,
        &result.files);
    if (result.stations.empty()) {
        result.error = L"SONEL：匿名 FTP 当日目录中没有找到日观测文件。";
    }
    return result;
}

SourceResult QueryRgna(
    const QueryDate& date,
    const Catalog& rgnaCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    // INEGI replaced the legacy browser workflow with a public SFTP archive.
    // The complete national daily archive starts at 2020 DOY 333
    // (2020-11-28).  The legacy browser endpoint must not be used for older
    // dates: it ignores the requested year and can return current rolling
    // files, which would create false historical availability records.
    const bool hasCompleteDailyArchive = date.year > 2020
        || (date.year == 2020 && date.dayOfYear >= 333);
    if (!hasCompleteDailyArchive) {
        result.error = L"RGNA：INEGI 当前匿名公开源没有 2020-11-28 以前的"
            L"完整全国日观测档案；为避免写入伪历史记录，已停止旧网页回填。";
        return result;
    }

    // Cache one directory session per calendar month so every day does not
    // reopen SSH.
    {
        static std::mutex monthCacheMutex;
        static std::unordered_map<int,
            std::shared_future<HttpTextResult>> monthCache;
        const int monthKey = date.year * 100 + date.month;
        std::shared_future<HttpTextResult> monthListing;
        {
            std::scoped_lock lock(monthCacheMutex);
            const auto found = monthCache.find(monthKey);
            if (found != monthCache.end()) {
                monthListing = found->second;
            } else {
                monthListing = std::async(
                    std::launch::async,
                    [year = date.year, month = date.month, stopToken]() {
                        return HttpListRgnaMexicoSftpMonth(
                            year,
                            month,
                            stopToken,
                            16U * 1024U * 1024U);
                    }).share();
                monthCache.emplace(monthKey, monthListing);
            }
        }
        const HttpTextResult response = monthListing.get();
        if (!response.ok()) {
            // Do not pin a transient SFTP failure in the process-wide cache;
            // proxy recovery and the next retry must be able to open a fresh
            // session for this month.
            {
                std::scoped_lock lock(monthCacheMutex);
                monthCache.erase(monthKey);
            }
            RecordTransportFailure(result, response);
            result.error = L"RGNA：无法读取 INEGI 官方公共 SFTP 月目录"
                + (response.error.empty()
                    ? std::wstring{}
                    : L"（" + response.error + L"）");
            return result;
        }

        static const std::regex dailyPattern(
            R"((?:^|[/\s])(([A-Za-z0-9]{4})([0-9]{3})0\.([0-9]{2})[dD]\.gz)(?:$|\s))",
            std::regex::optimize);
        std::unordered_set<std::string> seenFiles;
        for (std::sregex_iterator it(
                 response.body.begin(), response.body.end(), dailyPattern),
             end;
             it != end;
             ++it) {
            const std::string fileName = (*it)[1].str();
            const std::string four = ToUpperAscii((*it)[2].str());
            if (std::stoi((*it)[3].str()) != date.dayOfYear
                || std::stoi((*it)[4].str()) != date.year % 100
                || !seenFiles.insert(ToUpperAscii(fileName)).second) {
                continue;
            }
            StationFile station;
            station.fourCharId = four;
            station.stationId = four + "00MEX";
            station.country = "MEX";
            station.fileName = fileName;
            station.downloadUrl = std::format(
                L"sftp://rgnasftp@geodesia2.inegi.org.mx/{:04}/{:03}/{}",
                date.year,
                date.dayOfYear,
                Utf8ToWide(fileName));
            station.format = RinexFormat::Rinex2;
            station.interval = "30S";
            station.availableNetworks =
                NetworkBit(ObservationNetwork::Rgna);
            ApplyCoordinates(station, rgnaCatalog);
            CanonicalizeWithIgs(station, igsCatalog);
            result.stations.push_back(std::move(station));
        }
        std::sort(
            result.stations.begin(),
            result.stations.end(),
            [](const StationFile& left, const StationFile& right) {
                return left.stationId < right.stationId;
            });
        result.files = result.stations;
        if (result.stations.empty()) {
            result.emptyResultIsComplete = true;
            result.warning = L"RGNA：官方公共 SFTP 月目录读取成功，"
                L"但当天没有发布日观测文件。";
        }
        return result;
    }
}

SourceResult QueryRbmc(
    const QueryDate& date,
    const Catalog& regionalCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring root =
        L"https://geoftp.ibge.gov.br/"
        L"informacoes_sobre_posicionamento_geodesico/rbmc/";
    const std::wstring rinex3Directory = std::format(
        L"{}dados_RINEX3/{}/{:03}/",
        root,
        date.year,
        date.dayOfYear);
    const std::wstring rinex2Directory = std::format(
        L"{}dados/{}/{:03}/",
        root,
        date.year,
        date.dayOfYear);

    std::unordered_set<std::string> chosen;
    std::wstring errors;
    int attemptedDirectories = 0;
    int failedDirectories = 0;
    int successfulDirectories = 0;
    long transportErrorCode = 0;
    const auto appendError = [&](const std::wstring& value) {
        if (value.empty()) {
            return;
        }
        if (!errors.empty()) {
            errors += L"; ";
        }
        errors += value;
    };

    // RINEX 3 is the preferred RBMC product and is available from 2018.
    if (date.year >= 2018 && !stopToken.stop_requested()) {
        ++attemptedDirectories;
        const auto response = HttpGetText(
            rinex3Directory,
            stopToken,
            8U * 1024U * 1024U);
        if (response.ok()) {
            ++successfulDirectories;
            auto stations = ResolveArchiveCandidates(
                ParseDailyObservationFiles(response.body, date),
                regionalCatalog,
                igsCatalog,
                rinex3Directory,
                ObservationNetwork::Rbmc,
                &result.files);
            for (auto& station : stations) {
                if (station.country.empty()) {
                    station.country = "BRA";
                }
                chosen.insert(station.fourCharId);
                result.stations.push_back(std::move(station));
            }
        } else {
            ++failedDirectories;
            if (transportErrorCode == 0 && IsNetworkTransportFailure(response)) {
                transportErrorCode = response.transportErrorCode;
            }
            appendError(
                L"IBGE RBMC RINEX 3 directory unavailable"
                + (response.error.empty()
                    ? std::wstring{}
                    : L": " + response.error));
        }
    }

    // The long-running RBMC archive publishes one ZIP per station/day.  It
    // provides the historical fallback before RINEX 3 and fills stations that
    // did not publish a multi-GNSS file on a newer day.
    if (!stopToken.stop_requested()) {
        ++attemptedDirectories;
        const auto response = HttpGetText(
            rinex2Directory,
            stopToken,
            8U * 1024U * 1024U);
        if (response.ok()) {
            ++successfulDirectories;
            const std::regex zipName(
                R"(^([A-Z0-9]{4})([0-9]{3})[0-9]?\.zip$)",
                std::regex::icase);
            const std::string expectedDoy =
                std::format("{:03}", date.dayOfYear);
            for (const auto& fileName : ExtractHrefs(response.body)) {
                std::smatch match;
                if (!IsSafeArchiveFileName(fileName)
                    || !std::regex_match(fileName, match, zipName)
                    || match[2].str() != expectedDoy) {
                    continue;
                }
                const std::string code =
                    ToUpperAscii(match[1].str());
                StationFile station;
                station.stationId = code;
                station.fourCharId = code;
                station.country = "BRA";
                station.fileName = fileName;
                station.downloadUrl =
                    rinex2Directory + Utf8ToWide(fileName);
                station.format = RinexFormat::Rinex2;
                station.interval = "15S";
                station.availableNetworks =
                    NetworkBit(ObservationNetwork::Rbmc);
                ApplyCoordinates(station, regionalCatalog);
                if (!station.hasCoordinate) {
                    ApplyCoordinates(station, igsCatalog);
                }
                CanonicalizeWithIgs(station, igsCatalog);
                result.files.push_back(station);
                if (chosen.contains(code)) {
                    continue;
                }
                chosen.insert(code);
                result.stations.push_back(std::move(station));
            }
        } else {
            ++failedDirectories;
            if (transportErrorCode == 0 && IsNetworkTransportFailure(response)) {
                transportErrorCode = response.transportErrorCode;
            }
            appendError(
                L"IBGE RBMC historical directory unavailable"
                + (response.error.empty()
                    ? std::wstring{}
                    : L": " + response.error));
        }
    }

    if (result.stations.empty()) {
        if (failedDirectories == attemptedDirectories
            && transportErrorCode != 0) {
            result.networkTransportFailure = true;
            result.transportErrorCode = transportErrorCode;
        }
        if (successfulDirectories > 0 && failedDirectories == 0) {
            result.emptyResultIsComplete = true;
            result.warning =
                L"IBGE RBMC: the official archive was checked successfully, "
                L"but no daily files were published for this date.";
        } else {
            result.error = errors.empty()
                ? L"IBGE RBMC: no daily files were found for this day."
                : L"IBGE RBMC: " + errors;
        }
    } else if (!errors.empty()) {
        result.error = L"IBGE RBMC partial result: " + errors;
    }
    for (auto& file : result.files) {
        if (file.country.empty()) {
            file.country = "BRA";
        }
    }
    return result;
}

SourceResult QueryRamsac(
    const QueryDate& date,
    const Catalog& regionalCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    // RAMSAC accepts a date range of at most 31 days between the endpoints
    // (32 inclusive calendar days).  Cache one such block so the generic
    // day-by-day builder performs one request per station per block instead
    // of one request per station per day.
    struct BatchCache {
        std::mutex mutex;
        std::unordered_map<std::string, SourceResult> days;
    };
    static BatchCache cache;
    const auto dateKey = [](const QueryDate& value) {
        return std::format(
            "{:04}-{:02}-{:02}", value.year, value.month, value.day);
    };
    const std::string requestedKey = dateKey(date);
    {
        std::scoped_lock lock(cache.mutex);
        const auto found = cache.days.find(requestedKey);
        if (found != cache.days.end()) {
            return found->second;
        }
    }

    const auto requestedDay = std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
    constexpr int batchDayCount = 32;
    const auto firstDay = requestedDay - std::chrono::days{batchDayCount - 1};
    std::vector<QueryDate> batchDates;
    std::unordered_map<std::string, std::size_t> batchIndex;
    batchDates.reserve(batchDayCount);
    batchIndex.reserve(batchDayCount);
    for (int offset = 0; offset < batchDayCount; ++offset) {
        const std::chrono::year_month_day calendar{
            firstDay + std::chrono::days{offset}};
        QueryDate batchDate = MakeQueryDate(
            static_cast<int>(calendar.year()),
            static_cast<unsigned>(calendar.month()),
            static_cast<unsigned>(calendar.day()));
        batchIndex.emplace(dateKey(batchDate), batchDates.size());
        batchDates.push_back(batchDate);
    }
    std::vector<SourceResult> batchResults(batchDates.size());

    const std::wstring apiRoot =
        L"https://ramsac.ign.gob.ar/api/v1/";
    const auto coordinateResponse = HttpGetText(
        L"https://wms.ign.gob.ar/geoserver/geodesia-demarcacion/ows?"
        L"service=WFS&version=1.0.0&request=GetFeature"
        L"&typeName=ramsac&outputFormat=application%2Fjson",
        stopToken,
        8U * 1024U * 1024U);
    const Catalog officialCoordinateCatalog = coordinateResponse.ok()
        ? ParseRamsacGeoJsonCoordinates(coordinateResponse.body)
        : Catalog{};
    // Source-owned Site Logs identify a regional station more reliably
    // than a global four-character catalogue when that code is reused.
    Catalog nativeCoordinateCatalog;
#ifdef GNSS_DOWNLOADER_DUCKDB_BACKEND
    std::unordered_map<std::string, SiteLogRecord> nativeLogs;
    std::wstring nativeLogError;
    if (LoadSiteLogSummaryMap(
            GetNetworkAvailabilityDatabasePath(ObservationNetwork::Ramsac),
            "", nativeLogs, nativeLogError,
            NetworkBit(ObservationNetwork::Ramsac))) {
        for (const auto& [key, record] : nativeLogs) {
            if (key != record.stationId || !record.hasCoordinate
                || !std::isfinite(record.latitude) || !std::isfinite(record.longitude)
                || std::abs(record.latitude) > 90.0 || std::abs(record.longitude) > 180.0) continue;
            AddCatalogEntry(nativeCoordinateCatalog,
                {record.stationId, record.stationId.size() == 9
                    ? record.stationId.substr(6) : record.country,
                 record.latitude, record.longitude});
        }
        FinishCatalog(nativeCoordinateCatalog);
    }
#endif
    const auto directory = HttpGetText(
        apiRoot + L"estaciones",
        stopToken,
        2U * 1024U * 1024U);
    if (!directory.ok()) {
        SourceResult result;
        RecordTransportFailure(result, directory);
        result.error = L"RAMSAC: unable to read the official station API"
            + (directory.error.empty()
                ? std::wstring{}
                : L": " + directory.error);
        return result;
    }

    std::vector<std::string> codes;
    std::unordered_set<std::string> seenCodes;
    for (auto code :
         ExtractJsonStringValues(directory.body, "cod_estacion")) {
        code = ToUpperAscii(std::move(code));
        if (code.size() == 4
            && seenCodes.insert(code).second) {
            codes.push_back(std::move(code));
        }
    }
    if (codes.empty()) {
        SourceResult result;
        result.error =
            L"RAMSAC: the official API returned no stations.";
        return result;
    }

    const std::string formBody =
        "intervalo=15&fecha_desde=" + dateKey(batchDates.front())
        + "&fecha_hasta=" + dateKey(batchDates.back());
    std::atomic_size_t nextIndex{0};
    std::atomic_int failedRequests{0};
    std::atomic_long transportErrorCode{0};
    std::mutex resultMutex;
    const auto worker = [&]() {
        while (!stopToken.stop_requested()) {
            const std::size_t index = nextIndex.fetch_add(1);
            if (index >= codes.size()) {
                return;
            }
            const auto& code = codes[index];
            const std::wstring wideCode(code.begin(), code.end());
            const auto response = HttpPostFormText(
                apiRoot + L"rinex/" + wideCode,
                formBody,
                stopToken,
                512U * 1024U);
            if (!response.ok()) {
                failedRequests.fetch_add(1);
                if (IsNetworkTransportFailure(response)) {
                    long expected = 0;
                    transportErrorCode.compare_exchange_strong(
                        expected, response.transportErrorCode);
                }
                continue;
            }
            for (const auto& object :
                 ExtractTopLevelJsonObjects(response.body)) {
                const std::string fileName =
                    ExtractJsonStringValue(object, "archivo");
                const std::string observationStart =
                    ExtractJsonStringValue(object, "inicio");
                if (observationStart.size() < 10
                    || !IsSafeArchiveFileName(fileName)) {
                    continue;
                }
                const auto day = batchIndex.find(
                    observationStart.substr(0, 10));
                if (day == batchIndex.end()) {
                    continue;
                }
                const QueryDate& fileDate = batchDates[day->second];
                const std::string expected =
                    ToLowerAscii(code)
                    + std::format(
                        "{:03}0.{:02}d.z",
                        fileDate.dayOfYear,
                        fileDate.year % 100);
                if (ToLowerAscii(fileName) != expected) {
                    continue;
                }
                StationFile station;
                station.stationId = code;
                station.fourCharId = code;
                station.fileName = fileName;
                station.downloadUrl =
                    apiRoot + L"rinex/download/15/" + Utf8ToWide(fileName);
                station.format = RinexFormat::Rinex2;
                station.interval = "15S";
                station.availableNetworks =
                    NetworkBit(ObservationNetwork::Ramsac);
                ApplyCoordinates(station, officialCoordinateCatalog);
                if (!station.hasCoordinate) {
                    ApplyCoordinates(station, nativeCoordinateCatalog);
                }
                const auto nativePrefix = nativeCoordinateCatalog.byFourChar.find(code);
                const bool ambiguousNative = nativePrefix != nativeCoordinateCatalog.byFourChar.end()
                    && nativePrefix->second.size() > 1;
                if (!station.hasCoordinate && !ambiguousNative) {
                    ApplyCoordinates(station, igsCatalog);
                }
                if (!station.hasCoordinate && !ambiguousNative) {
                    ApplyCoordinates(station, regionalCatalog);
                }
                CanonicalizeWithIgs(station, igsCatalog);
                if (station.country.empty()) {
                    station.country = "ARG";
                }
                std::scoped_lock lock(resultMutex);
                batchResults[day->second].stations.push_back(
                    std::move(station));
            }
        }
    };
    {
        std::vector<std::jthread> workers;
        const std::size_t workerCount =
            std::min<std::size_t>(8, codes.size());
        workers.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers.emplace_back(worker);
        }
    }
    const bool allRequestsFailed =
        failedRequests.load() == static_cast<int>(codes.size());
    for (auto& dayResult : batchResults) {
        dayResult.files = dayResult.stations;
        if (dayResult.stations.empty()
            && allRequestsFailed
            && transportErrorCode.load() != 0) {
            dayResult.networkTransportFailure = true;
            dayResult.transportErrorCode = transportErrorCode.load();
        }
        if (dayResult.stations.empty() && !stopToken.stop_requested()) {
            if (allRequestsFailed || failedRequests.load() > 0) {
                dayResult.error = allRequestsFailed
                    ? L"RAMSAC: all official file API requests failed."
                    : L"RAMSAC: some station requests failed and no complete "
                      L"daily result can be stored.";
            } else {
                dayResult.emptyResultIsComplete = true;
                dayResult.warning = L"RAMSAC: the official API was checked "
                    L"successfully, but no files were published for this day.";
            }
        } else if (failedRequests.load() > 0) {
            dayResult.error = std::format(
                L"RAMSAC partial result: {} station API requests failed.",
                failedRequests.load());
        } else if (!coordinateResponse.ok()) {
            dayResult.error =
                L"RAMSAC: files are complete, but the official coordinate "
                L"catalog was temporarily unavailable; some map markers may "
                L"be missing.";
        }
    }

    SourceResult requestedResult = batchResults.back();
    if (!stopToken.stop_requested() && !allRequestsFailed) {
        std::scoped_lock lock(cache.mutex);
        cache.days.clear();
        cache.days.reserve(batchResults.size());
        for (std::size_t index = 0; index < batchResults.size(); ++index) {
            cache.days.emplace(
                dateKey(batchDates[index]),
                std::move(batchResults[index]));
        }
        requestedResult = cache.days.at(requestedKey);
    }
    return requestedResult;
}

SourceResult QueryRegnaRou(
    const QueryDate& date,
    const Catalog& regionalCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const auto today = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(
            std::chrono::system_clock::now())};
    const int currentYear = static_cast<int>(today.year());

    // IGM explicitly separates the current year (anonymous FTP) from the
    // historical archive (public-credential SFTP).  Prefer the current FTP
    // only for this UTC year; probing it for every historical day caused both
    // long timeouts and false "no data" results in the old adapter.
    std::wstring currentFtpError;
    if (date.year == currentYear) {
        const std::wstring baseUrl = std::format(
            L"ftp://pp.igm.gub.uy/hatanaka/{}/{:03}/",
            date.year,
            date.dayOfYear);
        const auto response = HttpListFtpDirectory(
            baseUrl,
            stopToken,
            4U * 1024U * 1024U);
        if (response.ok()) {
            result.stations = ResolveArchiveCandidates(
                ParseDailyObservationFiles(
                    BuildSyntheticDirectoryHtml(
                        ExtractLineFileNames(response.body)),
                    date),
                regionalCatalog,
                igsCatalog,
                baseUrl,
                ObservationNetwork::RegnaRou,
                &result.files);
            if (!result.stations.empty()) {
                return result;
            }
            result.emptyResultIsComplete = true;
            result.warning =
                L"REGNA-ROU：官方当年匿名 FTP 已查询，但当天没有发布日观测文件。";
            return result;
        }
        currentFtpError = response.error;
        // Around the annual rollover IGM may already have moved a day to the
        // historical server.  Fall through to the SFTP archive before
        // reporting the current FTP failure.
    }

    struct HistoricalMonthCache {
        std::mutex mutex;
        std::unordered_map<int, HttpTextResult> months;
    };
    static HistoricalMonthCache cache;
    const int monthKey = date.year * 100 + date.month;
    HttpTextResult response;
    {
        std::scoped_lock cacheLock(cache.mutex);
        const auto cached = cache.months.find(monthKey);
        if (cached != cache.months.end()) {
            response = cached->second;
        } else {
            response = HttpListRegnaRouSftpMonth(
                date.year,
                date.month,
                stopToken,
                32U * 1024U * 1024U);
            if (response.ok()) {
                cache.months.emplace(monthKey, response);
            }
        }
    }
    if (!response.ok()) {
        result.error =
            L"REGNA-ROU：无法读取 IGM 官方历史 SFTP 月目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        if (!currentFtpError.empty()) {
            result.error += L"；当年匿名 FTP 也不可用（"
                + currentFtpError + L"）";
        }
        return result;
    }

    const std::regex pathPattern(
        R"((sftpserver/([0-9]{4})/([0-9]{2})/([0-9]{2})/([A-Za-z][A-Za-z0-9]{3}))(?:[\r\n\s]|$))",
        std::regex::icase);
    std::unordered_set<std::string> seen;
    for (std::sregex_iterator iterator(
             response.body.begin(), response.body.end(), pathPattern),
         end;
         iterator != end;
         ++iterator) {
        const int year = std::stoi((*iterator)[2].str());
        const int month = std::stoi((*iterator)[3].str());
        const int day = std::stoi((*iterator)[4].str());
        if (year != date.year || month != date.month || day != date.day) {
            continue;
        }
        const std::string four = ToUpperAscii((*iterator)[5].str());
        const std::string fileName = ToLowerAscii(four)
            + std::format("{:03}0.rnx.zip", date.dayOfYear);
        if (!seen.insert(four).second) {
            continue;
        }
        StationFile station;
        station.stationId = four + "00URY";
        station.fourCharId = four;
        station.country = "URY";
        station.fileName = fileName;
        station.downloadUrl =
            L"sftp://regna@sftp.igm.gub.uy:2222/"
            + Utf8ToWide((*iterator)[1].str())
            + L"/" + Utf8ToWide(fileName);
        station.format = RinexFormat::Rinex34;
        station.interval = "NATIVE";
        station.availableNetworks =
            NetworkBit(ObservationNetwork::RegnaRou);
        ApplyCoordinates(station, regionalCatalog);
        CanonicalizeWithIgs(station, igsCatalog);
        result.stations.push_back(std::move(station));
    }
    result.files = result.stations;
    if (result.stations.empty()) {
        result.emptyResultIsComplete = true;
        result.warning =
            L"REGNA-ROU：官方历史 SFTP 月目录已查询，但当天没有发布日观测文件。";
    }
    return result;
}

SourceResult QueryRgp(
    const QueryDate& date,
    const Catalog& fallbackCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    // The main RGP HTTPS host can reject automated directory requests from
    // some regions.  ENSG is the official anonymous FTP mirror published by
    // IGN France and exposes the same daily RINEX holdings.
    const std::wstring dayRoot = std::format(
        L"ftp://rgpdata.ensg.eu/pub/data/{}/{:03}/",
        date.year,
        date.dayOfYear);
    const std::wstring thirtySecondUrl = dayRoot + L"data_30/";
    const auto thirtySecondResponse = HttpListFtpDirectory(
        thirtySecondUrl,
        stopToken,
        8U * 1024U * 1024U);
    RecordTransportFailure(result, thirtySecondResponse);
    if (!thirtySecondResponse.ok()) {
        result.error =
            L"RGP France: unable to read the official anonymous FTP data_30 directory: "
            + thirtySecondResponse.error;
        return result;
    }
    // The coordinate table is independent of the observation date. Cache a
    // successful response for the lifetime of the process so a historical
    // backfill does not download the same CSV thousands of times.
    static std::mutex coordinateMutex;
    static Catalog cachedCoordinateCatalog;
    Catalog officialCoordinateCatalog;
    bool coordinateAvailable = false;
    {
        std::scoped_lock coordinateLock(coordinateMutex);
        if (cachedCoordinateCatalog.byId.empty()) {
            const auto coordinateResponse = HttpGetText(
                L"https://rgp.ign.fr/STATIONS/coordRGP.php?t=csv",
                stopToken,
                4U * 1024U * 1024U);
            if (coordinateResponse.ok()) {
                cachedCoordinateCatalog =
                    ParseRgpCoordinateCsv(coordinateResponse.body);
            }
        }
        officialCoordinateCatalog = cachedCoordinateCatalog;
        coordinateAvailable = !officialCoordinateCatalog.byId.empty();
    }
    const Catalog rgpCatalog = MergeCatalogs(
        {&fallbackCatalog, &officialCoordinateCatalog});
    auto dailyCandidates = ParseDailyObservationFiles(
        BuildSyntheticDirectoryHtml(
            ExtractLineFileNames(thirtySecondResponse.body)),
        date);
    // RGP data_30 contains both one full-day RINEX 2 file (session 0)
    // and up to 24 hourly RINEX 2 fragments (sessions a-x) for the same
    // station.  The regular availability database represents station-days,
    // so retain only the full-day file. RINEX 3/4 candidates parsed above
    // already require the 01D duration token.
    std::erase_if(
        dailyCandidates,
        [](const FileCandidate& candidate) {
            return candidate.format == RinexFormat::Rinex2
                && (candidate.fileName.size() <= 7
                    || candidate.fileName[7] != '0');
        });
    result.stations = ResolveArchiveCandidates(
        dailyCandidates,
        rgpCatalog,
        igsCatalog,
        thirtySecondUrl,
        ObservationNetwork::Rgp,
        &result.files);
    if (result.stations.empty()) {
        result.error =
            L"RGP France: no full-day data_30 files were found for this day.";
    } else if (!coordinateAvailable) {
        result.warning =
            L"RGP France: files are complete, but the official coordinate "
            L"CSV was temporarily unavailable; some map markers may be "
            L"missing.";
    }
    return result;
}

void CompleteNationalArchiveStations(
    SourceResult& result,
    const std::string_view country,
    const Catalog& fallbackCatalog,
    const Catalog& igsCatalog) {
    const auto finish = [&](StationFile& station) {
        if (!station.hasCoordinate) ApplyCoordinates(station, fallbackCatalog);
        if (station.country.empty()) station.country = std::string(country);
        CanonicalizeWithIgs(station, igsCatalog);
    };
    for (auto& station : result.stations) finish(station);
    for (auto& file : result.files) finish(file);
}

SourceResult QueryRing(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = date.year >= 2024
        ? std::format(
            L"https://gnss.isprambiente.it/post2024/rinex/{}/{:03}/",
            date.year, date.dayOfYear)
        : std::format(
            L"https://gnss.isprambiente.it/pre2024/rinex/{}/{:03}/",
            date.year, date.dayOfYear);
    const auto response = HttpGetText(
        baseUrl, stopToken, 16U * 1024U * 1024U);
    if (!response.ok()) {
        if (response.statusCode == 404) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error = L"意大利 RING：无法读取 ISPRA 官方日目录"
            + (response.error.empty() ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    static std::mutex catalogMutex;
    static Catalog officialCatalog;
    static bool catalogAttempted = false;
    {
        std::scoped_lock lock(catalogMutex);
        if (!catalogAttempted) {
            catalogAttempted = true;
            const auto stations = HttpGetText(
                L"https://gnss.isprambiente.it/le-stazioni/",
                stopToken, 8U * 1024U * 1024U);
            if (stations.ok()) {
                officialCatalog = ParseHtmlLatitudeLongitudeCatalog(
                    stations.body, "ITA", false);
            }
        }
    }
    const Catalog ringCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});
    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        ringCatalog, igsCatalog, baseUrl,
        ObservationNetwork::Ring, &result.files);
    CompleteNationalArchiveStations(
        result, "ITA", ringCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    if (result.stations.empty() && !ExtractHrefs(response.body).empty()) {
        result.warning = L"意大利 RING：目录可读，但当天没有可识别的日观测文件。";
    }
    return result;
}

SourceResult QueryAposBevDaily(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::array<std::wstring, 2> directories{
        std::format(
            L"https://gnss.bev.gv.at/at.gv.bev.dc/data/obs/{}/{:03}/",
            date.year, date.dayOfYear),
        std::format(
            L"https://gnss.bev.gv.at/at.gv.bev.dc/data/obs_v3/{}/{:03}/",
            date.year, date.dayOfYear)
    };
    static std::mutex catalogMutex;
    static Catalog officialCatalog;
    static bool catalogAttempted = false;
    {
        std::scoped_lock lock(catalogMutex);
        if (!catalogAttempted) {
            catalogAttempted = true;
            const auto stations = HttpGetText(
                L"https://www.bev.gv.at/Services/Produkte/"
                L"Grundlagenvermessung/APOS.html",
                stopToken, 8U * 1024U * 1024U);
            if (stations.ok()) {
                officialCatalog = ParseHtmlLatitudeLongitudeCatalog(
                    stations.body, "AUT", false);
            }
        }
    }
    const Catalog aposCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});
    std::wstring lastError;
    bool anyReadable = false;
    std::array<std::future<HttpTextResult>, 2> directoryJobs{
        std::async(std::launch::async, [&]() {
            return HttpGetText(
                directories[0], stopToken, 16U * 1024U * 1024U);
        }),
        std::async(std::launch::async, [&]() {
            return HttpGetText(
                directories[1], stopToken, 16U * 1024U * 1024U);
        })
    };
    for (std::size_t index = 0; index < directories.size(); ++index) {
        const auto& directory = directories[index];
        const auto response = directoryJobs[index].get();
        if (!response.ok()) {
            if (response.statusCode != 404) {
                lastError = response.error;
                RecordTransportFailure(result, response);
            }
            continue;
        }
        anyReadable = true;
        auto parsed = ParseDailyObservationFiles(response.body, date);
        // The BEV regional data centre also contains foreign EPN stations.
        // Keep long-name AUT files directly; legacy short names are retained
        // only when an official/fallback catalog identifies them as Austrian.
        std::erase_if(parsed, [&](const FileCandidate& candidate) {
            if (candidate.stationId.size() == 9) {
                return candidate.stationId.substr(6, 3) != "AUT";
            }
            StationFile probe;
            probe.fourCharId = candidate.fourCharId;
            probe.stationId = candidate.fourCharId;
            ApplyCoordinates(probe, aposCatalog);
            return probe.country != "AUT";
        });
        std::vector<StationFile> files;
        auto stations = ResolveArchiveCandidates(
            parsed, aposCatalog, igsCatalog, directory,
            ObservationNetwork::Apos, &files);
        result.stations.insert(
            result.stations.end(),
            std::make_move_iterator(stations.begin()),
            std::make_move_iterator(stations.end()));
        result.files.insert(
            result.files.end(),
            std::make_move_iterator(files.begin()),
            std::make_move_iterator(files.end()));
    }
    if (!anyReadable) {
        if (!result.networkTransportFailure) {
            result.emptyResultIsComplete = true;
        } else {
            result.error = L"奥地利 APOS/BEV：无法读取官方公开观测档案"
                + (lastError.empty() ? std::wstring{}
                    : L"（" + lastError + L"）");
        }
        return result;
    }
    CompleteNationalArchiveStations(
        result, "AUT", aposCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QueryRobBelgiumDailyApi(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring apiUrl = std::format(
        L"https://gnss.be/api/v1/belgium/doy/{}/{:03}?rinexVersion=all",
        date.year, date.dayOfYear);
    const auto response = HttpGetText(
        apiUrl, stopToken, 16U * 1024U * 1024U);
    if (!response.ok()) {
        if (response.statusCode == 404) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error = L"比利时 ROB：无法读取官方 GNSS.be 日 API"
            + (response.error.empty() ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    static std::mutex catalogMutex;
    static Catalog officialCatalog;
    static bool catalogAttempted = false;
    {
        std::scoped_lock lock(catalogMutex);
        if (!catalogAttempted) {
            catalogAttempted = true;
            const auto page = HttpGetText(
                L"https://gnss.be/belgiandata.php",
                stopToken, 16U * 1024U * 1024U);
            if (page.ok()) {
                officialCatalog = ParseHtmlLatitudeLongitudeCatalog(
                    page.body, "BEL", true);
            }
        }
    }
    const Catalog robCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});
    for (const auto& object : ExtractTopLevelJsonObjects(response.body)) {
        std::string url = ExtractJsonStringValue(object, "url");
        std::string fileName = ExtractJsonStringValue(object, "filename");
        std::string stationId = ToUpperAscii(
            ExtractJsonStringValue(object, "stationId"));
        if (fileName.empty() && !url.empty()) {
            const auto slash = url.find_last_of('/');
            fileName = slash == std::string::npos
                ? url : url.substr(slash + 1);
        }
        if (!IsSafeEposDownloadUrl(url)
            || !IsSafeArchiveFileName(fileName)
            || (stationId.size() != 4 && stationId.size() != 9)) {
            continue;
        }
        StationFile station;
        station.stationId = stationId;
        station.fourCharId = stationId.substr(0, 4);
        station.country = "BEL";
        station.fileName = fileName;
        station.downloadUrl = Utf8ToWide(url);
        station.format = ExtractJsonStringValue(object, "rinexVersion") == "2"
            ? RinexFormat::Rinex2 : RinexFormat::Rinex34;
        station.interval = "30S";
        station.availableNetworks =
            NetworkBit(ObservationNetwork::RobBelgium);
        ApplyCoordinates(station, robCatalog);
        CanonicalizeWithIgs(station, igsCatalog);
        result.files.push_back(station);
        result.stations.push_back(std::move(station));
    }
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QueryDpga(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = std::format(
        L"https://gnss1.tudelft.nl/dpga/rinex/{}/{:03}/",
        date.year, date.dayOfYear);
    const auto response = HttpGetText(
        baseUrl, stopToken, 16U * 1024U * 1024U);
    if (!response.ok()) {
        if (response.statusCode == 404) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error = L"荷兰 DPGA：无法读取 TU Delft 官方日目录"
            + (response.error.empty() ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    static std::mutex catalogMutex;
    static Catalog officialCatalog;
    static bool catalogAttempted = false;
    {
        std::scoped_lock lock(catalogMutex);
        if (!catalogAttempted) {
            catalogAttempted = true;
            const auto coordinates = HttpGetText(
                L"https://gnss1.tudelft.nl/dpga/coordinates.html",
                stopToken, 8U * 1024U * 1024U);
            if (coordinates.ok()) {
                officialCatalog = ParseDpgaXyzCatalog(coordinates.body);
            }
        }
    }
    const Catalog dpgaCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});
    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        dpgaCatalog, igsCatalog, baseUrl,
        ObservationNetwork::Dpga, &result.files);
    CompleteNationalArchiveStations(
        result, "NLD", dpgaCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QueryCatNet(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    constexpr std::array<std::wstring_view, 12> months{
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (date.month < 1 || date.month > 12) {
        result.error = L"西班牙 CatNet：月份无效。";
        return result;
    }
    const bool modern = date.year > 2017
        || (date.year == 2017 && date.dayOfYear >= 156);
    const std::wstring monthPath = std::format(
        L"/{}/diari_30s/RefData.{:02}/Month.{}/",
        modern ? L"rinex3" : L"rinex", date.year % 100,
        months[static_cast<std::size_t>(date.month - 1)]);
    const std::wstring directDay = monthPath
        + std::format(L"Day.{:02}/", date.day);
    struct CatNetEntry {
        std::string name;
        std::string type;
        std::string path;
    };
    const auto readListing = [&](const std::wstring& path) {
        std::string asciiPath;
        asciiPath.reserve(path.size());
        for (const wchar_t character : path) {
            asciiPath.push_back(static_cast<char>(character));
        }
        const std::wstring apiUrl =
            L"https://geofons.icgc.cat/WebInterface/function/"
            L"?command=getXMLListing&path="
            + PercentEncodeAscii(asciiPath);
        auto response = HttpGetText(
            apiUrl, stopToken, 16U * 1024U * 1024U);
        std::vector<CatNetEntry> entries;
        if (response.ok()) {
            // The directory URL redirects to the JavaScript WebInterface.
            // This official endpoint returns the anonymous public XML list.
            if (response.body.find("<listingInfo") == std::string::npos
                || response.body.find("<listing") == std::string::npos) {
                response.error = L"ICGC 公共接口未返回文件 XML 清单。";
            } else {
                const std::regex itemPattern(
                    R"(<listing_subitem\b[^>]*>([\s\S]*?)</listing_subitem>)");
                for (std::sregex_iterator it(
                         response.body.begin(), response.body.end(), itemPattern),
                     end; it != end; ++it) {
                    const std::string item = (*it)[1].str();
                    const auto names = ExtractXmlValues(item, "name");
                    const auto types = ExtractXmlValues(item, "type");
                    const auto paths = ExtractXmlValues(item, "href_path");
                    if (names.empty() || types.empty() || paths.empty()) continue;
                    entries.push_back({
                        names.front(), ToUpperAscii(types.front()), paths.front()});
                }
            }
        }
        return std::pair{std::move(response), std::move(entries)};
    };
    std::vector<FileCandidate> candidates;
    const auto appendFiles = [&](const std::vector<CatNetEntry>& entries) {
        for (const auto& entry : entries) {
            if (entry.type != "FILE"
                || !IsSafeArchiveFileName(entry.name)) continue;
            std::string path = entry.path;
            if (path.ends_with('/')) path += entry.name;
            // href_path normally includes the file name. Preserve it exactly
            // instead of rebuilding or repeating it from the displayed name.
            if ((!path.starts_with("/rinex/diari_30s/")
                    && !path.starts_with("/rinex3/diari_30s/"))
                || path.find("..") != std::string::npos
                || path.find('\\') != std::string::npos
                || !path.ends_with("/" + entry.name)) continue;
            auto parsed = ParseDailyObservationFiles(
                BuildSyntheticDirectoryHtml({entry.name}), date);
            for (auto& candidate : parsed) {
                candidate.downloadPath = path;
                candidates.push_back(std::move(candidate));
            }
        }
    };
    auto [dayResponse, dayEntries] = readListing(directDay);
    if (dayResponse.ok()) appendFiles(dayEntries);
    if (candidates.empty() && !stopToken.stop_requested()) {
        auto [monthResponse, monthEntries] = readListing(monthPath);
        if (!monthResponse.ok() && !dayResponse.ok()) {
            RecordTransportFailure(result, monthResponse);
            result.error = L"西班牙 CatNet：无法读取 ICGC 官方 XML 目录"
                + (monthResponse.error.empty() ? std::wstring{}
                    : L"（" + monthResponse.error + L"）");
            return result;
        }
        // Older layouts can put a four-character station folder between
        // the month and Day directory.
        for (const auto& entry : monthEntries) {
            if (stopToken.stop_requested()) break;
            if (entry.type != "DIR" || entry.name.size() != 4
                || !std::all_of(entry.name.begin(), entry.name.end(),
                    [](const unsigned char ch) { return std::isalnum(ch) != 0; })) {
                continue;
            }
            std::string path = entry.path;
            if ((!path.starts_with("/rinex/diari_30s/")
                    && !path.starts_with("/rinex3/diari_30s/"))
                || path.find("..") != std::string::npos
                || path.find('\\') != std::string::npos) continue;
            if (!path.ends_with('/')) path.push_back('/');
            const std::wstring stationDay = Utf8ToWide(path)
                + std::format(L"Day.{:02}/", date.day);
            auto [response, entries] = readListing(stationDay);
            if (response.ok()) appendFiles(entries);
        }
    }
    result.stations = ResolveArchiveCandidates(
        candidates, archiveCatalog, igsCatalog,
        L"https://geofons.icgc.cat", ObservationNetwork::CatNet, &result.files);
    CompleteNationalArchiveStations(
        result, "ESP", archiveCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QuerySpsLux(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    if (date.year < 2011) {
        result.emptyResultIsComplete = true;
        return result;
    }
    struct YearCacheEntry {
        bool attempted = false;
        std::wstring archiveUrl;
        RemoteZipDirectoryResult directory;
    };
    static std::mutex cacheMutex;
    static std::unordered_map<int, YearCacheEntry> cache;
    YearCacheEntry entry;
    {
        std::scoped_lock lock(cacheMutex);
        entry = cache[date.year];
    }
    if (!entry.attempted) {
        entry.attempted = true;
        const auto dataset = HttpGetText(
            L"https://data.public.lu/api/1/datasets/"
            L"624d5364fe3eaed37bca7c98/",
            stopToken, 8U * 1024U * 1024U);
        if (dataset.ok()) {
            const std::regex urlPattern(
                std::format(
                    R"REGEX("url"\s*:\s*"(https:[^"\\]*{}\.zip)")REGEX",
                    date.year),
                std::regex::icase);
            std::smatch match;
            if (std::regex_search(dataset.body, match, urlPattern)) {
                entry.archiveUrl = Utf8ToWide(match[1].str());
            }
        }
        if (entry.archiveUrl.empty()) {
            entry.archiveUrl = std::format(
                L"https://download.data.public.lu/resources/"
                L"raw-gnss-data-from-the-spslux-network/{}.zip",
                date.year);
        }
        entry.directory = ListRemoteZipDirectory(
            entry.archiveUrl, stopToken);
        std::scoped_lock lock(cacheMutex);
        cache[date.year] = entry;
    }
    if (!entry.directory.error.empty()) {
        result.error = L"卢森堡 SPSLux：无法读取年度公开 ZIP 的远程目录"
            L"（" + entry.directory.error + L"）";
        result.networkTransportFailure = entry.directory.networkFailure;
        result.transportErrorCode = entry.directory.transportErrorCode;
        return result;
    }
    std::vector<std::string> names;
    for (const auto& path : entry.directory.paths) {
        const auto slash = path.find_last_of("/\\");
        const std::string name = slash == std::string::npos
            ? path : path.substr(slash + 1);
        if (IsSafeArchiveFileName(name)) names.push_back(name);
    }
    const auto candidates = ParseDailyObservationFiles(
        BuildSyntheticDirectoryHtml(names), date);
    result.stations = ResolveArchiveCandidates(
        candidates, archiveCatalog, igsCatalog, L"",
        ObservationNetwork::SpsLux, &result.files);
    // Download the indexed member using byte ranges, never the annual container.
    for (auto& station : result.stations) {
        const auto member = entry.directory.memberRoutes.find(station.fileName);
        station.downloadUrl = member == entry.directory.memberRoutes.end()
            ? std::wstring{} : member->second;
    }
    for (auto& file : result.files) {
        const auto member = entry.directory.memberRoutes.find(file.fileName);
        file.downloadUrl = member == entry.directory.memberRoutes.end()
            ? std::wstring{} : member->second;
    }
    CompleteNationalArchiveStations(
        result, "LUX", archiveCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QuerySopacGarner(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    // SOPAC/GARNER is a public scientific archive.  Its daily directory
    // includes both short-name RINEX 2 and long-name RINEX 3 files, so the
    // common candidate resolver can prefer the richer product per station.
    const std::wstring baseUrl = std::format(
        L"https://garner.ucsd.edu/archive/garner/rinex/{}/{:03}/",
        date.year,
        date.dayOfYear);
    const auto response = HttpGetText(
        baseUrl,
        stopToken,
        16U * 1024U * 1024U);
    if (!response.ok()) {
        if (response.statusCode == 404) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error =
            L"SOPAC/GARNER: unable to read the public daily directory"
            + (response.error.empty()
                ? std::wstring{}
                : L": " + response.error);
        return result;
    }
    static std::mutex catalogMutex;
    static Catalog coordinateCatalog;
    static bool coordinateAttempted = false;
    bool coordinateAvailable = false;
    {
        std::scoped_lock lock(catalogMutex);
        if (!coordinateAttempted) {
            coordinateAttempted = true;
            const auto current = HttpGetText(
                L"https://garner.ucsd.edu/pub/gamit/tables/igb20_comb.apr",
                stopToken, 4U * 1024U * 1024U);
            const auto historical = HttpGetText(
                L"https://garner.ucsd.edu/pub/gamit/tables/igb14_comb.apr",
                stopToken, 4U * 1024U * 1024U);
            const Catalog currentCatalog = current.ok()
                ? ParseSopacAprCoordinates(current.body) : Catalog{};
            const Catalog historicalCatalog = historical.ok()
                ? ParseSopacAprCoordinates(historical.body) : Catalog{};
            coordinateCatalog = MergeCatalogs(
                {&historicalCatalog, &currentCatalog});
        }
        coordinateAvailable = !coordinateCatalog.byId.empty();
    }
    const Catalog sopacCatalog = MergeCatalogs(
        {
            &archiveCatalog,
            &coordinateCatalog
        });
    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        sopacCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::SopacGarner,
        &result.files);
    if (result.stations.empty()) {
        result.emptyResultIsComplete = true;
    } else if (!coordinateAvailable) {
        result.warning =
            L"SOPAC/GARNER: files are complete, but both public coordinate "
            L"tables were temporarily unavailable; some map markers may "
            L"be missing.";
    }
    return result;
}

SourceResult QueryTrigNet(
    const QueryDate& date,
    const Catalog& trigNetCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const auto twoDigitYear = std::format(L"{:02}", date.year % 100);
    const auto doy = FormatDoy(date.dayOfYear);
    const bool modern = date.year > 2025
        || (date.year == 2025 && date.dayOfYear >= 105);
    const auto fourDigitYear = std::to_wstring(date.year);
    // TrigNet's public FTP archive has used two layouts.  The legacy
    // RefData.YY tree is still present for older material, while the current
    // AFREF mirror is published below AfrefData.  Probe the documented
    // day/product directories in the new tree first, then retain the legacy
    // paths as a fallback.  A directory probe is cheap (and a missing path is
    // not a transport failure), so this remains safe for daily backfills.
    const std::vector<std::wstring> directories{
        L"ftp://ftp.trignet.co.za/AfrefData/" + fourDigitYear
            + L"/" + doy + L"/30sec_daily/",
        L"ftp://ftp.trignet.co.za/AfrefData/" + fourDigitYear
            + L"/" + doy + L"/L1L2_30sec/",
        L"ftp://ftp.trignet.co.za/AfrefData/" + fourDigitYear
            + L"/" + doy + L"/",
        L"ftp://ftp.trignet.co.za/AfrefData/RefData." + twoDigitYear
            + L"/" + doy + L"/30sec_daily/",
        L"ftp://ftp.trignet.co.za/AfrefData/RefData." + twoDigitYear
            + L"/" + doy + L"/L1L2_30sec/",
        L"ftp://ftp.trignet.co.za/RefData." + twoDigitYear
            + L"/" + doy + L"/30sec_daily/",
        L"ftp://ftp.trignet.co.za/RefData." + twoDigitYear
            + L"/" + doy + L"/L1L2_30sec/"};

    std::wstring baseUrl;
    std::vector<std::string> fileNames;
    std::wstring lastError;
    for (const auto& directory : directories) {
        const auto response = HttpListFtpDirectory(
            directory,
            stopToken,
            2U * 1024U * 1024U);
        if (response.ok()) {
            fileNames = ExtractLineFileNames(response.body);
            if (!fileNames.empty()) {
                baseUrl = directory;
                break;
            }
        } else {
            lastError = response.error;
            const bool missingDirectory = response.statusCode == 404
                || response.transportErrorCode == 9
                || response.transportErrorCode == 78;
            if (!missingDirectory) {
                // Do not multiply a single network outage by every layout
                // candidate.  The builder can classify this as a transport
                // failure and let its normal retry/proxy policy handle it.
                RecordTransportFailure(result, response);
                result.error = L"TrigNet：无法读取官方匿名 FTP 的 30 秒日文件目录"
                    + (lastError.empty()
                        ? std::wstring{}
                        : L"（" + lastError + L"）");
                return result;
            }
        }
    }
    if (baseUrl.empty()) {
        result.error = L"TrigNet：无法读取官方匿名 FTP 的 30 秒日文件目录"
            + (lastError.empty()
                ? std::wstring{}
                : L"（" + lastError + L"）");
        return result;
    }

    const auto candidates = ParseDailyObservationFiles(
        BuildSyntheticDirectoryHtml(fileNames),
        date);
    result.stations = ResolveArchiveCandidates(
        candidates,
        trigNetCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::TrigNet,
        &result.files);

    const std::regex zipPattern(
        R"(^([A-Z0-9]{4})([0-9]{3})[A-Z0-9_-]*\.zip$)",
        std::regex::icase);
    std::unordered_set<std::string> existing;
    for (const auto& station : result.stations) {
        existing.insert(station.fourCharId);
    }
    for (const auto& fileName : fileNames) {
        std::smatch match;
        if (!std::regex_match(fileName, match, zipPattern)
            || std::stoi(match[2].str()) != date.dayOfYear) {
            continue;
        }
        const std::string four = ToUpperAscii(match[1].str());
        if (!trigNetCatalog.byFourChar.contains(four)
            || !existing.insert(four).second) {
            continue;
        }
        StationFile station;
        station.fourCharId = four;
        station.stationId = four + "00ZAF";
        station.country = "ZAF";
        station.fileName = fileName;
        station.downloadUrl = baseUrl + Utf8ToWide(fileName);
        station.format = modern
            ? RinexFormat::Rinex34
            : RinexFormat::Rinex2;
        station.interval = "30S";
        station.availableNetworks =
            NetworkBit(ObservationNetwork::TrigNet);
        ApplyCoordinates(station, trigNetCatalog);
        CanonicalizeWithIgs(station, igsCatalog);
        result.files.push_back(station);
        result.stations.push_back(std::move(station));
    }
    if (result.stations.empty()) {
        result.error = L"TrigNet：当天 30 秒日目录中没有找到观测文件。";
    }
    return result;
}

SourceResult QueryCut(
    const QueryDate& date,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = BuildDirectoryUrl(
        L"https://saegnss02p.curtin.edu.au/ldc/rinex3/daily/",
        date);
    const auto response = HttpGetText(
        baseUrl,
        stopToken,
        2U * 1024U * 1024U);
    if (!response.ok()) {
        result.error = L"CUT：无法读取 Curtin 官方日观测目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }

    Catalog cutCatalog;
    AddCatalogEntry(cutCatalog, {"CUT000AUS", "AUS", -32.0040, 115.8950});
    AddCatalogEntry(cutCatalog, {"CUT200AUS", "AUS", -32.0039, 115.8948});
    AddCatalogEntry(cutCatalog, {"CUTA00AUS", "AUS", -32.0040, 115.8948});
    AddCatalogEntry(cutCatalog, {"CUTB00AUS", "AUS", -32.0039, 115.8948});
    AddCatalogEntry(cutCatalog, {"CUTC00AUS", "AUS", -32.0039, 115.8947});
    FinishCatalog(cutCatalog);

    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        cutCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::Cut);
    if (result.stations.empty()) {
        result.error = L"CUT：当天官方目录中没有找到日观测文件。";
    }
    return result;
}

SourceResult QueryCsn(
    const QueryDate& date,
    const Catalog& csnCatalog,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = std::format(
        L"https://gps.csn.uchile.cl/data/{}/{}/",
        date.year,
        FormatDoy(date.dayOfYear));

    // CSN rate-limits rapid directory crawling (HTTP 429).  Serialize CSN
    // requests, keep a polite minimum interval, and honor a progressively
    // longer cooldown when the server asks us to slow down.
    static std::mutex requestMutex;
    static std::chrono::steady_clock::time_point nextRequest{};
    const auto waitUntil = [&](const auto deadline) {
        while (!stopToken.stop_requested()
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };
    HttpTextResult response;
    {
        std::unique_lock requestLock(requestMutex);
        constexpr std::array cooldowns{
            std::chrono::seconds{15},
            std::chrono::seconds{30},
            std::chrono::seconds{60}
        };
        for (std::size_t attempt = 0; attempt <= cooldowns.size(); ++attempt) {
            waitUntil(nextRequest);
            if (stopToken.stop_requested()) {
                break;
            }
            response = HttpGetText(
                baseUrl,
                stopToken,
                4U * 1024U * 1024U);
            nextRequest = std::chrono::steady_clock::now()
                + std::chrono::milliseconds{2000};
            if (response.statusCode != 429 || attempt == cooldowns.size()) {
                break;
            }
            nextRequest = std::chrono::steady_clock::now()
                + cooldowns[attempt];
        }
    }
    if (!response.ok()) {
        RecordTransportFailure(result, response);
        result.error = L"CSN：无法读取官方公开日观测目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        csnCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::Csn,
        &result.files);
    const auto finishStation = [&](StationFile& station) {
        if (!station.hasCoordinate) {
            ApplyCoordinates(station, archiveCatalog);
        }
        if (station.hasCoordinate) {
            CanonicalizeWithIgs(station, igsCatalog);
        }
        if (station.country.empty()) {
            station.country = "CHL";
        }
    };
    for (auto& station : result.stations) {
        finishStation(station);
    }
    for (auto& file : result.files) {
        finishStation(file);
    }
    if (result.stations.empty()) {
        result.emptyResultIsComplete = true;
        result.warning = L"CSN: the official daily directory was checked "
            L"successfully, but it contains no daily observation files.";
    }
    return result;
}

SourceResult QueryCacs(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = std::format(
        L"https://cacsa.nrcan.gc.ca/gps/data/gpsdata/"
        L"{:02}{:03}/{:02}d/",
        date.year % 100,
        date.dayOfYear,
        date.year % 100);
    const auto directory = HttpGetText(
        baseUrl,
        stopToken,
        8U * 1024U * 1024U);
    if (!directory.ok()) {
        if (directory.statusCode == 404) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, directory);
        result.error = L"加拿大 CACS：无法读取 NRCan 官方日目录"
            + (directory.error.empty()
                ? std::wstring{}
                : L"（" + directory.error + L"）");
        return result;
    }

    Catalog liveCatalog;
    bool coordinatesAvailable = false;
    {
        static std::mutex coordinateMutex;
        static Catalog coordinateCache;
        static bool coordinateCacheReady = false;
        std::scoped_lock lock(coordinateMutex);
        if (!coordinateCacheReady) {
            const auto coordinates = HttpPostFormText(
                L"https://webapp.csrs-scrs.nrcan-rncan.gc.ca/"
                L"geod/process/getStations.php",
                "map=cacs&maxlatitude=90&minlatitude=-90"
                "&minlongitude=-180&maxlongitude=180",
                stopToken,
                8U * 1024U * 1024U);
            if (coordinates.ok()) {
                coordinateCache = ParseCacsCoordinateJson(coordinates.body);
                coordinateCacheReady = !coordinateCache.byId.empty();
            }
        }
        coordinatesAvailable = coordinateCacheReady;
        if (coordinateCacheReady) {
            liveCatalog = coordinateCache;
        }
    }
    const Catalog cacsCatalog = MergeCatalogs(
        {&archiveCatalog, &liveCatalog});
    const auto candidates = ParseDailyObservationFiles(directory.body, date);
    result.stations = ResolveArchiveCandidates(
        candidates,
        cacsCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::Cacs,
        &result.files);
    for (auto& station : result.stations) {
        if (station.country.empty()) {
            station.country = "CAN";
        }
    }
    for (auto& file : result.files) {
        if (file.country.empty()) {
            file.country = "CAN";
        }
    }
    if (result.stations.empty()) {
        result.emptyResultIsComplete = true;
        if (!ExtractHrefs(directory.body).empty()) {
            result.warning =
                L"加拿大 CACS：官方目录可读，但当天没有可识别的RINEX日观测文件。";
        }
    } else if (!coordinatesAvailable) {
        result.warning =
            L"加拿大 CACS：日文件已完整返回，但 NRCan 当前坐标接口暂时"
            L"不可用；少数地图点可能缺少坐标。";
    }
    return result;
}

SourceResult QueryGeoNetNz(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring root =
        L"https://geonet-open-data.s3.ap-southeast-2.amazonaws.com/";
    const std::string yearPrefix = std::format(
        "gnss/rinex/{}/", date.year);
    const std::string dayPrefix = std::format(
        "{}{:03}/", yearPrefix, date.dayOfYear);

    struct YearEntry {
        std::mutex mutex;
        std::condition_variable condition;
        bool loading = false;
        bool ready = false;
        std::unordered_map<int, std::vector<std::string>> filesByDoy;
        std::uint64_t lastUse = 0;
    };
    static std::mutex yearsMutex;
    static std::unordered_map<int, std::shared_ptr<YearEntry>> years;
    static std::uint64_t useSerial = 0;
    std::shared_ptr<YearEntry> yearEntry;
    {
        std::scoped_lock lock(yearsMutex);
        auto& slot = years[date.year];
        if (!slot) slot = std::make_shared<YearEntry>();
        yearEntry = slot;
        yearEntry->lastUse = ++useSerial;
        // Up to eight year workers are supported.  Keep a few additional
        // completed years so a GUI query near a boundary remains a cache hit.
        while (years.size() > 10) {
            auto oldest = years.end();
            for (auto iterator = years.begin(); iterator != years.end(); ++iterator) {
                if (iterator->first == date.year) continue;
                if (oldest == years.end()
                    || iterator->second->lastUse < oldest->second->lastUse) {
                    oldest = iterator;
                }
            }
            if (oldest == years.end()) break;
            years.erase(oldest);
        }
    }

    bool loadYear = false;
    {
        std::unique_lock lock(yearEntry->mutex);
        yearEntry->condition.wait(lock, [&]() {
            return !yearEntry->loading || stopToken.stop_requested();
        });
        if (stopToken.stop_requested()) {
            result.error = L"查询已取消。";
            return result;
        }
        if (!yearEntry->ready) {
            yearEntry->loading = true;
            loadYear = true;
        }
    }
    if (loadYear) {
        std::unordered_map<int, std::vector<std::string>> filesByDoy;
        std::mutex filesMutex;
        std::mutex errorMutex;
        std::wstring loadError;
        long transportCode = 0;
        bool transportFailure = false;
        std::atomic_int nextShard{0};
        std::atomic_bool failed{false};
        const auto recordError = [&](
            const std::wstring& message,
            const bool isTransport,
            const long code) {
            std::scoped_lock lock(errorMutex);
            if (loadError.empty()) {
                loadError = message;
                transportFailure = isTransport;
                transportCode = code;
            }
            failed.store(true);
        };
        const auto loadShard = [&]() {
            while (!stopToken.stop_requested() && !failed.load()) {
                const int shard = nextShard.fetch_add(1);
                if (shard > 36) return;
                const std::string shardPrefix = yearPrefix
                    + std::format("{:02}", shard);
                std::string continuationToken;
                std::unordered_map<int, std::vector<std::string>> shardFiles;
                bool shardComplete = false;
                for (int page = 0;
                     page < 50 && !stopToken.stop_requested();
                     ++page) {
                    std::wstring listUrl = root
                        + L"?list-type=2&max-keys=1000&prefix="
                        + PercentEncodeAscii(shardPrefix);
                    if (!continuationToken.empty()) {
                        listUrl += L"&continuation-token="
                            + PercentEncodeAscii(continuationToken);
                    }
                    const auto listing = HttpGetText(
                        listUrl, stopToken, 8U * 1024U * 1024U);
                    if (!listing.ok()) {
                        recordError(
                            listing.error,
                            IsNetworkTransportFailure(listing),
                            listing.transportErrorCode);
                        return;
                    }
                    for (const auto& key :
                         ExtractXmlValues(listing.body, "Key")) {
                        if (!key.starts_with(yearPrefix)
                            || key.find("..") != std::string::npos) {
                            continue;
                        }
                        const std::size_t doyStart = yearPrefix.size();
                        if (key.size() < doyStart + 5
                            || key[doyStart + 3] != '/') {
                            continue;
                        }
                        const std::string doyText = key.substr(doyStart, 3);
                        if (!std::all_of(
                                doyText.begin(), doyText.end(),
                                [](const unsigned char value) {
                                    return std::isdigit(value) != 0;
                                })) {
                            continue;
                        }
                        const int doy = std::stoi(doyText);
                        if (doy < 1 || doy > 366) continue;
                        const auto slash = key.find_last_of('/');
                        if (slash == std::string::npos
                            || slash + 1 >= key.size()) {
                            continue;
                        }
                        const std::string fileName = key.substr(slash + 1);
                        if (IsSafeArchiveFileName(fileName)) {
                            shardFiles[doy].push_back(fileName);
                        }
                    }
                    if (listing.body.find(
                            "<IsTruncated>true</IsTruncated>")
                        == std::string::npos) {
                        shardComplete = true;
                        break;
                    }
                    const auto tokens = ExtractXmlValues(
                        listing.body, "NextContinuationToken");
                    if (tokens.empty() || tokens.front().empty()) {
                        recordError(
                            L"S3分页响应缺少继续令牌", false, 0);
                        return;
                    }
                    continuationToken = tokens.front();
                }
                if (!shardComplete && !stopToken.stop_requested()) {
                    recordError(L"S3分片目录分页超过安全上限", false, 0);
                    return;
                }
                std::scoped_lock lock(filesMutex);
                for (auto& [doy, names] : shardFiles) {
                    auto& destination = filesByDoy[doy];
                    destination.insert(
                        destination.end(),
                        std::make_move_iterator(names.begin()),
                        std::make_move_iterator(names.end()));
                }
            }
        };
        {
            std::vector<std::jthread> workers;
            workers.reserve(8);
            for (int worker = 0; worker < 8; ++worker) {
                workers.emplace_back(loadShard);
            }
        }
        {
            std::scoped_lock lock(yearEntry->mutex);
            yearEntry->loading = false;
            if (loadError.empty() && !stopToken.stop_requested()) {
                yearEntry->filesByDoy = std::move(filesByDoy);
                yearEntry->ready = true;
            }
        }
        yearEntry->condition.notify_all();
        if (!loadError.empty()) {
            result.networkTransportFailure = transportFailure;
            result.transportErrorCode = transportCode;
            result.error =
                L"新西兰 GeoNet/PositioNZ：无法读取官方公开S3年度目录"
                + (loadError.empty() ? std::wstring{}
                    : L"（" + loadError + L"）");
            return result;
        }
        if (stopToken.stop_requested()) {
            result.error = L"查询已取消。";
            return result;
        }
    }

    std::vector<std::string> fileNames;
    {
        std::scoped_lock lock(yearEntry->mutex);
        const auto found = yearEntry->filesByDoy.find(date.dayOfYear);
        if (found != yearEntry->filesByDoy.end()) {
            fileNames = found->second;
        }
    }
    const auto candidates = ParseDailyObservationFiles(
        BuildSyntheticDirectoryHtml(fileNames), date);
    if (candidates.empty()) {
        result.emptyResultIsComplete = true;
        if (!fileNames.empty()) {
            result.error =
                L"新西兰 GeoNet/PositioNZ：S3当日目录存在对象，"
                L"但没有可识别的RINEX日观测文件。";
        }
        return result;
    }

    Catalog liveCatalog;
    bool coordinatesAvailable = false;
    {
        static std::mutex coordinateMutex;
        static Catalog coordinateCache;
        static bool coordinateCacheReady = false;
        std::scoped_lock lock(coordinateMutex);
        if (!coordinateCacheReady) {
            const auto coordinates = HttpGetText(
                L"https://api.geonet.org.nz/network/station?sensorType=8",
                stopToken,
                4U * 1024U * 1024U);
            if (coordinates.ok()) {
                coordinateCache =
                    ParseGeoNetCoordinateGeoJson(coordinates.body);
                coordinateCacheReady = !coordinateCache.byId.empty();
            }
        }
        coordinatesAvailable = coordinateCacheReady;
        if (coordinateCacheReady) {
            liveCatalog = coordinateCache;
        }
    }
    const Catalog geoNetCatalog = MergeCatalogs(
        {&archiveCatalog, &liveCatalog});
    const std::wstring baseUrl = root + Utf8ToWide(dayPrefix);
    result.stations = ResolveArchiveCandidates(
        candidates,
        geoNetCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::GeoNetNz,
        &result.files);
    for (auto& station : result.stations) {
        if (station.country.empty()) station.country = "NZL";
    }
    for (auto& file : result.files) {
        if (file.country.empty()) file.country = "NZL";
    }
    if (!coordinatesAvailable) {
        result.warning =
            L"新西兰 GeoNet/PositioNZ：文件目录完整，但官方坐标API"
            L"暂时不可用；少数地图点可能缺少坐标。";
    }
    return result;
}

SourceResult QueryMagnaEco(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const auto coordinateResponse = HttpGetText(
        L"https://services2.arcgis.com/nBgFvSzxargFHWca/ArcGIS/"
        L"rest/services/Proyectos___Aeroestudios_y_Batimetria_WFL1/"
        L"FeatureServer/21/query?where=1%3D1&outFields=*"
        L"&returnGeometry=true&outSR=4326&f=geojson",
        stopToken,
        16U * 1024U * 1024U);
    const Catalog officialCatalog = coordinateResponse.ok()
        ? ParseArcGisGeoJsonCoordinates(
            coordinateResponse.body,
            "MRTNomencl",
            "COL")
        : Catalog{};
    const Catalog magnaCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});

    const std::array<std::wstring, 2> directories{
        std::format(
            L"ftp://magnaeco:magnaeco1@srvstorage.igac.gov.co/"
            L"Data/MAGNA-ECO/{}/{:03}/Observados/",
            date.year,
            date.dayOfYear),
        std::format(
            L"ftp://magnaeco:magnaeco1@srvstorage.igac.gov.co/"
            L"Data/MAGNA-ECO/{}/{}/Observados/",
            date.year,
            date.dayOfYear)
    };
    std::wstring lastError;
    long transportErrorCode = 0;
    for (const auto& directory : directories) {
        const auto response = HttpListFtpDirectory(
            directory,
            stopToken,
            8U * 1024U * 1024U);
        if (!response.ok()) {
            lastError = response.error;
            if (transportErrorCode == 0 && IsNetworkTransportFailure(response)) {
                transportErrorCode = response.transportErrorCode;
            }
            continue;
        }
        result.stations = ResolveArchiveCandidates(
            ParseDailyObservationFiles(
                BuildSyntheticDirectoryHtml(
                    ExtractLineFileNames(response.body)),
                date),
            magnaCatalog,
            igsCatalog,
            directory,
            ObservationNetwork::MagnaEco,
            &result.files);
        if (!result.stations.empty()) {
            break;
        }
    }
    for (auto& station : result.stations) {
        if (station.country.empty()) {
            station.country = "COL";
        }
        CanonicalizeWithIgs(station, igsCatalog);
    }
    for (auto& file : result.files) {
        if (file.country.empty()) {
            file.country = "COL";
        }
        CanonicalizeWithIgs(file, igsCatalog);
    }
    if (result.stations.empty()) {
        if (transportErrorCode != 0) {
            result.networkTransportFailure = true;
            result.transportErrorCode = transportErrorCode;
        }
        result.error =
            L"哥伦比亚 MAGNA-ECO：官方固定凭据 FTP 未返回当天日观测文件"
            + (lastError.empty()
                ? std::wstring{}
                : L"（" + lastError + L"）");
    } else if (!coordinateResponse.ok()) {
        result.error =
            L"哥伦比亚 MAGNA-ECO：当天文件已返回，但 IGAC 当前坐标服务"
            L"暂时不可用；少数地图点可能缺少坐标。";
    }
    return result;
}

SourceResult QueryPanamaCors(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    constexpr std::array<std::wstring_view, 12> months{
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
        L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"
    };
    if (date.month < 1 || date.month > 12) {
        result.error = L"巴拿马 IGNTG CORS：日期月份无效。";
        return result;
    }
    const std::wstring pattern = std::format(
        L"RefData.{:02}/Month.{}/*/*{:03}0.{:02}o.zip",
        date.year % 100,
        months[static_cast<std::size_t>(date.month - 1)],
        date.dayOfYear,
        date.year % 100);
    const auto response = HttpListPanamaSftpFiles(
        pattern,
        stopToken,
        4U * 1024U * 1024U);
    if (!response.ok()) {
        result.error =
            L"巴拿马 IGNTG CORS：无法读取官方公开凭据 SFTP 日文件"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }

    const Catalog officialCatalog = PanamaCoordinateCatalog();
    const Catalog panamaCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});
    const std::regex pathPattern(
        R"((RefData\.[0-9]{2}/Month\.[A-Za-z]{3}/([A-Za-z0-9]{4})/([A-Za-z0-9._-]+\.zip)))",
        std::regex::icase);
    std::unordered_set<std::string> seen;
    for (std::sregex_iterator iterator(
             response.body.begin(),
             response.body.end(),
             pathPattern),
         end;
         iterator != end;
         ++iterator) {
        const std::string remotePath = (*iterator)[1].str();
        const std::string four = ToUpperAscii((*iterator)[2].str());
        const std::string fileName = (*iterator)[3].str();
        if (!seen.insert(four).second
            || !IsSafeArchiveFileName(fileName)) {
            continue;
        }
        StationFile station;
        station.stationId = four;
        station.fourCharId = four;
        station.country = "PAN";
        station.fileName = fileName;
        station.downloadUrl =
            L"sftp://corsigntg@cors.anati.gob.pa/"
            + Utf8ToWide(remotePath);
        station.format = RinexFormat::Rinex2;
        station.interval = "30S";
        station.availableNetworks =
            NetworkBit(ObservationNetwork::PanamaCors);
        ApplyCoordinates(station, panamaCatalog);
        CanonicalizeWithIgs(station, igsCatalog);
        result.stations.push_back(std::move(station));
    }
    if (result.stations.empty()) {
        result.error =
            L"巴拿马 IGNTG CORS：当天官方 SFTP 中没有找到日观测 ZIP。";
    }
    return result;
}

SourceResult QueryHondurasCors(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    struct StationSource {
        std::string id;
        std::wstring folder;
    };
    const std::array<StationSource, 7> sources{{
        {"SPS1", L"SanPedroSula"},
        {"CEIB", L"LaCeiba"},
        {"JUT1", L"Juticalpa"},
        {"COMA", L"Comayagua"},
        {"SIGU", L"Siguatepeque"},
        {"PUC1", L"PuertoCortes"},
        {"TEG2", L"Tegucigalpa"}
    }};
    SourceResult result;
    const Catalog officialCatalog = HondurasCoordinateCatalog();
    const Catalog hondurasCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});
    const std::string expectedDate = std::format(
        "{:04}{:02}{:02}",
        date.year,
        date.month,
        date.day);
    int failedDirectories = 0;
    for (const auto& source : sources) {
        if (stopToken.stop_requested()) {
            break;
        }
        const std::wstring directoryUrl =
            L"https://www.ign.hn/repositorio?path=EstacionesCors%2F"
            + source.folder;
        const auto response = HttpGetText(
            directoryUrl,
            stopToken,
            4U * 1024U * 1024U);
        if (!response.ok()) {
            ++failedDirectories;
            continue;
        }
        std::string selectedHref;
        std::string selectedName;
        for (const auto& href : ExtractHrefs(response.body)) {
            const auto slash = href.find_last_of('/');
            const std::string name =
                slash == std::string::npos
                ? href
                : href.substr(slash + 1);
            const std::string lower = ToLowerAscii(name);
            if (name.find(expectedDate) == std::string::npos
                || !lower.ends_with(".zip")
                || !IsSafeArchiveFileName(name)) {
                continue;
            }
            selectedHref = href;
            selectedName = name;
            break;
        }
        if (selectedName.empty()) {
            continue;
        }
        std::wstring downloadUrl;
        if (selectedHref.starts_with("https://")) {
            downloadUrl = Utf8ToWide(selectedHref);
        } else if (selectedHref.starts_with("/")) {
            downloadUrl =
                L"https://www.ign.hn" + Utf8ToWide(selectedHref);
        } else {
            downloadUrl =
                L"https://www.ign.hn/storage/EstacionesCors/"
                + source.folder + L"/" + Utf8ToWide(selectedName);
        }
        StationFile station;
        station.stationId = source.id;
        station.fourCharId = source.id;
        station.country = "HND";
        station.fileName = selectedName;
        station.downloadUrl = std::move(downloadUrl);
        station.format = RinexFormat::Rinex2;
        station.interval = "RAW";
        station.availableNetworks =
            NetworkBit(ObservationNetwork::HondurasCors);
        ApplyCoordinates(station, hondurasCatalog);
        CanonicalizeWithIgs(station, igsCatalog);
        result.stations.push_back(std::move(station));
    }
    if (result.stations.empty()) {
        result.error = failedDirectories == static_cast<int>(sources.size())
            ? L"洪都拉斯 IGN CORS：官方仓库的 7 个测站目录均无法读取。"
            : L"洪都拉斯 IGN CORS：官方仓库当天没有找到可下载 ZIP。";
    } else if (failedDirectories > 0) {
        result.error = std::format(
            L"洪都拉斯 IGN CORS：{} 个官方测站目录暂时无法读取，其余结果已显示。",
            failedDirectories);
    }
    return result;
}

SourceResult QueryEposIceland(
    const QueryDate& date,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    // The EPOS API efficiently returns long date ranges.  Keep one rolling
    // 365-day block in memory so the day-by-day builder only queries each
    // station once per block, while the SQLite database remains daily.
    struct BatchCache {
        std::mutex mutex;
        std::unordered_map<std::string, SourceResult> days;
    };
    static BatchCache cache;
    const auto dateKey = [](const QueryDate& value) {
        return std::format(
            "{:04}-{:02}-{:02}", value.year, value.month, value.day);
    };
    const std::string requestedKey = dateKey(date);
    {
        std::scoped_lock lock(cache.mutex);
        const auto found = cache.days.find(requestedKey);
        if (found != cache.days.end()) {
            return found->second;
        }
    }

    const auto requestedDay = std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
    constexpr int batchDayCount = 365;
    const auto firstDay = requestedDay - std::chrono::days{batchDayCount - 1};
    std::vector<QueryDate> batchDates;
    std::unordered_map<std::string, std::size_t> batchIndex;
    batchDates.reserve(batchDayCount);
    batchIndex.reserve(batchDayCount);
    for (int offset = 0; offset < batchDayCount; ++offset) {
        const std::chrono::year_month_day calendar{
            firstDay + std::chrono::days{offset}};
        QueryDate batchDate = MakeQueryDate(
            static_cast<int>(calendar.year()),
            static_cast<unsigned>(calendar.month()),
            static_cast<unsigned>(calendar.day()));
        batchIndex.emplace(dateKey(batchDate), batchDates.size());
        batchDates.push_back(batchDate);
    }
    std::vector<SourceResult> batchResults(batchDates.size());

    const auto catalogResponse = HttpGetText(
        L"https://api.vedur.is/epos/gps/station?"
        L"bbox=62.8%2C-25.0%2C67.0%2C-12.5&format_type=JSON",
        stopToken,
        8U * 1024U * 1024U);
    if (!catalogResponse.ok()) {
        SourceResult result;
        RecordTransportFailure(result, catalogResponse);
        result.error = L"冰岛 EPOS/IMO：无法读取官方测站 API"
            + (catalogResponse.error.empty()
                ? std::wstring{}
                : L"（" + catalogResponse.error + L"）");
        return result;
    }

    struct IcelandStation {
        std::string marker;
        double latitude = 0.0;
        double longitude = 0.0;
        bool hasCoordinate = false;
        ObservationNetworkMask memberships = 0;
    };
    std::vector<IcelandStation> stationsToQuery;
    std::unordered_set<std::string> catalogMarkers;
    for (const auto& object :
         ExtractTopLevelJsonObjects(catalogResponse.body)) {
        const std::string marker = ToUpperAscii(
            ExtractJsonStringValue(object, "marker"));
        if (marker.size() != 4
            || !catalogMarkers.insert(marker).second) {
            continue;
        }
        IcelandStation station;
        station.marker = marker;
        station.memberships =
            NetworkBit(ObservationNetwork::EposIceland);
        station.hasCoordinate =
            ExtractJsonNumberValue(object, "lat", station.latitude)
            && ExtractJsonNumberValue(object, "lon", station.longitude);
        stationsToQuery.push_back(std::move(station));
    }
    if (stationsToQuery.empty()) {
        SourceResult result;
        result.error =
            L"冰岛 EPOS/IMO：官方测站 API 中没有测站。";
        return result;
    }

    std::atomic_size_t nextStation{0};
    std::atomic_int failedRequests{0};
    std::atomic_long transportErrorCode{0};
    std::mutex resultMutex;
    const auto worker = [&]() {
        while (!stopToken.stop_requested()) {
            const std::size_t index = nextStation.fetch_add(1);
            if (index >= stationsToQuery.size()) {
                return;
            }
            const auto& apiStation = stationsToQuery[index];
            const std::wstring url = std::format(
                L"https://api.vedur.is/epos/gps/rinex?"
                L"station_marker={}"
                L"&start_time={:04}-{:02}-{:02}T00%3A00%3A00"
                L"&end_time={:04}-{:02}-{:02}T23%3A59%3A59"
                L"&format_type=JSON",
                Utf8ToWide(apiStation.marker),
                batchDates.front().year,
                batchDates.front().month,
                batchDates.front().day,
                batchDates.back().year,
                batchDates.back().month,
                batchDates.back().day);
            const auto response = HttpGetText(
                url,
                stopToken,
                2U * 1024U * 1024U);
            if (!response.ok()) {
                failedRequests.fetch_add(1);
                if (IsNetworkTransportFailure(response)) {
                    long expected = 0;
                    transportErrorCode.compare_exchange_strong(
                        expected, response.transportErrorCode);
                }
                continue;
            }
            std::unordered_set<std::string> seenFiles;
            for (const auto& object :
                 ExtractTopLevelJsonObjects(response.body)) {
                const std::string referenceDate =
                    ExtractJsonStringValue(object, "reference_date");
                const std::string marker = ToUpperAscii(
                    ExtractJsonStringValue(object, "station_marker"));
                const std::string fileName =
                    ExtractJsonStringValue(object, "file_name");
                std::string downloadUrl =
                    ExtractJsonStringValue(object, "url");
                // Some public API rows append file_name twice. Normalize
                // only an exact repeated suffix; retain the official host
                // and directory supplied by the provider.
                if (IsSafeArchiveFileName(fileName)) {
                    const std::string duplicateSuffix = fileName + fileName;
                    while (downloadUrl.ends_with(duplicateSuffix)) {
                        downloadUrl.resize(downloadUrl.size() - fileName.size());
                    }
                }
                if (referenceDate.size() < 10
                    || marker != apiStation.marker
                    || !IsSafeArchiveFileName(fileName)
                    || !downloadUrl.starts_with(
                        "https://data.epos-iceland.is/files/gps/")
                    || !seenFiles.insert(fileName).second) {
                    continue;
                }
                const auto day = batchIndex.find(referenceDate.substr(0, 10));
                if (day == batchIndex.end()) {
                    continue;
                }

                StationFile station;
                station.stationId = marker;
                station.fourCharId = marker;
                station.country = "ISL";
                station.fileName = fileName;
                station.downloadUrl = Utf8ToWide(downloadUrl);
                // EPOS-IS has advertised some short-name .yyD.Z files as
                // RINEX3 in its API. The archive file header is RINEX 2;
                // the legacy observation name is more reliable here.
                std::string legacyName = ToUpperAscii(fileName);
                if (legacyName.ends_with(".Z")) {
                    legacyName.resize(legacyName.size() - 2);
                }
                const bool legacyRinex2 = legacyName.size() == 12
                    && legacyName[8] == '.'
                    && std::isdigit(static_cast<unsigned char>(legacyName[9]))
                    && std::isdigit(static_cast<unsigned char>(legacyName[10]))
                    && (legacyName[11] == 'D' || legacyName[11] == 'O');
                station.format =
                    legacyRinex2
                    || ExtractJsonStringValue(object, "file_type") == "RINEX2"
                    ? RinexFormat::Rinex2
                    : RinexFormat::Rinex34;
                station.interval = ToUpperAscii(
                    ExtractJsonStringValue(
                        object,
                        "sampling_frequency"));
                station.availableNetworks = apiStation.memberships;
                station.latitude = apiStation.latitude;
                station.longitude = apiStation.longitude;
                station.hasCoordinate = apiStation.hasCoordinate;
                double fileLatitude = 0.0;
                double fileLongitude = 0.0;
                if (ExtractJsonNumberValue(
                        object,
                        "lat",
                        fileLatitude)
                    && ExtractJsonNumberValue(
                        object,
                        "lon",
                        fileLongitude)) {
                    station.latitude = fileLatitude;
                    station.longitude = fileLongitude;
                    station.hasCoordinate = true;
                }
                CanonicalizeWithIgs(station, igsCatalog);
                std::scoped_lock lock(resultMutex);
                batchResults[day->second].stations.push_back(
                    std::move(station));
            }
        }
    };
    {
        std::vector<std::jthread> workers;
        const std::size_t workerCount =
            std::min<std::size_t>(6, stationsToQuery.size());
        workers.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers.emplace_back(worker);
        }
    }
    const bool allRequestsFailed =
        failedRequests.load() == static_cast<int>(stationsToQuery.size());
    for (auto& dayResult : batchResults) {
        dayResult.files = dayResult.stations;
        if (dayResult.stations.empty()
            && allRequestsFailed
            && transportErrorCode.load() != 0) {
            dayResult.networkTransportFailure = true;
            dayResult.transportErrorCode = transportErrorCode.load();
        }
        if (dayResult.stations.empty()) {
            dayResult.error =
                L"冰岛 EPOS/IMO：当天官方 API 中没有可下载的 RINEX 文件。";
            if (allRequestsFailed) {
                dayResult.error += L" 所有测站请求均失败。";
            }
        } else if (failedRequests.load() > 0) {
            dayResult.error = std::format(
                L"冰岛 EPOS/IMO：{} 个测站 API 请求失败，其余结果已显示。",
                failedRequests.load());
        }
    }

    SourceResult requestedResult = batchResults.back();
    if (!stopToken.stop_requested() && !allRequestsFailed) {
        std::scoped_lock lock(cache.mutex);
        cache.days.clear();
        cache.days.reserve(batchResults.size());
        for (std::size_t index = 0; index < batchResults.size(); ++index) {
            cache.days.emplace(
                dateKey(batchDates[index]),
                std::move(batchResults[index]));
        }
        requestedResult = cache.days.at(requestedKey);
    }
    return requestedResult;
}

SourceResult QueryEposGateway(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken,
    const ObservationNetwork network,
    const std::wstring_view displayLabel,
    const std::string_view networkFilter,
    const std::string_view countryFilter = {}) {
    // The pan-European GLASS gateway can return every regular RINEX file in
    // a date range.  Cache a fixed 14-day calendar block so either ascending
    // or descending historical scans make one file request per block rather
    // than one request per day.  High-rate data use a separate API and are
    // deliberately not included here.
    struct BatchCache {
        struct CachedBlock {
            std::unordered_map<std::string, SourceResult> days;
            std::uint64_t lastUse = 0;
        };
        std::mutex mutex;
        std::unordered_map<std::string, CachedBlock> blocks;
        Catalog coordinates;
        std::uint64_t useSerial = 0;
    };
    static std::mutex cachesMutex;
    static std::unordered_map<ObservationNetworkMask,
        std::shared_ptr<BatchCache>> caches;
    std::shared_ptr<BatchCache> cache;
    {
        std::scoped_lock lock(cachesMutex);
        auto& entry = caches[NetworkBit(network)];
        if (!entry) entry = std::make_shared<BatchCache>();
        cache = entry;
    }
    const auto dateKey = [](const QueryDate& value) {
        return std::format(
            "{:04}-{:02}-{:02}", value.year, value.month, value.day);
    };
    const std::string requestedKey = dateKey(date);

    const auto requestedDay = std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
    constexpr int batchDayCount = 14;
    const auto ordinal = requestedDay.time_since_epoch().count();
    const auto remainder = (ordinal % batchDayCount + batchDayCount)
        % batchDayCount;
    const auto firstDay = requestedDay - std::chrono::days{remainder};

    std::vector<QueryDate> batchDates;
    std::unordered_map<std::string, std::size_t> batchIndex;
    std::unordered_map<std::string, std::size_t> longDateIndex;
    std::unordered_map<std::string, std::size_t> shortDateIndex;
    batchDates.reserve(batchDayCount);
    batchIndex.reserve(batchDayCount);
    longDateIndex.reserve(batchDayCount);
    shortDateIndex.reserve(batchDayCount);
    for (int offset = 0; offset < batchDayCount; ++offset) {
        const std::chrono::year_month_day calendar{
            firstDay + std::chrono::days{offset}};
        QueryDate batchDate = MakeQueryDate(
            static_cast<int>(calendar.year()),
            static_cast<unsigned>(calendar.month()),
            static_cast<unsigned>(calendar.day()));
        batchIndex.emplace(dateKey(batchDate), batchDates.size());
        longDateIndex.emplace(
            std::format("{:04}{:03}", batchDate.year, batchDate.dayOfYear),
            batchDates.size());
        shortDateIndex.emplace(
            std::format("{:02}{:03}", batchDate.year % 100,
                batchDate.dayOfYear),
            batchDates.size());
        batchDates.push_back(batchDate);
    }
    const std::string blockKey = dateKey(batchDates.front());
    {
        std::scoped_lock lock(cache->mutex);
        const auto block = cache->blocks.find(blockKey);
        if (block != cache->blocks.end()) {
            const auto found = block->second.days.find(requestedKey);
            if (found != block->second.days.end()) {
                block->second.lastUse = ++cache->useSerial;
                return found->second;
            }
        }
    }
    std::vector<SourceResult> batchResults(batchDates.size());

    const std::wstring fileQuery = networkFilter.empty()
        ? std::format(
            L"https://gnssdata-epos.oca.eu/GlassFramework/webresources/"
            L"files/dateRange/date_from={:04}-{:02}-{:02}%26date_to="
            L"{:04}-{:02}-{:02}/script/0/50000?filtervalidatedfiles=1",
            batchDates.front().year,
            batchDates.front().month,
            batchDates.front().day,
            batchDates.back().year,
            batchDates.back().month,
            batchDates.back().day)
        : std::format(
            L"https://gnssdata-epos.oca.eu/GlassFramework/webresources/"
            L"files/combination/network={}%26dateRange="
            L"{:04}-{:02}-{:02},{:04}-{:02}-{:02}/script/0/50000"
            L"?filtervalidatedfiles=1",
            PercentEncodeAscii(networkFilter),
            batchDates.front().year,
            batchDates.front().month,
            batchDates.front().day,
            batchDates.back().year,
            batchDates.back().month,
            batchDates.back().day);
    const auto fileResponse = HttpGetText(
        fileQuery,
        stopToken,
        64U * 1024U * 1024U);
    if (!fileResponse.ok()) {
        SourceResult failure;
        RecordTransportFailure(failure, fileResponse);
        failure.error = std::wstring(displayLabel)
            + L"：无法读取官方 EPOS GLASS 文件目录"
            + (fileResponse.error.empty()
                ? std::wstring{}
                : L"（" + fileResponse.error + L"）");
        return failure;
    }

    std::vector<std::string> fileUrls;
    {
        std::istringstream lines(fileResponse.body);
        std::string line;
        while (std::getline(lines, line)) {
            line = TrimAscii(std::move(line));
            if (IsSafeEposDownloadUrl(line)) {
                fileUrls.push_back(std::move(line));
            }
        }
    }
    if (fileUrls.empty() && !TrimAscii(fileResponse.body).empty()) {
        SourceResult failure;
        failure.error = std::wstring(displayLabel)
            + L"：官方 EPOS GLASS 轻量路线接口返回了无法识别的内容。";
        return failure;
    }

    static const std::regex longName(
        R"(^([A-Z0-9]{9})_[RSU]_([0-9]{4})([0-9]{3})0000_01D_([0-9]{2}[SMH])_([A-Z]O)\.(crx|rnx)\.gz$)",
        std::regex::icase);
    static const std::regex shortName(
        R"(^([A-Z0-9]{4})([0-9]{3})([0A-X])\.([0-9]{2})([DO])\.(gz|Z)$)",
        std::regex::icase);
    std::unordered_set<std::string> allMarkers;
    for (const auto& downloadUrl : fileUrls) {
        if (stopToken.stop_requested()) {
            SourceResult cancelled;
            cancelled.error = L"查询已取消。";
            return cancelled;
        }
        const auto queryFile = downloadUrl.find("file=");
        const auto lastSlash = downloadUrl.find_last_of('/');
        std::size_t nameStart = queryFile == std::string::npos
            ? (lastSlash == std::string::npos ? 0 : lastSlash + 1)
            : queryFile + 5;
        if (queryFile != std::string::npos) {
            if (lastSlash != std::string::npos && lastSlash >= nameStart) {
                nameStart = lastSlash + 1;
            }
        }
        const auto nameEnd = downloadUrl.find_first_of("?#&", nameStart);
        const std::string fileName = downloadUrl.substr(
            nameStart,
            nameEnd == std::string::npos
                ? std::string::npos : nameEnd - nameStart);
        if (!IsSafeArchiveFileName(fileName)
            || !IsSafeEposDownloadUrl(downloadUrl)) {
            continue;
        }

        std::smatch match;
        std::size_t dayIndex = batchDates.size();
        std::string stationId;
        std::string fourCharId;
        std::string interval;
        RinexFormat format = RinexFormat::Rinex34;
        if (std::regex_match(fileName, match, longName)) {
            stationId = ToUpperAscii(match[1].str());
            fourCharId = stationId.substr(0, 4);
            if (!countryFilter.empty()
                && stationId.substr(6, 3) != countryFilter) {
                continue;
            }
            const int fileYear = std::stoi(match[2].str());
            const int fileDoy = std::stoi(match[3].str());
            const auto day = longDateIndex.find(
                std::format("{:04}{:03}", fileYear, fileDoy));
            if (day == longDateIndex.end()) continue;
            dayIndex = day->second;
            interval = ToUpperAscii(match[4].str());
        } else if (std::regex_match(fileName, match, shortName)) {
            fourCharId = ToUpperAscii(match[1].str());
            stationId = fourCharId;
            const auto day = shortDateIndex.find(
                match[4].str() + match[2].str());
            if (day == shortDateIndex.end()) continue;
            dayIndex = day->second;
            format = RinexFormat::Rinex2;
        } else {
            continue;
        }

        StationFile station;
        station.stationId = stationId;
        station.fourCharId = fourCharId;
        station.country = stationId.size() == 9
            ? stationId.substr(6, 3)
            : std::string{};
        station.fileName = fileName;
        station.downloadUrl = Utf8ToWide(downloadUrl);
        station.format = format;
        station.interval = interval;
        station.availableNetworks = NetworkBit(network);
        if (!countryFilter.empty() && stationId.size() != 9) {
            ApplyCoordinates(station, archiveCatalog);
            if (!station.country.empty()
                && station.country != countryFilter) {
                continue;
            }
        }
        batchResults[dayIndex].stations.push_back(std::move(station));
        allMarkers.insert(stationId);
    }

    // Fetch station metadata in bounded marker batches.  The all-stations
    // endpoint is currently empty on the production gateway, whereas marker
    // batches return the authoritative coordinates reliably.
    Catalog cachedCoordinates;
    {
        std::scoped_lock lock(cache->mutex);
        cachedCoordinates = cache->coordinates;
    }
    std::vector<std::string> missingMarkers;
    missingMarkers.reserve(allMarkers.size());
    for (const auto& marker : allMarkers) {
        StationFile probe;
        probe.stationId = marker;
        probe.fourCharId = marker.substr(0, std::min<std::size_t>(4, marker.size()));
        ApplyCoordinates(probe, archiveCatalog);
        if (!probe.hasCoordinate) {
            ApplyCoordinates(probe, cachedCoordinates);
        }
        if (!probe.hasCoordinate) {
            missingMarkers.push_back(marker);
        }
    }
    std::sort(missingMarkers.begin(), missingMarkers.end());

    constexpr std::size_t markerBatchSize = 80;
    const std::size_t metadataBatchCount =
        (missingMarkers.size() + markerBatchSize - 1) / markerBatchSize;
    std::atomic_size_t nextMetadataBatch{0};
    std::atomic_int failedMetadataBatches{0};
    std::atomic_long metadataTransportCode{0};
    Catalog fetchedCoordinates;
    std::mutex fetchedMutex;
    const auto metadataWorker = [&]() {
        while (!stopToken.stop_requested()) {
            const std::size_t batch = nextMetadataBatch.fetch_add(1);
            if (batch >= metadataBatchCount) {
                return;
            }
            const std::size_t begin = batch * markerBatchSize;
            const std::size_t end = std::min(
                missingMarkers.size(), begin + markerBatchSize);
            std::string markerList;
            for (std::size_t index = begin; index < end; ++index) {
                if (!markerList.empty()) markerList.push_back(',');
                markerList += missingMarkers[index];
            }
            const std::wstring url =
                L"https://gnssdata-epos.oca.eu/GlassFramework/webresources/"
                L"stations/marker/"
                + Utf8ToWide(markerList)
                + L"/short/json";
            const auto response = HttpGetText(
                url,
                stopToken,
                8U * 1024U * 1024U);
            if (!response.ok()) {
                failedMetadataBatches.fetch_add(1);
                if (IsNetworkTransportFailure(response)) {
                    long expected = 0;
                    metadataTransportCode.compare_exchange_strong(
                        expected, response.transportErrorCode);
                }
                continue;
            }
            Catalog local;
            for (const auto& object :
                 ExtractTopLevelJsonObjects(response.body)) {
                std::string marker = ToUpperAscii(
                    ExtractJsonStringValue(object, "marker"));
                if (marker.empty()) {
                    marker = ToUpperAscii(
                        ExtractJsonStringValue(object, "marker_long_name"));
                }
                double latitude = 0.0;
                double longitude = 0.0;
                if ((marker.size() != 4 && marker.size() != 9)
                    || !ExtractJsonNumberValue(object, "lat", latitude)
                    || !ExtractJsonNumberValue(object, "lon", longitude)
                    || std::abs(latitude) > 90.0
                    || std::abs(longitude) > 180.0) {
                    continue;
                }
                AddCatalogEntry(
                    local,
                    {marker,
                     marker.size() == 9 ? marker.substr(6, 3) : std::string{},
                     latitude,
                     longitude});
            }
            std::scoped_lock lock(fetchedMutex);
            for (const auto& [id, entry] : local.byId) {
                AddCatalogEntry(fetchedCoordinates, entry);
            }
        }
    };
    if (metadataBatchCount > 0) {
        std::vector<std::jthread> workers;
        const std::size_t workerCount = std::min<std::size_t>(
            6, metadataBatchCount);
        workers.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers.emplace_back(metadataWorker);
        }
    }
    if (stopToken.stop_requested()) {
        SourceResult cancelled;
        cancelled.error = L"查询已取消。";
        return cancelled;
    }
    if (failedMetadataBatches.load() > 0) {
        SourceResult failure;
        failure.networkTransportFailure = metadataTransportCode.load() != 0;
        failure.transportErrorCode = metadataTransportCode.load();
        failure.error = std::format(
            L"{}：{} 个官方测站坐标批次读取失败；为避免写入不完整地图记录，本批次未入库。",
            displayLabel,
            failedMetadataBatches.load());
        return failure;
    }

    FinishCatalog(fetchedCoordinates);
    {
        std::scoped_lock lock(cache->mutex);
        for (const auto& [id, entry] : fetchedCoordinates.byId) {
            AddCatalogEntry(cache->coordinates, entry);
        }
        FinishCatalog(cache->coordinates);
        cachedCoordinates = cache->coordinates;
    }

    for (auto& dayResult : batchResults) {
        for (auto& station : dayResult.stations) {
            ApplyCoordinates(station, archiveCatalog);
            if (!station.hasCoordinate) {
                ApplyCoordinates(station, cachedCoordinates);
            }
            if (!station.hasCoordinate) {
                ApplyCoordinates(station, igsCatalog);
            }
            CanonicalizeWithIgs(station, igsCatalog);
        }
        if (!countryFilter.empty()) {
            std::erase_if(dayResult.stations, [&](const StationFile& station) {
                return station.country != countryFilter;
            });
        }
        dayResult.files = dayResult.stations;
        dayResult.emptyResultIsComplete = dayResult.stations.empty();
    }

    SourceResult requestedResult = batchResults[batchIndex.at(requestedKey)];
    {
        std::scoped_lock lock(cache->mutex);
        BatchCache::CachedBlock block;
        block.days.reserve(batchResults.size());
        block.lastUse = ++cache->useSerial;
        for (std::size_t index = 0; index < batchResults.size(); ++index) {
            block.days.emplace(
                dateKey(batchDates[index]),
                std::move(batchResults[index]));
        }
        cache->blocks.insert_or_assign(blockKey, std::move(block));
        requestedResult = cache->blocks.at(blockKey).days.at(requestedKey);

        // Two EPOS block workers need their current blocks simultaneously.
        // Retain a few recent blocks, but keep memory bounded during multi-year
        // backfills (one modern block can contain tens of thousands of routes).
        constexpr std::size_t maximumCachedBlocks = 4;
        while (cache->blocks.size() > maximumCachedBlocks) {
            auto oldest = cache->blocks.end();
            for (auto iterator = cache->blocks.begin();
                 iterator != cache->blocks.end(); ++iterator) {
                if (iterator->first == blockKey) continue;
                if (oldest == cache->blocks.end()
                    || iterator->second.lastUse < oldest->second.lastUse) {
                    oldest = iterator;
                }
            }
            if (oldest == cache->blocks.end()) break;
            cache->blocks.erase(oldest);
        }
    }
    return requestedResult;
}

SourceResult QueryEposGnss(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    return QueryEposGateway(
        date, archiveCatalog, igsCatalog, stopToken,
        ObservationNetwork::EposGnss, L"EPOS-GNSS", {});
}

SourceResult QueryApos(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    // The BEV public data-centre routes are also exposed through the EPOS
    // GLASS range gateway.  A 14-day country-filtered request is equivalent
    // to many pairs of obs/obs_v3 daily listings and preserves each actual
    // download URL.  It also avoids stressing the BEV directory service.
    return QueryEposGateway(
        date, archiveCatalog, igsCatalog, stopToken,
        ObservationNetwork::Apos, L"APOS/BEV", {}, "AUT");
}

SourceResult QueryRobBelgium(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    // ROB's direct /doy endpoint is deliberately limited to 20 requests per
    // minute.  The same Belgian repository routes are published through the
    // EPOS GLASS range API.  Fetch one 14-day range and retain BEL stations,
    // preserving the actual gnss.be/ROB download URLs without violating the
    // per-user daily-API rate limit.
    return QueryEposGateway(
        date, archiveCatalog, igsCatalog, stopToken,
        ObservationNetwork::RobBelgium, L"ROB Belgium", {}, "BEL");
}

SourceResult QueryOsNet(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    return QueryEposGateway(
        date, archiveCatalog, igsCatalog, stopToken,
        ObservationNetwork::OsNet, L"OS Net", "OS Net");
}

SourceResult QueryErgnss(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    return QueryEposGateway(
        date, archiveCatalog, igsCatalog, stopToken,
        ObservationNetwork::Ergnss, L"ERGNSS", "ERGNSS");
}

SourceResult QueryRenep(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    return QueryEposGateway(
        date, archiveCatalog, igsCatalog, stopToken,
        ObservationNetwork::Renep, L"RENEP", "ReNEP");
}

bool IsMissingArchiveDirectory(const HttpTextResult& response) {
    // curl 9 means that the FTP server is reachable but the requested remote
    // directory does not exist.  curl 78 is the analogous remote-file case.
    return response.statusCode == 404
        || response.transportErrorCode == 9
        || response.transportErrorCode == 78;
}

SourceResult QueryKasiNgii(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring suffix = std::format(
        L"{}/{}/{:02}d/",
        date.year,
        FormatDoy(date.dayOfYear),
        date.year % 100);
    const std::array<std::wstring, 2> roots{
        L"ftp://gnss-ftp.kasi.re.kr/kasinet/daily/",
        L"ftp://gnss-ftp.kasi.re.kr/kvn/daily/"
    };
    bool anyDirectory = false;
    for (std::size_t index = 0; index < roots.size(); ++index) {
        std::wstring baseUrl = roots[index] + suffix;
        // KASI's FTP daemon commonly rejects the second simultaneous control
        // connection from one address.  Two short sequential NLST requests
        // are faster than triggering proxy recovery for a false outage.
        const auto response = HttpListFtpDirectory(
            baseUrl, stopToken, 4U * 1024U * 1024U);
        if (!response.ok() && !IsMissingArchiveDirectory(response)) {
            RecordTransportFailure(result, response);
            result.error = L"KASI：无法完整读取 KASINet/KVN 匿名 FTP 日目录"
                + (response.error.empty()
                    ? std::wstring{}
                    : L"（" + response.error + L"）");
            return result;
        }
        std::vector<FileCandidate> candidates;
        if (response.ok()) {
            anyDirectory = true;
            candidates = ParseDailyObservationFiles(
                BuildSyntheticDirectoryHtml(
                    ExtractLineFileNames(response.body)), date);
        }
        if (candidates.empty() && !stopToken.stop_requested()) {
            // The 1995 onward legacy archive stores .yyO.Z/.yyD.Z directly
            // in YYYY/DDD/, before the newer yyD subdirectory layout.
            const std::wstring legacyUrl = BuildDirectoryUrl(roots[index], date);
            const auto legacyResponse = HttpListFtpDirectory(
                legacyUrl, stopToken, 4U * 1024U * 1024U);
            if (!legacyResponse.ok()
                && !IsMissingArchiveDirectory(legacyResponse)) {
                RecordTransportFailure(result, legacyResponse);
                result.error = L"KASI：无法读取 KASINet/KVN 历史日目录"
                    + (legacyResponse.error.empty() ? std::wstring{}
                        : L"（" + legacyResponse.error + L"）");
                return result;
            }
            if (legacyResponse.ok()) {
                anyDirectory = true;
                baseUrl = legacyUrl;
                const auto legacyNames = ExtractLineFileNames(legacyResponse.body);
                candidates = ParseDailyObservationFiles(
                    BuildSyntheticDirectoryHtml(legacyNames), date);
                const std::string observationFolder =
                    std::format("{:02}o", date.year % 100);
                if (candidates.empty() && !stopToken.stop_requested()
                    && std::find(legacyNames.begin(), legacyNames.end(),
                        observationFolder) != legacyNames.end()) {
                    // From 2002 the uncompressed-observation namespace uses
                    // yyO/; its files can still be Unix-compress .yyO.Z.
                    const std::wstring observationUrl = legacyUrl
                        + Utf8ToWide(observationFolder) + L"/";
                    const auto observationResponse = HttpListFtpDirectory(
                        observationUrl, stopToken, 4U * 1024U * 1024U);
                    if (!observationResponse.ok()
                        && !IsMissingArchiveDirectory(observationResponse)) {
                        RecordTransportFailure(result, observationResponse);
                        result.error = L"KASI：无法读取历史观测 yyO 目录"
                            + (observationResponse.error.empty() ? std::wstring{}
                                : L"（" + observationResponse.error + L"）");
                        return result;
                    }
                    if (observationResponse.ok()) {
                        baseUrl = observationUrl;
                        candidates = ParseDailyObservationFiles(
                            BuildSyntheticDirectoryHtml(
                                ExtractLineFileNames(observationResponse.body)), date);
                    }
                }
            }
        }
        if (candidates.empty()) continue;
        auto stations = ResolveArchiveCandidates(
            candidates,
            archiveCatalog,
            igsCatalog,
            baseUrl,
            ObservationNetwork::KasiNgii,
            &result.files);
        result.stations.insert(
            result.stations.end(),
            std::make_move_iterator(stations.begin()),
            std::make_move_iterator(stations.end()));
    }
    CompleteNationalArchiveStations(
        result, "KOR", archiveCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    if (!anyDirectory) {
        result.emptyResultIsComplete = true;
    }
    // NGII GNSSData makes anonymous downloads by generating a requested ZIP,
    // but exposes no complete per-day file inventory.  Never manufacture
    // availability rows from its station list.  Emit this scope note once.
    static std::atomic_bool scopeNoticeEmitted{false};
    if (!scopeNoticeEmitted.exchange(true)) {
        result.warning =
            L"KASI-NGII：知识库只写入 KASI 匿名目录中实际枚举到的文件；"
            L"NGII GNSSData 的动态 ZIP 门户没有完整日清单，未写入未经验证的站日。";
    }
    return result;
}

SourceResult QueryRenag(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::array<std::wstring, 2> baseUrls{
        std::format(
            L"https://renag.resif.fr/pub/data/{}/{}/",
            date.year, FormatDoy(date.dayOfYear)),
        std::format(
            L"https://renag.resif.fr/pub/rinex3/{}/{}/",
            date.year, FormatDoy(date.dayOfYear))
    };
    std::array<std::future<HttpTextResult>, 2> tasks;
    for (std::size_t index = 0; index < baseUrls.size(); ++index) {
        const auto url = baseUrls[index];
        tasks[index] = std::async(
            std::launch::async,
            [url, stopToken]() {
                return HttpGetText(
                    url, stopToken, 16U * 1024U * 1024U);
            });
    }
    for (std::size_t index = 0; index < baseUrls.size(); ++index) {
        const auto response = tasks[index].get();
        if (!response.ok()) {
            if (IsMissingArchiveDirectory(response)) {
                continue;
            }
            RecordTransportFailure(result, response);
            result.error = L"RENAG：无法完整读取官方 RINEX 2/3 日目录"
                + (response.error.empty()
                    ? std::wstring{}
                    : L"（" + response.error + L"）");
            return result;
        }
        auto stations = ResolveArchiveCandidates(
            ParseDailyObservationFiles(response.body, date),
            archiveCatalog,
            igsCatalog,
            baseUrls[index],
            ObservationNetwork::Renag,
            &result.files);
        result.stations.insert(
            result.stations.end(),
            std::make_move_iterator(stations.begin()),
            std::make_move_iterator(stations.end()));
    }
    CompleteNationalArchiveStations(
        result, "FRA", archiveCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QueryNsgi(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = std::format(
        L"https://gnss-data.kadaster.nl/data/daily/{}/{}/",
        date.year, FormatDoy(date.dayOfYear));
    const auto response = HttpGetText(
        baseUrl, stopToken, 16U * 1024U * 1024U);
    if (!response.ok()) {
        if (IsMissingArchiveDirectory(response)) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error = L"NSGI：无法读取荷兰 Kadaster 官方日目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }

    static std::mutex catalogMutex;
    static Catalog officialCatalog;
    static bool catalogAttempted = false;
    {
        std::scoped_lock lock(catalogMutex);
        if (!catalogAttempted) {
            catalogAttempted = true;
            const auto metadata = HttpGetText(
                L"https://gnss-data.kadaster.nl/info/current_metadata.html",
                stopToken,
                8U * 1024U * 1024U);
            if (metadata.ok()) {
                officialCatalog = ParseHtmlLatitudeLongitudeCatalog(
                    metadata.body, "NLD", true);
            }
        }
    }
    const Catalog nsgiCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});
    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        nsgiCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::Nsgi,
        &result.files);
    CompleteNationalArchiveStations(
        result, "NLD", nsgiCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QueryNoaGnss(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = std::format(
        L"https://www.gein.noa.gr/services/GPSData/{}/{}/",
        date.year, FormatDoy(date.dayOfYear));
    const auto response = HttpGetText(
        baseUrl, stopToken, 16U * 1024U * 1024U);
    if (!response.ok()) {
        if (IsMissingArchiveDirectory(response)) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error = L"NOA GNSS：无法读取希腊 GEIN 官方日目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        archiveCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::NoaGnss,
        &result.files);
    CompleteNationalArchiveStations(
        result, "GRC", archiveCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    return result;
}

SourceResult QueryRedgae(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    // REDGAE is a federation and has no single complete public directory.
    // EPOS GLASS provides the verifiable public routes contributed by Spanish
    // member networks.  Country filtering avoids pretending that a single
    // ERGNSS mirror represents the whole federation.
    return QueryEposGateway(
        date,
        archiveCatalog,
        igsCatalog,
        stopToken,
        ObservationNetwork::Redgae,
        L"REDGAE",
        {},
        "ESP");
}

// TU Delft's GNSS Fieldlab is a small, deliberately stable archive.  Its
// coordinate page identifies the nine continuously operating receivers and
// the daily directory contains RINEX 3 observation/navigation files.
Catalog DlabCatalog() {
    Catalog catalog;
    const auto add = [&](const char* code, const double latitude,
                         const double longitude) {
        AddCatalogEntry(
            catalog,
            {std::string(code) + "00NLD", "NLD", latitude, longitude});
    };
    add("TUDB", 52.0053516684, 4.3712616894);
    add("TUDE", 51.9977659634, 4.3746754388);
    add("TUDI", 52.0009689609, 4.3693991301);
    add("TUDL", 51.9885288819, 4.3765761952);
    add("TUDN", 52.0002316067, 4.3745065323);
    add("TUDP", 51.9965641232, 4.3795342778);
    add("TUDR", 51.9918316231, 4.3805394153);
    add("TUDS", 51.9981814196, 4.3773260603);
    add("TUDX", 51.9955469525, 4.3750371416);
    FinishCatalog(catalog);
    return catalog;
}

Catalog LoadMagnetCatalog(const std::stop_token stopToken) {
    static std::mutex catalogMutex;
    static Catalog catalog;
    static bool attempted = false;
    std::scoped_lock lock(catalogMutex);
    if (!attempted) {
        attempted = true;
        const auto response = HttpGetText(
            L"https://geodesy.unr.edu/magnet/Table1web.html",
            stopToken,
            8U * 1024U * 1024U);
        if (response.ok()) {
            catalog = ParseHtmlLatitudeLongitudeCatalog(
                response.body, "USA", false);
        }
    }
    return catalog;
}

SourceResult QueryDlab(
    const QueryDate& date,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = BuildDirectoryUrl(
        L"https://gnss1.tudelft.nl/dlab/data/", date);
    const auto response = HttpGetText(
        baseUrl, stopToken, 8U * 1024U * 1024U);
    if (!response.ok()) {
        if (IsMissingArchiveDirectory(response)) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error = L"TU Delft DLAB：无法读取官方日目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    const auto catalog = DlabCatalog();
    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        catalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::Dlab,
        &result.files);
    CompleteNationalArchiveStations(
        result, "NLD", catalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    if (result.stations.empty() && !ExtractHrefs(response.body).empty()) {
        result.warning =
            L"TU Delft DLAB：官方目录可读，但当天没有可识别的RINEX观测文件。";
    }
    return result;
}

SourceResult QueryMagnet(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    const std::wstring baseUrl = BuildDirectoryUrl(
        L"https://geodesy.unr.edu/magnet/rinex/", date);
    const auto response = HttpGetText(
        baseUrl, stopToken, 32U * 1024U * 1024U);
    if (!response.ok()) {
        if (IsMissingArchiveDirectory(response)) {
            result.emptyResultIsComplete = true;
            return result;
        }
        RecordTransportFailure(result, response);
        result.error = L"Nevada MAGNET：无法读取官方日目录"
            + (response.error.empty()
                ? std::wstring{}
                : L"（" + response.error + L"）");
        return result;
    }
    const auto officialCatalog = LoadMagnetCatalog(stopToken);
    const auto magnetCatalog = MergeCatalogs(
        {&archiveCatalog, &officialCatalog});
    result.stations = ResolveArchiveCandidates(
        ParseDailyObservationFiles(response.body, date),
        magnetCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::Magnet,
        &result.files);
    CompleteNationalArchiveStations(
        result, "USA", magnetCatalog, igsCatalog);
    result.emptyResultIsComplete = result.stations.empty();
    if (result.stations.empty() && !ExtractHrefs(response.body).empty()) {
        result.warning =
            L"Nevada MAGNET：官方目录可读，但当天没有可识别的RINEX观测文件。";
    }
    return result;
}

SourceResult QueryPangaCwu(
    const QueryDate& date,
    const Catalog& archiveCatalog,
    const Catalog& igsCatalog,
    const std::stop_token stopToken) {
    SourceResult result;
    // PANGA's current public archive is linked from panga.org.  Keep the
    // historic CWU/geodesy aliases as fallbacks because older dates and
    // mirrors may still be served there.
    const std::array<std::wstring, 4> roots{
        L"https://www.panga.org/pub/data/",
        L"https://panga.org/pub/data/",
        L"https://www.geodesy.org/pub/data/",
        L"https://www.panga.cwu.edu/data_ftp_pub/data/"
    };
    std::wstring baseUrl;
    HttpTextResult response;
    std::vector<FileCandidate> parsedCandidates;
    std::wstring firstSuccessfulUrl;
    HttpTextResult firstSuccessfulResponse;
    bool haveSuccessfulResponse = false;
    std::wstring lastError;
    bool allMissing = true;
    for (const auto& root : roots) {
        const auto candidateUrl = BuildDirectoryUrl(root, date);
        const auto candidate = HttpGetText(
            candidateUrl, stopToken, 32U * 1024U * 1024U);
        if (candidate.ok()) {
            if (!haveSuccessfulResponse) {
                firstSuccessfulUrl = candidateUrl;
                firstSuccessfulResponse = candidate;
                haveSuccessfulResponse = true;
            }
            auto candidates = ParseDailyObservationFiles(candidate.body, date);
            if (!candidates.empty()) {
                // A fallback root produced a valid archive response, so an
                // earlier failed alias must not make this successful date
                // look like a transport failure to the network watchdog.
                result.networkTransportFailure = false;
                result.transportErrorCode = 0;
                baseUrl = candidateUrl;
                response = candidate;
                parsedCandidates = std::move(candidates);
                break;
            }
            // The current PANGA layout places files below interval-specific
            // directories (30sec/, 10sec/, rinex3/, ...).  The date-level
            // listing itself therefore normally contains no files.
            struct Subdirectory {
                const char* name;
                const char* interval;
            };
            constexpr std::array<Subdirectory, 6> subdirectories{{
                {"01sec", "01S"},
                {"05sec", "05S"},
                {"10sec", "10S"},
                {"15sec", "15S"},
                {"30sec", "30S"},
                {"rinex3", "30S"}
            }};
            std::unordered_set<std::string> advertised;
            for (auto href : ExtractHrefs(candidate.body)) {
                while (!href.empty() && href.back() == '/') href.pop_back();
                if (std::find_if(
                        std::begin(subdirectories),
                        std::end(subdirectories),
                        [&](const Subdirectory& directory) {
                            return ToLowerAscii(href) == directory.name;
                        }) != std::end(subdirectories)) {
                    advertised.insert(ToLowerAscii(std::move(href)));
                }
            }
            if (!advertised.empty()) {
                for (const auto& directory : subdirectories) {
                    if (!advertised.contains(directory.name)) continue;
                    const auto childUrl = candidateUrl
                        + Utf8ToWide(directory.name) + L"/";
                    const auto child = HttpGetText(
                        childUrl, stopToken, 32U * 1024U * 1024U);
                    if (!child.ok()) {
                        if (!IsMissingArchiveDirectory(child)) {
                            allMissing = false;
                            lastError = child.error;
                            if (IsNetworkTransportFailure(child)) {
                                result.networkTransportFailure = true;
                                if (result.transportErrorCode == 0) {
                                    result.transportErrorCode =
                                        child.transportErrorCode;
                                }
                            }
                        }
                        continue;
                    }
                    auto nested = ParseDailyObservationFiles(
                        child.body, date);
                    for (auto& nestedCandidate : nested) {
                        if (nestedCandidate.format == RinexFormat::Rinex2) {
                            nestedCandidate.interval = directory.interval;
                        }
                        nestedCandidate.downloadPath =
                            std::string(directory.name) + "/"
                            + nestedCandidate.fileName;
                        parsedCandidates.push_back(
                            std::move(nestedCandidate));
                    }
                }
                if (!parsedCandidates.empty()) {
                    result.networkTransportFailure = false;
                    result.transportErrorCode = 0;
                    baseUrl = candidateUrl;
                    response = candidate;
                    break;
                }
            }
            // A stale alias can return a generic HTML page with HTTP 200.
            // Continue to the next root before treating it as an empty day.
            continue;
        }
        if (!IsMissingArchiveDirectory(candidate)) allMissing = false;
        lastError = candidate.error;
        if (IsNetworkTransportFailure(candidate)) {
            result.networkTransportFailure = true;
            if (result.transportErrorCode == 0) {
                result.transportErrorCode = candidate.transportErrorCode;
            }
        }
    }
    if (baseUrl.empty() && haveSuccessfulResponse) {
        baseUrl = std::move(firstSuccessfulUrl);
        response = std::move(firstSuccessfulResponse);
    }
    if (baseUrl.empty()) {
        if (allMissing) {
            result.emptyResultIsComplete = true;
            return result;
        }
        if (lastError.empty()) {
            result.emptyResultIsComplete = true;
            return result;
        }
        result.error = L"PANGA/CWU：无法读取官方公开日目录（"
            + lastError + L"）";
        return result;
    }
    const auto pangaCatalog = MergeCatalogs({&archiveCatalog});
    result.stations = ResolveArchiveCandidates(
        parsedCandidates,
        pangaCatalog,
        igsCatalog,
        baseUrl,
        ObservationNetwork::PangaCwu,
        &result.files);
    // PANGA's public station page is a cooperative-station list rather than
    // a complete coordinate catalogue. Keep every file even when a station
    // has no coordinate; fill known IGS/archive coordinates when available.
    const auto finish = [&](StationFile& station) {
        if (!station.hasCoordinate) ApplyCoordinates(station, pangaCatalog);
        if (!station.hasCoordinate) ApplyCoordinates(station, igsCatalog);
        if (station.country.empty() && station.stationId.size() == 9) {
            station.country = station.stationId.substr(6, 3);
        }
    };
    for (auto& station : result.stations) finish(station);
    for (auto& file : result.files) finish(file);
    result.emptyResultIsComplete = result.stations.empty();
    if (result.stations.empty() && !ExtractHrefs(response.body).empty()) {
        result.warning =
            L"PANGA/CWU：官方目录可读，但当天没有可识别的RINEX观测文件。";
    }
    return result;
}

int DownloadPreference(const StationFile& station) {
    const std::wstring& url = station.downloadUrl;
    int score = station.format == RinexFormat::Rinex34 ? 20 : 10;
    if (url.starts_with(L"https://")) {
        score += 25;
    }
    if (url.find(L"/root_ftp/IGS/") != std::wstring::npos) {
        score += 400;
    } else if (url.find(L"/root_ftp/EUREF/") != std::wstring::npos) {
        score += 300;
    } else if (url.find(L"geodesy.noaa.gov") != std::wstring::npos
               || url.find(L"noaa-cors-pds.s3.amazonaws.com")
                   != std::wstring::npos) {
        score += 200;
    } else if (url.find(L"rinex.geodetic.gov.hk") != std::wstring::npos) {
        score += 100;
    } else if (url.find(L"ga-gnss-data-rinex-v1") != std::wstring::npos) {
        score += 90;
    } else if (url.find(L"ftp.sonel.org") != std::wstring::npos) {
        score += 80;
    } else if (url.find(L"inegi.org.mx") != std::wstring::npos) {
        score += 70;
    } else if (url.find(L"geoftp.ibge.gov.br") != std::wstring::npos) {
        score += 68;
    } else if (url.find(L"ramsac.ign.gob.ar") != std::wstring::npos) {
        score += 66;
    } else if (url.find(L"pp.igm.gub.uy") != std::wstring::npos) {
        score += 64;
    } else if (url.find(L"rgpdata.ensg.eu") != std::wstring::npos) {
        score += 62;
    } else if (url.find(L"ftp.trignet.co.za") != std::wstring::npos) {
        score += 60;
    } else if (url.find(L"garner.ucsd.edu") != std::wstring::npos) {
        score += 58;
    } else if (url.find(L"data.epos-iceland.is") != std::wstring::npos) {
        score += 55;
    } else if (url.find(L"gps.csn.uchile.cl") != std::wstring::npos) {
        score += 50;
    } else if (url.find(L"cacsa.nrcan.gc.ca") != std::wstring::npos) {
        score += 49;
    } else if (url.find(L"srvstorage.igac.gov.co") != std::wstring::npos) {
        score += 48;
    } else if (url.find(L"cors.anati.gob.pa") != std::wstring::npos) {
        score += 47;
    } else if (url.find(L"www.ign.hn") != std::wstring::npos) {
        score += 46;
    } else if (url.find(L"saegnss2.curtin.edu.au") != std::wstring::npos) {
        score += 45;
    } else if (url.find(L"gnss1.tudelft.nl/dlab") != std::wstring::npos) {
        score += 44;
    } else if (url.find(L"geodesy.unr.edu/magnet") != std::wstring::npos) {
        score += 43;
    } else if (url.find(L"geodesy.org/pub/data") != std::wstring::npos
               || url.find(L"panga.cwu.edu/data_ftp_pub") != std::wstring::npos) {
        score += 42;
    }
    return score;
}

bool IsShortStationIdentifier(const std::string& stationId) {
    return !stationId.empty() && stationId.size() <= 4U;
}

bool HasReliableStationCoordinate(const StationFile& station) {
    return station.hasCoordinate
        && std::isfinite(station.latitude)
        && std::isfinite(station.longitude)
        && station.latitude >= -90.0 && station.latitude <= 90.0
        && station.longitude >= -180.0 && station.longitude <= 180.0
        && !(std::abs(station.latitude) < 1.0e-12
            && std::abs(station.longitude) < 1.0e-12);
}

std::string StationFourCharacterCode(const StationFile& station) {
    std::string value = ToUpperAscii(station.fourCharId);
    if (value.empty() && station.stationId.size() >= 4U) {
        value = ToUpperAscii(station.stationId.substr(0, 4));
    }
    if (value.size() > 4U) value.resize(4U);
    return value;
}

bool IsExplicitPhysicalStationAlias(
    const StationFile& left,
    const StationFile& right) {
    if (StationFourCharacterCode(left) != "STAR"
        || StationFourCharacterCode(right) != "STAR") {
        return false;
    }
    const auto leftSopac =
        left.availableNetworks == NetworkBit(ObservationNetwork::SopacGarner);
    const auto rightSopac =
        right.availableNetworks == NetworkBit(ObservationNetwork::SopacGarner);
    const auto leftPanga =
        left.availableNetworks == NetworkBit(ObservationNetwork::PangaCwu);
    const auto rightPanga =
        right.availableNetworks == NetworkBit(ObservationNetwork::PangaCwu);
    return (leftSopac && rightPanga) || (leftPanga && rightSopac);
}

// Four-character station codes are reused by independent archives.  Keep
// this predicate in the legacy/date-range hydration path in lock-step with
// ObservationSearchDb's central-index union rule: an exact short code may be
// merged across networks only when the rows are demonstrably co-located (or a
// deliberately explicit alias says they are the same monument).  Full
// nine-character IDs retain their global exact-ID semantics.
bool SamePhysicalStation(const StationFile& left, const StationFile& right) {
    const std::string leftId = ToUpperAscii(left.stationId);
    const std::string rightId = ToUpperAscii(right.stationId);
    const bool leftShort = leftId.empty() || IsShortStationIdentifier(leftId);
    const bool rightShort = rightId.empty() || IsShortStationIdentifier(rightId);
    const bool shortIdentifier = leftShort || rightShort;
    const bool networksKnown = left.availableNetworks != 0
        && right.availableNetworks != 0;
    const bool sameNetwork = networksKnown
        && (left.availableNetworks & right.availableNetworks) != 0;

    if (!leftId.empty() && leftId == rightId) {
        // A short code is safe without coordinates only inside one archive
        // network.  If either row has no network mask, retain the historical
        // same-source behavior only when both masks are absent; otherwise the
        // unknown cross-network case must be treated conservatively.
        if (!shortIdentifier || sameNetwork
            || (!networksKnown
                && left.availableNetworks == 0
                && right.availableNetworks == 0)) {
            return true;
        }
    }

    const std::string leftFour = StationFourCharacterCode(left);
    const std::string rightFour = StationFourCharacterCode(right);
    if (leftFour.empty() || leftFour != rightFour) return false;

    // Keep the established coordinate-based alias behavior inside one
    // archive/network even when a source changes a station's suffix or emits
    // a full-vs-short ID on different days.  The conservative short-code
    // rule below is specifically for cross-network collisions.
    if (sameNetwork && HasReliableStationCoordinate(left)
        && HasReliableStationCoordinate(right)
        && DistanceKm(
            left.latitude, left.longitude,
            right.latitude, right.longitude) <= 3.0) {
        return true;
    }

    // Full IDs which differ are intentionally never collapsed merely because
    // their first four characters and coordinates happen to match.  Distinct
    // full IDs can legitimately refer to co-located monuments.
    if (!shortIdentifier) return false;
    if (IsExplicitPhysicalStationAlias(left, right)) return true;
    if (!HasReliableStationCoordinate(left)
        || !HasReliableStationCoordinate(right)) {
        return false;
    }
    return DistanceKm(
        left.latitude, left.longitude,
        right.latitude, right.longitude) <= 3.0;
}

void MergeStation(
    std::vector<StationFile>& stations,
    std::unordered_map<std::string, std::vector<std::size_t>>& byFourChar,
    StationFile station) {
    auto& candidates = byFourChar[ToUpperAscii(station.fourCharId)];
    for (const auto index : candidates) {
        if (!SamePhysicalStation(stations[index], station)) {
            continue;
        }
        auto& existing = stations[index];
        const auto memberships = static_cast<ObservationNetworkMask>(
            existing.availableNetworks | station.availableNetworks);
        const auto formats = static_cast<RinexFormatMask>(
            EffectiveRinexFormatMask(existing)
            | EffectiveRinexFormatMask(station));
        if (DownloadPreference(station) > DownloadPreference(existing)) {
            const bool selected = existing.selected;
            existing = std::move(station);
            existing.selected = selected;
        }
        existing.availableNetworks = memberships;
        existing.availableRinexFormats = formats;
        return;
    }
    candidates.push_back(stations.size());
    stations.push_back(std::move(station));
}

void AppendSourceError(
    std::wstring& warning,
    const std::wstring& sourceError) {
    if (sourceError.empty()) {
        return;
    }
    if (!warning.empty()) {
        warning += L"；";
    }
    warning += sourceError;
}

// ---- Offline date-range query helpers ---------------------------------

std::string NormalizeRangeFilterToken(std::string value) {
    value = ToUpperAscii(TrimAscii(std::move(value)));
    value.erase(
        std::remove_if(
            value.begin(),
            value.end(),
            [](const unsigned char character) {
                return std::isspace(character) != 0;
            }),
        value.end());
    if (value == "ANY" || value == "*" || value == "ALL") {
        return {};
    }
    // Accept the forms commonly used in the GUI and in station metadata.
    if (!value.empty()
        && std::all_of(
            value.begin(), value.end(),
            [](const unsigned char character) {
                return std::isdigit(character) != 0;
            })) {
        value += 'S';
    }
    if (value.ends_with("SEC")) {
        value.resize(value.size() - 3);
        value += 'S';
    }
    if (value.ends_with("HZ")) {
        value.resize(value.size() - 2);
        value += 'S';
    }
    // Observation indexes historically used a two-character seconds field
    // (01S/05S/15S/30S/60S), while the GUI deliberately shows the friendlier
    // 1S/5S labels.  Canonicalise the numeric seconds portion so either form
    // matches the same database rows.  Leave non-seconds intervals (05M,
    // 01H, 01D, etc.) untouched.
    if (value.ends_with('S') && value.size() > 1) {
        std::string digits = value.substr(0, value.size() - 1);
        if (!digits.empty()
            && std::all_of(
                digits.begin(), digits.end(),
                [](const unsigned char character) {
                    return std::isdigit(character) != 0;
                })) {
            const auto firstSignificant = digits.find_first_not_of('0');
            if (firstSignificant == std::string::npos) {
                digits = "0";
            } else {
                digits.erase(0, firstSignificant);
            }
            if (digits.size() == 1) digits.insert(digits.begin(), '0');
            value = digits + 'S';
        }
    }
    return value;
}

std::vector<std::string> SplitRangeFilterList(const std::string& text) {
    std::vector<std::string> values;
    std::string token;
    const auto flush = [&]() {
        auto normalized = NormalizeRangeFilterToken(token);
        token.clear();
        if (!normalized.empty()) values.push_back(std::move(normalized));
    };
    for (const char character : text) {
        if (character == ',' || character == ';' || character == '|'
            || character == '\n' || character == '\r') {
            flush();
        } else {
            token.push_back(character);
        }
    }
    flush();
    return values;
}

bool RangeFieldMatches(
    const std::string& value,
    const std::string& includeText,
    const std::string& excludeText) {
    const auto include = SplitRangeFilterList(includeText);
    const auto exclude = SplitRangeFilterList(excludeText);
    const std::string normalized = ToUpperAscii(value);
    if (!include.empty()) {
        const bool allIncluded = std::all_of(
            include.begin(), include.end(),
            [&](const std::string& token) {
                return normalized.find(token) != std::string::npos;
            });
        if (!allIncluded) return false;
    }
    if (std::any_of(
            exclude.begin(), exclude.end(),
            [&](const std::string& token) {
                return normalized.find(token) != std::string::npos;
            })) {
        return false;
    }
    return true;
}

std::string EffectiveRangeField(
    const std::string& primary,
    const std::string& compatibility);

// A date-range query can inspect millions of daily file rows.  Splitting and
// normalising the same six filter strings for every row used to allocate a
// number of temporary vectors/strings per record.  Prepare the predicates
// once per query instead, then use the allocation-free (apart from the
// station's existing strings) matcher below.
struct PreparedRangeFilters {
    int rinexVersion = 0;
    bool includeRinex2 = true;
    bool includeRinex34 = true;
    std::vector<std::string> intervals;
    std::vector<std::string> systemsInclude;
    std::vector<std::string> systemsExclude;
    std::vector<std::string> receiverInclude;
    std::vector<std::string> receiverExclude;
    std::vector<std::string> antennaInclude;
    std::vector<std::string> antennaExclude;

    bool hasSiteLogCriteria = false;
};

PreparedRangeFilters PrepareRangeFilters(
    const ObservationRangeOptions& options) {
    PreparedRangeFilters prepared;
    prepared.rinexVersion = options.rinexVersion;
    prepared.includeRinex2 = options.includeRinex2;
    prepared.includeRinex34 = options.includeRinex34;
    if (!options.sampleIntervals.empty()) {
        for (const auto& value : options.sampleIntervals) {
            const auto normalized = NormalizeRangeFilterToken(value);
            if (!normalized.empty()) prepared.intervals.push_back(normalized);
        }
    } else if (!options.sampleInterval.empty()) {
        const auto normalized = NormalizeRangeFilterToken(options.sampleInterval);
        if (!normalized.empty()) prepared.intervals.push_back(normalized);
    }

    const auto prepareField = [](const std::string& primary,
                                 const std::string& compatibility) {
        return SplitRangeFilterList(EffectiveRangeField(primary, compatibility));
    };
    prepared.systemsInclude = prepareField(
        options.systemsInclude, options.includeSystems);
    prepared.systemsExclude = prepareField(
        options.systemsExclude, options.excludeSystems);
    prepared.receiverInclude = prepareField(
        options.receiverInclude, options.includeReceiver);
    prepared.receiverExclude = prepareField(
        options.receiverExclude, options.excludeReceiver);
    prepared.antennaInclude = prepareField(
        options.antennaInclude, options.includeAntenna);
    prepared.antennaExclude = prepareField(
        options.antennaExclude, options.excludeAntenna);
    prepared.hasSiteLogCriteria = !prepared.systemsInclude.empty()
        || !prepared.systemsExclude.empty()
        || !prepared.receiverInclude.empty()
        || !prepared.receiverExclude.empty()
        || !prepared.antennaInclude.empty()
        || !prepared.antennaExclude.empty();
    return prepared;
}

bool PreparedRangeFieldMatches(
    const std::string& value,
    const std::vector<std::string>& include,
    const std::vector<std::string>& exclude) {
    const std::string normalized = ToUpperAscii(value);
    if (!std::all_of(
            include.begin(), include.end(),
            [&](const std::string& token) {
                return normalized.find(token) != std::string::npos;
            })) {
        return false;
    }
    return std::none_of(
        exclude.begin(), exclude.end(),
        [&](const std::string& token) {
            return normalized.find(token) != std::string::npos;
        });
}

bool MatchesPreparedRangeOptions(
    const StationFile& station,
    const PreparedRangeFilters& options) {
    if (options.rinexVersion != 0) {
        if (options.rinexVersion == 2
            && station.format != RinexFormat::Rinex2) {
            return false;
        }
        if ((options.rinexVersion == 3 || options.rinexVersion == 4)
            && station.format != RinexFormat::Rinex34) {
            return false;
        }
        if (options.rinexVersion != 2 && options.rinexVersion != 3
            && options.rinexVersion != 4) {
            return false;
        }
    } else if ((station.format == RinexFormat::Rinex2 && !options.includeRinex2)
               || (station.format == RinexFormat::Rinex34
                   && !options.includeRinex34)) {
        return false;
    }

    if (!options.intervals.empty()) {
        const auto stationInterval = NormalizeRangeFilterToken(station.interval);
        if (stationInterval.empty()
            || std::none_of(
                options.intervals.begin(), options.intervals.end(),
                [&](const std::string& value) {
                    return value == stationInterval;
                })) {
            return false;
        }
    }

    // Site Log fields are applied by the temporal enrichment worker when the
    // query has metadata criteria.  If a caller supplies already-enriched
    // rows, honour the same predicates here as well.
    if (options.hasSiteLogCriteria && station.hasSiteLog
        && (!PreparedRangeFieldMatches(
                station.siteLogSystems,
                options.systemsInclude,
                options.systemsExclude)
            || !PreparedRangeFieldMatches(
                station.siteLogReceiver,
                options.receiverInclude,
                options.receiverExclude)
            || !PreparedRangeFieldMatches(
                station.siteLogAntenna,
                options.antennaInclude,
                options.antennaExclude))) {
        return false;
    }
    return true;
}

std::string EffectiveRangeField(
    const std::string& primary,
    const std::string& compatibility) {
    return primary.empty() ? compatibility : primary;
}

bool MatchesObservationRangeOptions(
    const StationFile& station,
    const ObservationRangeOptions& options) {
    return MatchesPreparedRangeOptions(
        station,
        PrepareRangeFilters(options));
}

constexpr std::array<ObservationNetwork, 42> kAllObservationNetworks{
    ObservationNetwork::Igs,
    ObservationNetwork::Mgex,
    ObservationNetwork::Ngs,
    ObservationNetwork::Epn,
    ObservationNetwork::Hk,
    ObservationNetwork::GaGdc,
    ObservationNetwork::Rgna,
    ObservationNetwork::Sonel,
    ObservationNetwork::TrigNet,
    ObservationNetwork::Cut,
    ObservationNetwork::Csn,
    ObservationNetwork::EposIceland,
    ObservationNetwork::Rbmc,
    ObservationNetwork::Ramsac,
    ObservationNetwork::RegnaRou,
    ObservationNetwork::Cacs,
    ObservationNetwork::Rgp,
    ObservationNetwork::SopacGarner,
    ObservationNetwork::MagnaEco,
    ObservationNetwork::PanamaCors,
    ObservationNetwork::HondurasCors,
    ObservationNetwork::EposGnss,
    ObservationNetwork::Gref,
    ObservationNetwork::OsNet,
    ObservationNetwork::Ergnss,
    ObservationNetwork::Renep,
    ObservationNetwork::GeoNetNz,
    ObservationNetwork::Ring,
    ObservationNetwork::Apos,
    ObservationNetwork::RobBelgium,
    ObservationNetwork::Dpga,
    ObservationNetwork::CatNet,
    ObservationNetwork::SpsLux,
    ObservationNetwork::KasiNgii,
    ObservationNetwork::Renag,
    ObservationNetwork::Nsgi,
    ObservationNetwork::NoaGnss,
    ObservationNetwork::Redgae,
    ObservationNetwork::GeoNetJapan,
    ObservationNetwork::Dlab,
    ObservationNetwork::Magnet,
    ObservationNetwork::PangaCwu
};

bool SupportsStrictLocalRangeQuery(
    const NetworkSelection selection,
    std::wstring& unsupportedLabel) {
    for (const auto network : kAllObservationNetworks) {
        if (!HasNetwork(selection.mask, network)) continue;
        // IGS/MGEX use the shared local index; all other supported entries
        // use their single-source SQLite index.  These four legacy adapters
        // still have an online-only QueryDaily implementation, so reject
        // them before the first date to honour the range query's local-only
        // contract.
        const bool onlineOnly = network == ObservationNetwork::HondurasCors;
        if (onlineOnly || (!HasSingleSourceKnowledgeBase(network)
                           && network != ObservationNetwork::Igs
                           && network != ObservationNetwork::Mgex)) {
            unsupportedLabel = KnowledgeBaseNetworkLabel(network);
            return false;
        }
    }
    // Reject unknown high bits rather than accidentally letting QueryDaily
    // select no source and report a misleading empty result.
    ObservationNetworkMask known = 0;
    for (const auto network : kAllObservationNetworks) known |= NetworkBit(network);
    if ((selection.mask & ~known) != 0) {
        unsupportedLabel = L"未知站网位";
        return false;
    }
    return true;
}

std::chrono::sys_days ToRangeSysDays(const QueryDate& date) {
    return std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
}

bool IsRangeDateValid(const QueryDate& date) {
    if (date.year < 1
        || date.month < 1 || date.month > 12
        || date.day < 1 || date.day > 31) {
        return false;
    }
    // Public callers sometimes construct QueryDate directly and leave the
    // cached day-of-year field at zero.  Validate from year/month/day rather
    // than rejecting an otherwise valid calendar date.
    if (DayOfYear(date.year, date.month, date.day) <= 0) {
        return false;
    }
    const auto normalized = AddDays(date, 0);
    return normalized.year == date.year
        && normalized.month == date.month
        && normalized.day == date.day;
}

SourceResult QuerySingleSourceKnowledgeBase(
    const QueryDate& date,
    const ObservationNetwork network,
    const bool includeAllFiles) {
    SourceResult result;
    const auto indexed = LoadNetworkAvailability(
        date, network, includeAllFiles);
    if (!indexed.error.empty()) {
        result.error = KnowledgeBaseNetworkLabel(network)
            + L"：读取本地知识库失败（" + indexed.error + L"）。";
        return result;
    }
    if (!indexed.complete) {
        result.error = KnowledgeBaseNetworkLabel(network)
            + L"：本地知识库尚未收录该日期；请调整查询日期范围。";
        return result;
    }
    result.stations = indexed.stations;
    // Keep the preferred station list for the legacy daily API, while
    // exposing every raw daily_files row separately to the date-range
    // engine.  Do not put the same rows in both vectors: the range engine
    // uses the raw view when it is available and would otherwise count a
    // preferred row twice for the same day.
    if (includeAllFiles) {
        result.files = indexed.allFiles;
    }
    result.fromLocalIndex = true;
    result.emptyResultIsComplete = indexed.stations.empty();
    if (indexed.provisional) {
        result.warning = KnowledgeBaseNetworkLabel(network)
            + L"：该日期属于最近7天暂定记录，构建器下次运行会重新扫描。";
    }
    return result;
}

}  // namespace

IgsHighrateBuildResult BuildIgsHighrateAvailability(
    const QueryDate& date,
    const std::stop_token stopToken) {
    return BuildIgsHighrateAvailabilityImpl(date, stopToken);
}

QueryResult QueryDailyObservationFiles(
    const QueryDate& date,
    const NetworkSelection selection,
    const std::stop_token stopToken,
    const KnowledgeBaseQueryMode mode,
    const bool includeAllFiles) {
    QueryResult result;
    result.date = date;
    result.selection = selection;
    result.directoryUrl =
        L"42 个 GNSS 公开目录、官方 API 与公开镜像";

    if (date.dayOfYear <= 0) {
        result.error = L"日期无效。";
        return result;
    }
    if (!selection.Any()) {
        result.error = L"请至少选择一个站网。";
        return result;
    }

    // Station catalogs are immutable embedded resources.  Parse them once
    // per process instead of rebuilding every catalog for every date query.
    static const auto igsCatalog = ParseCatalogJson(
        LoadEmbeddedResource(IDR_IGS_CATALOG));
    static const auto ngsCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_NGS_CATALOG));
    static const auto epnCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_EPN_CATALOG));
    static const auto hkCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_HK_CATALOG));
    static const auto gaCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_GA_GDC_CATALOG));
    static const auto rgnaCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_RGNA_CATALOG));
    static const auto sonelCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_SONEL_CATALOG));
    static const auto trigNetCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_TRIGNET_CATALOG));
    static const auto csnCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_CSN_CATALOG));
    static const auto regionalCatalog = ParseCoordinateTsv(
        LoadEmbeddedResource(IDR_SIRGAS_CATALOG));
    static const auto archiveCatalog = MergeCatalogs({
        &regionalCatalog,
        &ngsCatalog,
        &epnCatalog,
        &gaCatalog,
        &rgnaCatalog,
        &sonelCatalog,
        &csnCatalog,
        &igsCatalog
    });
    result.catalogSource =
        L"各机构官方/公开坐标目录；CACS、MAGNA-ECO、RAMSAC、RGP 与 "
        L"EPOS 坐标由官方 API 当次返回";

    std::vector<SourceResult> sources;
    if (HasNetwork(selection.mask, ObservationNetwork::Igs)
        || HasNetwork(selection.mask, ObservationNetwork::Mgex)) {
        sources.push_back(QueryIgsAndMgex(
            date,
            selection,
            igsCatalog,
            stopToken,
            mode,
            includeAllFiles));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Ngs)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Ngs, includeAllFiles)
            : QueryNgs(date, ngsCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Epn)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Epn, includeAllFiles)
            : QueryEpn(date, epnCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Hk)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Hk, includeAllFiles)
            : QueryHk(date, hkCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::GaGdc)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::GaGdc, includeAllFiles)
            : QueryGaGdc(date, gaCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Rgna)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Rgna, includeAllFiles)
            : QueryRgna(date, rgnaCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Sonel)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Sonel, includeAllFiles)
            : QuerySonel(date, sonelCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::TrigNet)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::TrigNet, includeAllFiles)
            : QueryTrigNet(
                date,
                trigNetCatalog,
                igsCatalog,
                stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Cut)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(date, ObservationNetwork::Cut, includeAllFiles)
            : QueryCut(date, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Csn)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Csn, includeAllFiles)
            : QueryCsn(date, csnCatalog, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::EposIceland)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::EposIceland, includeAllFiles)
            : QueryEposIceland(date, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Rbmc)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Rbmc, includeAllFiles)
            : QueryRbmc(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Ramsac)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Ramsac, includeAllFiles)
            : QueryRamsac(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::RegnaRou)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::RegnaRou, includeAllFiles)
            : QueryRegnaRou(
                date,
                archiveCatalog,
                igsCatalog,
                stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Cacs)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Cacs, includeAllFiles)
            : QueryCacs(
                date,
                archiveCatalog,
                igsCatalog,
                stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Rgp)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Rgp, includeAllFiles)
            : QueryRgp(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::SopacGarner)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::SopacGarner, includeAllFiles)
            : QuerySopacGarner(
                date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::MagnaEco)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::MagnaEco, includeAllFiles)
            : QueryMagnaEco(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Dlab)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Dlab, includeAllFiles)
            : QueryDlab(date, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Magnet)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Magnet, includeAllFiles)
            : QueryMagnet(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::PangaCwu)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::PangaCwu, includeAllFiles)
            : QueryPangaCwu(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::PanamaCors)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(date, ObservationNetwork::PanamaCors, includeAllFiles)
            : QueryPanamaCors(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::HondurasCors)) {
        sources.push_back(QueryHondurasCors(
            date,
            archiveCatalog,
            igsCatalog,
            stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::EposGnss)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::EposGnss, includeAllFiles)
            : QueryEposGnss(
                date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Gref)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Gref, includeAllFiles)
            : QueryGref(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::OsNet)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::OsNet, includeAllFiles)
            : QueryOsNet(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Ergnss)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Ergnss, includeAllFiles)
            : QueryErgnss(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Renep)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Renep, includeAllFiles)
            : QueryRenep(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::GeoNetNz)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::GeoNetNz, includeAllFiles)
            : QueryGeoNetNz(
                date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Ring)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Ring, includeAllFiles)
            : QueryRing(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Apos)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Apos, includeAllFiles)
            : QueryApos(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::RobBelgium)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::RobBelgium, includeAllFiles)
            : QueryRobBelgium(
                date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Dpga)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Dpga, includeAllFiles)
            : QueryDpga(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::CatNet)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::CatNet, includeAllFiles)
            : QueryCatNet(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::SpsLux)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::SpsLux, includeAllFiles)
            : QuerySpsLux(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::KasiNgii)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::KasiNgii, includeAllFiles)
            : QueryKasiNgii(
                date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Renag)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Renag, includeAllFiles)
            : QueryRenag(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Nsgi)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Nsgi, includeAllFiles)
            : QueryNsgi(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::NoaGnss)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::NoaGnss, includeAllFiles)
            : QueryNoaGnss(
                date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::Redgae)) {
        sources.push_back(mode == KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly
            ? QuerySingleSourceKnowledgeBase(
                date, ObservationNetwork::Redgae, includeAllFiles)
            : QueryRedgae(date, archiveCatalog, igsCatalog, stopToken));
    }
    if (!stopToken.stop_requested()
        && HasNetwork(selection.mask, ObservationNetwork::GeoNetJapan)) {
        // GEONET is indexed in optimized 10-station × 10-day blocks by its
        // dedicated builder.  Interactive queries deliberately stay local.
        sources.push_back(QuerySingleSourceKnowledgeBase(
            date, ObservationNetwork::GeoNetJapan, includeAllFiles));
    }
    if (stopToken.stop_requested()) {
        result.error = L"查询已取消。";
        return result;
    }

    std::unordered_map<std::string, std::vector<std::size_t>> byFourChar;
    bool allSourcesEmptyComplete = !sources.empty();
    for (auto& source : sources) {
        allSourcesEmptyComplete = allSourcesEmptyComplete
            && source.emptyResultIsComplete;
        AppendSourceError(result.warning, source.error);
        AppendSourceError(result.warning, source.warning);
        result.usedIgsLocalIndex = result.usedIgsLocalIndex
            || source.fromIgsLocalIndex;
        result.usedLocalKnowledgeBase = result.usedLocalKnowledgeBase
            || source.fromLocalIndex;
        if (source.igsSourcesAttempted > 0) {
            result.igsSourcesAttempted = source.igsSourcesAttempted;
            result.igsSourcesSuccessful = source.igsSourcesSuccessful;
            result.igsNetworkFailures = source.igsNetworkFailures;
            result.igsFailedSources = source.igsFailedSources;
        }
        if (source.networkTransportFailure) {
            result.networkTransportFailure = true;
            if (result.transportErrorCode == 0) {
                result.transportErrorCode = source.transportErrorCode;
            }
        }
        for (auto& station : source.stations) {
            MergeStation(
                result.stations,
                byFourChar,
                std::move(station));
        }
        for (auto& file : source.files) {
            result.knowledgeBaseFiles.push_back(std::move(file));
        }
    }
    result.emptyResultIsComplete = allSourcesEmptyComplete;
    if (result.stations.empty()) {
        if (result.emptyResultIsComplete) {
            return result;
        }
        result.error = result.warning.empty()
            ? L"所选站网在该日期没有找到可下载的日观测文件。"
            : result.warning;
        result.warning.clear();
        return result;
    }

    std::sort(
        result.stations.begin(),
        result.stations.end(),
        [](const StationFile& left, const StationFile& right) {
            return left.stationId < right.stationId;
        });
    result.filesWithoutCoordinates = static_cast<int>(std::count_if(
        result.stations.begin(),
        result.stations.end(),
        [](const StationFile& station) {
            return !station.hasCoordinate;
        }));
    return result;
}

QueryResult QueryObservationDateRangeImpl(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const NetworkSelection selection,
    const ObservationRangeOptions& options,
    const std::stop_token stopToken,
    const bool materializePeriodFiles,
    const std::span<const StationFile> wantedStations,
    const bool allowIncompleteDates,
    const ObservationManifestProgressCallback& manifestProgress) {
    QueryResult result;
    result.date = startDate;
    result.selection = selection;
    result.periodStart = startDate;
    result.periodEnd = endDate;
    result.directoryUrl = L"本地 GNSS 观测知识库（日期范围）";

    if (!IsRangeDateValid(startDate) || !IsRangeDateValid(endDate)) {
        result.error = L"日期范围无效。";
        return result;
    }
    if (!selection.Any()) {
        result.error = L"请至少选择一个站网。";
        return result;
    }
    const auto startDays = ToRangeSysDays(startDate);
    const auto endDays = ToRangeSysDays(endDate);
    if (endDays < startDays) {
        result.error = L"结束日期不能早于起始日期。";
        return result;
    }
    const auto dayCount = (endDays - startDays).count() + 1;
    if (dayCount <= 0
        || dayCount > static_cast<long long>(std::numeric_limits<int>::max())) {
        result.error = L"日期范围过大。";
        return result;
    }
    int expectedDays = static_cast<int>(dayCount);

    // Space-weather predicates select dates before station completeness is
    // evaluated.  Keep this range reader consistent with the bitmap query
    // path: the denominator is the number of dates that survived the Kp/Dst/
    // F10.7 filter, and daily rows outside that date set are not eligible for
    // either completeness or the eventual download manifest.
    const auto weather = SelectSpaceWeatherDates(
        DefaultSpaceWeatherDatabasePath(),
        startDate,
        endDate,
        options,
        stopToken);
    if (!weather.error.empty()) {
        result.error = weather.error;
        return result;
    }
    if (weather.active) {
        expectedDays = weather.matchedDays;
        result.warning = weather.warning;
        if (expectedDays <= 0) {
            result.coversDateRange = true;
            result.emptyResultIsComplete = true;
            if (result.warning.empty()) {
                result.warning = L"没有日期满足当前空间天气筛选条件。";
            }
            return result;
        }
    }

    // Normalise all row-level predicates once.  The range may contain
    // millions of records; rebuilding these vectors inside the row loop is
    // disproportionately expensive (especially for the default 30S filter).
    const PreparedRangeFilters preparedFilters = PrepareRangeFilters(options);
    const ObservationStationDateFilter frameDateFilter(options);
    const auto expectedDaysForStation = [&](const StationFile& station) {
        if (!frameDateFilter.active()) return expectedDays;
        const int firstOrdinal = static_cast<int>(
            startDays.time_since_epoch().count());
        const int lastOrdinal = static_cast<int>(
            endDays.time_since_epoch().count());
        int count = 0;
        for (const auto& range : frameDateFilter.Ranges(
                station, firstOrdinal, lastOrdinal)) {
            if (!weather.active) {
                count += range.second - range.first + 1;
            } else {
                for (int ordinal = range.first; ordinal <= range.second; ++ordinal) {
                    if (weather.Contains(ordinal)) ++count;
                }
            }
        }
        return count;
    };

    // The SQLite range readers can apply at most one exact RINEX-family and
    // one exact interval predicate.  Keep all other cases in the existing
    // authoritative matcher: this is important for multi-value intervals
    // and for the independent include/exclude switches.
    int sqlRinexVersion = preparedFilters.rinexVersion;
    if (sqlRinexVersion == 0
        && preparedFilters.includeRinex2 != preparedFilters.includeRinex34) {
        sqlRinexVersion = preparedFilters.includeRinex2 ? 2 : 3;
    }
    const std::string_view sqlSampleInterval =
        preparedFilters.intervals.size() == 1
        ? std::string_view(preparedFilters.intervals.front())
        : std::string_view{};

    std::wstring unsupported;
    if (!SupportsStrictLocalRangeQuery(selection, unsupported)) {
        result.error = L"日期范围查询只读取本地知识库；站网 "
            + unsupported
            + L" 当前没有可用的本地范围索引，未联网。";
        return result;
    }

    struct RangeAggregate {
        std::vector<DatedStationFile> records;
        // Summary queries only need one representative plus a day-presence
        // bitmap.  Keeping full StationFile objects for every day would
        // defeat deferred download even if QueryResult::periodFiles stayed
        // empty, so the lightweight path stores no records here.
        StationFile summaryRepresentative;
        bool hasSummaryRepresentative = false;
        // A mixed-network range query can expose the same physical station
        // more than once for a day (for example a preferred row plus an
        // alternate RINEX family).  For the normal (year-or-shorter) query,
        // a dense day-index array is considerably cheaper than one
        // unordered_map allocation per station.  It also makes the hot
        // lookup O(1) with no hashing.  Very long ranges use the sparse map
        // fallback below to avoid allocating an unexpectedly large matrix.
        std::vector<std::int32_t> recordByDayDense;
        std::unordered_map<int, std::size_t> recordByDaySparse;
        std::vector<std::uint8_t> daySeenDense;
        std::unordered_set<int> daySeenSparse;
        int availableDays = 0;
        int expectedDays = 0;
        RinexFormatMask availableFormats = 0;
    };
    std::vector<RangeAggregate> aggregates;
    // Exact station IDs are the fast path.  Four-character buckets handle
    // regional archives that use different nine-character suffixes while
    // still exposing the same physical monument and coordinates.
    // A short station code can legitimately occur in more than one selected
    // archive.  Keep all aggregate candidates under the ID and let
    // SamePhysicalStation (network-aware and coordinate-aware) choose one;
    // mapping a key to the first row would route every later duplicate into
    // the wrong physical station.
    std::unordered_map<std::string, std::vector<std::size_t>> byStationId;
    std::unordered_map<std::string, std::vector<std::size_t>> byFourChar;
    byStationId.reserve(4096);
    byFourChar.reserve(4096);

    // Build small lookup buckets once.  Download hydration normally receives
    // only a handful of selected representatives, but an exact-ID fast path
    // keeps the per-row test O(1), while the four-character bucket preserves
    // the same coordinate-aware physical-station matching used by the range
    // union itself.
    std::unordered_set<std::string> wantedStationIds;
    std::unordered_map<std::string, std::vector<std::size_t>> wantedByFourChar;
    if (!wantedStations.empty()) {
        wantedStationIds.reserve(wantedStations.size() * 2U);
        wantedByFourChar.reserve(wantedStations.size());
        for (std::size_t index = 0; index < wantedStations.size(); ++index) {
            const auto stationId = ToUpperAscii(wantedStations[index].stationId);
            if (!stationId.empty()) wantedStationIds.insert(stationId);
            const auto fourChar = ToUpperAscii(wantedStations[index].fourCharId);
            if (!fourChar.empty()) wantedByFourChar[fourChar].push_back(index);
        }
    }
    const auto isWantedStation = [&](const StationFile& station) {
        if (wantedStations.empty()) return true;
        const auto stationId = ToUpperAscii(station.stationId);
        if (!stationId.empty() && wantedStationIds.contains(stationId)) {
            // Do not let a reused four-character code from another archive
            // pass solely because its text ID matches.  Compare against the
            // selected representative's network membership and physical
            // identity; this keeps a selected NGS OKAK from pulling in the
            // unrelated SOPAC OKAK during deferred hydration.
            for (const auto& wanted : wantedStations) {
                if (ToUpperAscii(wanted.stationId) != stationId) continue;
                if (SamePhysicalStation(wanted, station)) return true;
            }
            return false;
        }
        const auto fourChar = ToUpperAscii(station.fourCharId);
        const auto found = wantedByFourChar.find(fourChar);
        if (found == wantedByFourChar.end()) return false;
        return std::any_of(
            found->second.begin(), found->second.end(),
            [&](const std::size_t index) {
                return SamePhysicalStation(wantedStations[index], station);
            });
    };

    const auto appendWarning = [&](const QueryDate& date,
                                   const std::wstring& warning) {
        if (warning.empty()) return;
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += FormatDate(date) + L"：" + warning;
    };
    const auto hasMissingIndexMessage = [](const std::wstring& text) {
        return text.find(L"尚未收录该日期") != std::wstring::npos
            || text.find(L"本地知识库失败") != std::wstring::npos
            || text.find(L"本地知识库读取失败") != std::wstring::npos
            || text.find(L"本地索引暂不可用") != std::wstring::npos
            // A provisional/legacy IGS row means that at least one of the
            // seven required mirrors was not verified.  It is useful for a
            // single-day preview, but must not silently count as a complete
            // day in an exact date-range query.
            || text.find(L"部分七源") != std::wstring::npos
            || text.find(L"旧版") != std::wstring::npos
            || text.find(L"最近7天暂定") != std::wstring::npos
            || text.find(L"暂定记录") != std::wstring::npos
            || text.find(L"待复扫") != std::wstring::npos;
    };

    const bool useDenseDayIndex = expectedDays <= 4096;
    // Keep both file families until a frequency predicate has been applied.
    // A preferred R2 mirror must not hide an eligible R3 file (or vice versa).
    bool collectFrequencyCandidates = materializePeriodFiles
        && (!options.signalCodesInclude.empty() || !options.signalCodesExclude.empty());
    std::vector<DatedStationFile> frequencyCandidates;


    // Add one already-filtered file candidate to the physical-station/day
    // aggregate.  Both the batched range loaders and the compatibility
    // daily-loader path use this exact merge rule, so changing the read path
    // cannot change which station/file is exposed to the user.
    const auto appendStation = [&](const QueryDate& day,
                                   const int dayIndex,
                                   StationFile station) {
        if (weather.active) {
            const auto ordinal = ToRangeSysDays(day).time_since_epoch().count();
            if (!weather.Contains(static_cast<int>(ordinal))) {
                return;
            }
        }
        if (frameDateFilter.active()
            && !frameDateFilter.Allows(station, day)) {
            return;
        }
        if (!isWantedStation(station)
            || !MatchesPreparedRangeOptions(station, preparedFilters)) {
            return;
        }

        if (collectFrequencyCandidates) {
            DatedStationFile candidate;
            candidate.date = day;
            candidate.station = std::move(station);
            frequencyCandidates.push_back(std::move(candidate));
            return;
        }

        std::size_t aggregateIndex = std::numeric_limits<std::size_t>::max();
        const std::string stationId = ToUpperAscii(station.stationId);
        if (!stationId.empty()) {
            const auto found = byStationId.find(stationId);
            if (found != byStationId.end()) {
                for (const auto candidate : found->second) {
                    if (candidate >= aggregates.size()) continue;
                    const auto& aggregate = aggregates[candidate];
                    const StationFile* exemplar = nullptr;
                    if (materializePeriodFiles) {
                        if (!aggregate.records.empty()) {
                            exemplar = &aggregate.records.front().station;
                        }
                    } else if (aggregate.hasSummaryRepresentative) {
                        exemplar = &aggregate.summaryRepresentative;
                    }
                    if (exemplar != nullptr
                        && SamePhysicalStation(*exemplar, station)) {
                        aggregateIndex = candidate;
                        break;
                    }
                }
            }
        }
        const std::string fourChar = ToUpperAscii(station.fourCharId);
        if (aggregateIndex == std::numeric_limits<std::size_t>::max()
            && !fourChar.empty()) {
            const auto found = byFourChar.find(fourChar);
            if (found != byFourChar.end()) {
                for (const auto candidate : found->second) {
                    const auto& aggregate = aggregates[candidate];
                    const StationFile* exemplar = nullptr;
                    if (materializePeriodFiles) {
                        if (!aggregate.records.empty()) {
                            exemplar = &aggregate.records.front().station;
                        }
                    } else if (aggregate.hasSummaryRepresentative) {
                        exemplar = &aggregate.summaryRepresentative;
                    }
                    if (exemplar != nullptr
                        && SamePhysicalStation(*exemplar, station)) {
                        aggregateIndex = candidate;
                        break;
                    }
                }
            }
        }
        if (aggregateIndex == std::numeric_limits<std::size_t>::max()) {
            aggregateIndex = aggregates.size();
            aggregates.emplace_back();
            aggregates.back().expectedDays = expectedDaysForStation(station);
            if (useDenseDayIndex) {
                if (materializePeriodFiles) {
                    aggregates.back().recordByDayDense.assign(
                        static_cast<std::size_t>(dayCount), -1);
                } else {
                    aggregates.back().daySeenDense.assign(
                        static_cast<std::size_t>(dayCount), 0);
                }
            }
            if (!stationId.empty()) {
                byStationId[stationId].push_back(aggregateIndex);
            }
            if (!fourChar.empty()) {
                byFourChar[fourChar].push_back(aggregateIndex);
            }
        } else if (!stationId.empty()) {
            // A station can acquire a nine-character suffix in a later
            // archive revision.  Keep that alias on the same aggregate.  Do
            // not insert duplicates into the candidate vector: otherwise a
            // later row would perform redundant compatibility checks.
            auto& candidates = byStationId[stationId];
            if (std::find(candidates.begin(), candidates.end(), aggregateIndex)
                == candidates.end()) {
                candidates.push_back(aggregateIndex);
            }
        }

        auto& aggregate = aggregates[aggregateIndex];
        aggregate.availableFormats = static_cast<RinexFormatMask>(
            aggregate.availableFormats | EffectiveRinexFormatMask(station));
        if (!materializePeriodFiles) {
            bool alreadySeen = false;
            if (useDenseDayIndex) {
                auto& seen = aggregate.daySeenDense[
                    static_cast<std::size_t>(dayIndex)];
                alreadySeen = seen != 0;
                seen = 1;
            } else {
                alreadySeen = !aggregate.daySeenSparse.insert(dayIndex).second;
            }
            if (!alreadySeen) ++aggregate.availableDays;

            if (!aggregate.hasSummaryRepresentative) {
                aggregate.summaryRepresentative = std::move(station);
                aggregate.hasSummaryRepresentative = true;
                return;
            }

            auto& representative = aggregate.summaryRepresentative;
            const auto memberships = static_cast<ObservationNetworkMask>(
                representative.availableNetworks | station.availableNetworks);
            const bool representativeHasCoordinate =
                representative.hasCoordinate;
            const double representativeLatitude = representative.latitude;
            const double representativeLongitude = representative.longitude;
            const bool candidateHasCoordinate = station.hasCoordinate;
            const double candidateLatitude = station.latitude;
            const double candidateLongitude = station.longitude;
            std::string country = representative.country.empty()
                ? station.country : representative.country;
            std::vector<std::string> sources =
                representative.availabilitySources;
            for (const auto& source : station.availabilitySources) {
                if (std::find(sources.begin(), sources.end(), source)
                    == sources.end()) {
                    sources.push_back(source);
                }
            }
            if (DownloadPreference(station)
                > DownloadPreference(representative)) {
                representative = std::move(station);
            }
            representative.availableNetworks = memberships;
            representative.availabilitySources = std::move(sources);
            if (representative.country.empty()) {
                representative.country = std::move(country);
            }
            if (!representative.hasCoordinate) {
                if (candidateHasCoordinate) {
                    representative.hasCoordinate = true;
                    representative.latitude = candidateLatitude;
                    representative.longitude = candidateLongitude;
                } else if (representativeHasCoordinate) {
                    representative.hasCoordinate = true;
                    representative.latitude = representativeLatitude;
                    representative.longitude = representativeLongitude;
                }
            }
            return;
        }

        std::size_t existingIndex = std::numeric_limits<std::size_t>::max();
        if (useDenseDayIndex) {
            const auto value = aggregate.recordByDayDense[
                static_cast<std::size_t>(dayIndex)];
            if (value >= 0) {
                existingIndex = static_cast<std::size_t>(value);
            }
        } else {
            const auto found = aggregate.recordByDaySparse.find(dayIndex);
            if (found != aggregate.recordByDaySparse.end()) {
                existingIndex = found->second;
            }
        }
        if (existingIndex != std::numeric_limits<std::size_t>::max()) {
            auto& existingRecord = aggregate.records[existingIndex];
            auto& existing = existingRecord.station;
            const auto formats = static_cast<RinexFormatMask>(
                EffectiveRinexFormatMask(existing)
                | EffectiveRinexFormatMask(station));
            const bool replacementHasCoordinate = station.hasCoordinate;
            const double replacementLatitude = station.latitude;
            const double replacementLongitude = station.longitude;
            // Preserve every verified mirror for the same physical file.
            if (existing.fileName == station.fileName) {
                for (const auto& route : station.downloadRoutes) {
                    if (std::none_of(existing.downloadRoutes.begin(), existing.downloadRoutes.end(),
                            [&](const DownloadRoute& item) { return item.url == route.url; }))
                        existing.downloadRoutes.push_back(route);
                }
                station.downloadRoutes = existing.downloadRoutes;
            }
            const bool onlineDay = std::any_of(options.onlineSnapshots.begin(), options.onlineSnapshots.end(),
                [&](const ObservationSnapshotWindow& snapshot) {
                    return (snapshot.networks & station.availableNetworks) != 0
                        && ToRangeSysDays(day) > ToRangeSysDays(snapshot.lastKnownDate);
                });
            const bool replace = onlineDay && station.format != existing.format
                ? station.format == RinexFormat::Rinex34
                : DownloadPreference(station) > DownloadPreference(existing);
            if (replace) {
                const bool selected = existing.selected;
                const auto memberships = static_cast<ObservationNetworkMask>(
                    existing.availableNetworks | station.availableNetworks);
                existing = std::move(station);
                existing.selected = selected;
                existing.availableNetworks = memberships;
            } else {
                existing.availableNetworks =
                    static_cast<ObservationNetworkMask>(
                        existing.availableNetworks | station.availableNetworks);
            }
            if (!existing.hasCoordinate && replacementHasCoordinate) {
                existing.hasCoordinate = true;
                existing.latitude = replacementLatitude;
                existing.longitude = replacementLongitude;
            }
            existing.availableRinexFormats = formats;
            return;
        }

        DatedStationFile dated;
        dated.date = day;
        dated.stationIndex = aggregateIndex;
        dated.station = std::move(station);
        const std::size_t recordIndex = aggregate.records.size();
            aggregate.records.push_back(std::move(dated));
        if (useDenseDayIndex) {
            aggregate.recordByDayDense[static_cast<std::size_t>(dayIndex)] =
                static_cast<std::int32_t>(recordIndex);
        } else {
            aggregate.recordByDaySparse.emplace(dayIndex, recordIndex);
        }
        ++aggregate.availableDays;
    };

    // Long range queries are strictly local.  Read each selected SQLite
    // archive once for the whole interval, then feed its rows through the
    // same physical-station merge used by the legacy daily path.  The readers
    // run concurrently, but their bounded per-source queues are consumed in
    // the historical source order below.  This overlaps independent SQLite
    // I/O without changing tie-breaking/merge semantics or retaining a
    // second full-range vector for every source.
    const auto appendRangeError = [&](const ObservationNetwork network,
                                      const std::wstring& detail) {
        result.error = L"日期范围查询未完成："
            + KnowledgeBaseNetworkLabel(network) + L"（" + detail + L"）。未联网。";
    };
    const auto appendIncompleteRangeWarning = [&result](
            const ObservationNetwork network,
            const bool igs,
            const std::vector<QueryDate>& dates) {
        if (dates.empty()) return;
        std::wstring warning = igs
            ? L"IGS/MGEX：" + std::to_wstring(dates.size())
                + L" 天未达到严格七源；已使用当天现有的可用来源，"
                  L"完全无索引的日期按无数据跳过"
            : KnowledgeBaseNetworkLabel(network) + L"："
                + std::to_wstring(dates.size())
                + L" 天知识库为缺失、失败或暂定状态，已按无数据跳过";
        warning += L"（";
        const std::size_t shown = std::min<std::size_t>(dates.size(), 5U);
        for (std::size_t index = 0; index < shown; ++index) {
            if (index != 0) warning += L"、";
            warning += FormatDate(dates[index]);
        }
        if (shown < dates.size()) warning += L"等";
        warning += L"）";
        if (!result.warning.empty()) result.warning += L"；";
        result.warning += std::move(warning);
    };
    struct RangeTaskSpec {
        ObservationNetwork network = ObservationNetwork::Igs;
        bool igs = false;
    };
    std::vector<RangeTaskSpec> rangeTasks;
    const bool wantedMembershipReliable = !wantedStations.empty()
        && std::all_of(
            wantedStations.begin(), wantedStations.end(),
            [](const StationFile& station) {
                return station.availableNetworks != 0;
            });
    const auto wantedCouldUseNetwork = [&](const ObservationNetwork network,
                                           const bool igsShared) {
        if (wantedStations.empty() || !wantedMembershipReliable) return true;
        return std::any_of(
            wantedStations.begin(), wantedStations.end(),
            [&](const StationFile& station) {
                if (igsShared) {
                    return HasNetwork(
                               station.availableNetworks,
                               ObservationNetwork::Igs)
                        || HasNetwork(
                               station.availableNetworks,
                               ObservationNetwork::Mgex);
                }
                return HasNetwork(station.availableNetworks, network);
            });
    };
    if (HasNetwork(selection.mask, ObservationNetwork::Igs)
        || HasNetwork(selection.mask, ObservationNetwork::Mgex)) {
        // IGS and MGEX share one physical database and one reader.
        if (wantedCouldUseNetwork(ObservationNetwork::Igs, true)) {
            rangeTasks.push_back({ObservationNetwork::Igs, true});
        }
    }
    for (const auto network : kAllObservationNetworks) {
        if (network == ObservationNetwork::Igs
            || network == ObservationNetwork::Mgex
            || !HasNetwork(selection.mask, network)) {
            continue;
        }
        if (wantedCouldUseNetwork(network, false)) {
            rangeTasks.push_back({network, false});
        }
    }

    const auto stationBelongsToTask = [&](const StationFile& station,
                                          const RangeTaskSpec& task) {
        if (!wantedMembershipReliable) return true;
        if (task.igs) {
            return HasNetwork(
                       station.availableNetworks, ObservationNetwork::Igs)
                || HasNetwork(
                       station.availableNetworks, ObservationNetwork::Mgex);
        }
        return HasNetwork(station.availableNetworks, task.network);
    };
    std::vector<std::vector<std::string>> rangeTaskFourChars(
        rangeTasks.size());
    for (std::size_t taskIndex = 0;
         taskIndex < rangeTasks.size(); ++taskIndex) {
        std::unordered_set<std::string> seen;
        for (const auto& station : wantedStations) {
            if (!stationBelongsToTask(station, rangeTasks[taskIndex])) continue;
            std::string four = ToUpperAscii(station.fourCharId);
            if (four.size() != 4U && station.stationId.size() >= 4U) {
                four = ToUpperAscii(station.stationId.substr(0, 4));
            }
            if (four.size() == 4U && seen.insert(four).second) {
                rangeTaskFourChars[taskIndex].push_back(std::move(four));
            }
        }
        std::sort(
            rangeTaskFourChars[taskIndex].begin(),
            rangeTaskFourChars[taskIndex].end());
        // One SQL statement can seek both key forms for 120 stations.  Above
        // that point, hundreds of per-day seeks become slower than the
        // original sequential range scan, especially when the user presses
        // "select all" and genuinely needs most rows.  Keep the fast seek
        // path for ordinary 1-120 station jobs and automatically fall back
        // to the bulk reader for larger manifests.
        constexpr std::size_t maximumTargetedStations = 120U;
        if (rangeTaskFourChars[taskIndex].size()
            > maximumTargetedStations) {
            rangeTaskFourChars[taskIndex].clear();
        }
    }

    // Targeted SQLite readers execute one primary-key seek for every
    // UTC-date / station-prefix statement.  This is both a stable progress
    // unit and a much more honest progress bar than counting bytes read from
    // unrelated rows in a multi-gigabyte database.
    constexpr std::size_t stationRangesPerStatement = 240U;
    std::vector<std::size_t> manifestTaskTotalUnits(
        rangeTasks.size(), static_cast<std::size_t>(expectedDays));
    std::size_t manifestTotalUnits = 0;
    for (std::size_t taskIndex = 0;
         taskIndex < rangeTasks.size(); ++taskIndex) {
        if (!rangeTaskFourChars[taskIndex].empty()) {
            const std::size_t keyRanges =
                rangeTaskFourChars[taskIndex].size() * 2U;
            const std::size_t statements =
                (keyRanges + stationRangesPerStatement - 1U)
                / stationRangesPerStatement;
            manifestTaskTotalUnits[taskIndex] =
                static_cast<std::size_t>(expectedDays) * statements;
        }
        manifestTotalUnits += manifestTaskTotalUnits[taskIndex];
    }
    std::vector<std::size_t> manifestTaskCompletedUnits(
        rangeTasks.size(), 0U);
    std::atomic_size_t manifestFilesFound{0};
    std::mutex manifestProgressMutex;
    const auto publishManifestProgress = [&](const std::size_t taskIndex,
                                             const std::size_t completed,
                                             const std::size_t reportedTotal) {
        if (!manifestProgress || taskIndex >= rangeTasks.size()) return;
        std::scoped_lock lock(manifestProgressMutex);
        const std::size_t expected = manifestTaskTotalUnits[taskIndex];
        const std::size_t normalised = reportedTotal == 0
            ? std::min(completed, expected)
            : static_cast<std::size_t>(std::llround(
                static_cast<double>(std::min(completed, reportedTotal))
                    / static_cast<double>(reportedTotal)
                    * static_cast<double>(expected)));
        manifestTaskCompletedUnits[taskIndex] =
            std::min(normalised, expected);
        ObservationManifestProgress value;
        value.totalUnits = manifestTotalUnits;
        value.totalSources = rangeTasks.size();
        for (std::size_t index = 0;
             index < manifestTaskCompletedUnits.size(); ++index) {
            value.completedUnits += manifestTaskCompletedUnits[index];
            if (manifestTaskCompletedUnits[index]
                >= manifestTaskTotalUnits[index]) {
                ++value.completedSources;
            }
        }
        value.discoveredFiles = manifestFilesFound.load(
            std::memory_order_relaxed);
        value.currentSource = rangeTasks[taskIndex].igs
            ? L"IGS/MGEX"
            : KnowledgeBaseNetworkLabel(rangeTasks[taskIndex].network);
        manifestProgress(value);
    };
    if (manifestProgress) {
        ObservationManifestProgress value;
        value.totalUnits = manifestTotalUnits;
        value.totalSources = rangeTasks.size();
        value.currentSource = L"准备本地知识库";
        manifestProgress(value);
    }

    struct RangeTaskChannel {
        std::mutex mutex;
        std::condition_variable dataReady;
        std::condition_variable spaceReady;
        std::deque<std::vector<DatedStationFile>> batches;
        bool started = false;
        bool finished = false;
        bool complete = false;
        bool stopped = false;
        std::vector<QueryDate> incompleteDates;
        std::wstring error;
    };
    std::vector<std::unique_ptr<RangeTaskChannel>> channels;
    channels.reserve(rangeTasks.size());
    for (std::size_t index = 0; index < rangeTasks.size(); ++index) {
        channels.push_back(std::make_unique<RangeTaskChannel>());
    }
    constexpr std::size_t rangeBatchSize = 256;
    constexpr std::size_t maxQueuedBatches = 4;
    std::atomic<std::size_t> nextRangeTask{0};
    std::atomic_bool cancelRangeReaders{false};

    const auto wakeRangeChannels = [&]() {
        for (const auto& channel : channels) {
            channel->dataReady.notify_all();
            channel->spaceReady.notify_all();
        }
    };
    const auto cancelRangeReadersNow = [&]() {
        cancelRangeReaders.store(true, std::memory_order_release);
        wakeRangeChannels();
    };

    // Publish a bounded batch.  The coordinator consumes channels in the
    // same order as the old sequential loop, so equal-preference candidates
    // retain their previous winner while database reads overlap in workers.
    const auto publishRangeBatch = [&](RangeTaskChannel& channel,
                                       std::vector<DatedStationFile>& batch) {
        if (batch.empty()) return true;
        std::unique_lock lock(channel.mutex);
        channel.spaceReady.wait(lock, [&]() {
            return channel.batches.size() < maxQueuedBatches
                || cancelRangeReaders.load(std::memory_order_acquire)
                || stopToken.stop_requested();
        });
        if (cancelRangeReaders.load(std::memory_order_acquire)
            || stopToken.stop_requested()) {
            return false;
        }
        channel.batches.push_back(std::move(batch));
        batch.clear();
        lock.unlock();
        channel.dataReady.notify_one();
        return true;
    };

    const auto runRangeTask = [&](const std::size_t taskIndex) {
        auto& channel = *channels[taskIndex];
        const auto spec = rangeTasks[taskIndex];
        {
            std::scoped_lock lock(channel.mutex);
            channel.started = true;
        }

        std::vector<DatedStationFile> batch;
        batch.reserve(rangeBatchSize);
        const auto emit = [&](DatedStationFile&& dated) {
            if (stopToken.stop_requested()
                || cancelRangeReaders.load(std::memory_order_acquire)) {
                return false;
            }
            const auto dayIndexValue =
                (ToRangeSysDays(dated.date) - startDays).count();
            if (dayIndexValue < 0 || dayIndexValue >= dayCount) {
                return true;
            }
            const auto filesFound = manifestFilesFound.fetch_add(
                1, std::memory_order_relaxed) + 1U;
            if (rangeTaskFourChars[taskIndex].empty()
                && (filesFound & 0xfffU) == 0U) {
                // A large "select all" job deliberately uses the sequential
                // scan.  Its exact row total is database-specific, but keep
                // the live file counter moving while this source is active.
                publishManifestProgress(taskIndex, 0, 1);
            }
            batch.push_back(std::move(dated));
            if (batch.size() >= rangeBatchSize
                && !publishRangeBatch(channel, batch)) {
                return false;
            }
            return true;
        };

        bool complete = false;
        bool stopped = false;
        std::vector<QueryDate> incompleteDates;
        std::wstring error;
        try {
            if (spec.igs) {
                const bool includeIgs = HasNetwork(
                    selection.mask, ObservationNetwork::Igs);
                const bool includeMgex = HasNetwork(
                    selection.mask, ObservationNetwork::Mgex);
                const auto stream = StreamIgsAvailabilityRange(
                    startDate,
                    endDate,
                    [&](DatedIndexedIgsAvailability&& dated) {
                        if (!includeIgs
                            && !(includeMgex
                                && HasNetwork(
                                    dated.record.station.availableNetworks,
                                    ObservationNetwork::Mgex))) {
                            return true;
                        }
                        return emit(DatedStationFile{
                            dated.date,
                            0,
                            std::move(dated.record.station)});
                    },
                    sqlRinexVersion,
                    sqlSampleInterval,
                    allowIncompleteDates,
                    rangeTaskFourChars[taskIndex],
                    [&](const std::size_t completed,
                        const std::size_t total) {
                        publishManifestProgress(taskIndex, completed, total);
                    });
                complete = stream.complete;
                stopped = stream.stopped;
                incompleteDates = stream.incompleteDates;
                error = stream.error;
            } else {
                const auto stream = StreamNetworkAvailabilityRange(
                    startDate,
                    endDate,
                    spec.network,
                    true,
                    [&](DatedStationFile&& dated) {
                        return emit(std::move(dated));
                    },
                    sqlRinexVersion,
                    sqlSampleInterval,
                    allowIncompleteDates,
                    rangeTaskFourChars[taskIndex],
                    [&](const std::size_t completed,
                        const std::size_t total) {
                        publishManifestProgress(taskIndex, completed, total);
                    });
                complete = stream.complete;
                stopped = stream.stopped;
                incompleteDates = stream.incompleteDates;
                error = stream.error;
            }
            if (error.empty() && !stopped && !batch.empty()) {
                if (!publishRangeBatch(channel, batch)) stopped = true;
            }
        } catch (const std::exception& exception) {
            error = Utf8ToWide(exception.what());
        } catch (...) {
            error = L"读取本地知识库时发生未知异常。";
        }
        {
            std::scoped_lock lock(channel.mutex);
            channel.complete = complete;
            channel.stopped = stopped;
            channel.incompleteDates = std::move(incompleteDates);
            channel.error = std::move(error);
            channel.finished = true;
        }
        channel.dataReady.notify_all();
        channel.spaceReady.notify_all();
    };

    // Four readers are enough to overlap independent SQLite I/O while
    // avoiding dozens of simultaneous SQLite file handles on slower disks.
    const std::size_t rangeWorkerCount = std::min<std::size_t>(
        4U, rangeTasks.size());
    std::vector<std::jthread> rangeWorkers;
    rangeWorkers.reserve(rangeWorkerCount);
    for (std::size_t worker = 0; worker < rangeWorkerCount; ++worker) {
        rangeWorkers.emplace_back([&]() {
            while (!cancelRangeReaders.load(std::memory_order_acquire)) {
                const auto taskIndex = nextRangeTask.fetch_add(
                    1, std::memory_order_acq_rel);
                if (taskIndex >= rangeTasks.size()) return;
                runRangeTask(taskIndex);
            }
        });
    }

    bool bulkRangeLoaded = true;
    for (std::size_t taskIndex = 0;
         taskIndex < rangeTasks.size() && bulkRangeLoaded;
         ++taskIndex) {
        auto& channel = *channels[taskIndex];
        while (true) {
            std::vector<DatedStationFile> batch;
            {
                std::unique_lock lock(channel.mutex);
                channel.dataReady.wait(lock, [&]() {
                    return !channel.batches.empty()
                        || channel.finished
                        || cancelRangeReaders.load(
                            std::memory_order_acquire)
                        || stopToken.stop_requested();
                });
                if (!channel.batches.empty()) {
                    batch = std::move(channel.batches.front());
                    channel.batches.pop_front();
                    lock.unlock();
                    channel.spaceReady.notify_one();
                } else if (channel.finished) {
                    break;
                } else {
                    break;
                }
            }
            if (stopToken.stop_requested()) {
                result.error = L"查询已取消。";
                bulkRangeLoaded = false;
                cancelRangeReadersNow();
                break;
            }
            for (auto& dated : batch) {
                if (stopToken.stop_requested()) {
                    result.error = L"查询已取消。";
                    bulkRangeLoaded = false;
                    cancelRangeReadersNow();
                    break;
                }
                const auto dayIndexValue =
                    (ToRangeSysDays(dated.date) - startDays).count();
                appendStation(
                    dated.date,
                    static_cast<int>(dayIndexValue),
                    std::move(dated.station));
            }
            if (!bulkRangeLoaded) break;
        }
        if (!bulkRangeLoaded) break;
        bool complete = false;
        bool stopped = false;
        std::vector<QueryDate> incompleteDates;
        std::wstring taskError;
        {
            std::scoped_lock lock(channel.mutex);
            complete = channel.complete;
            stopped = channel.stopped;
            incompleteDates = channel.incompleteDates;
            taskError = channel.error;
        }
        if (stopToken.stop_requested() || stopped) {
            result.error = L"查询已取消。";
            bulkRangeLoaded = false;
            cancelRangeReadersNow();
            break;
        }
        if (!taskError.empty() || !complete) {
            if (rangeTasks[taskIndex].igs) {
                result.error = L"日期范围查询未完成：IGS 本地知识库日期范围不完整（"
                    + (taskError.empty()
                        ? L"缺少完整七源日期"
                        : taskError)
                    + L"）。未联网。";
            } else {
                appendRangeError(
                    rangeTasks[taskIndex].network,
                    taskError.empty()
                        ? L"本地知识库日期范围不完整"
                        : taskError);
            }
            bulkRangeLoaded = false;
            cancelRangeReadersNow();
            break;
        }
        if (allowIncompleteDates && !incompleteDates.empty()) {
            std::erase_if(incompleteDates, [&](const QueryDate& date) {
                const auto mask = rangeTasks[taskIndex].igs
                    ? NetworkBit(ObservationNetwork::Igs) | NetworkBit(ObservationNetwork::Mgex)
                    : NetworkBit(rangeTasks[taskIndex].network);
                return std::any_of(options.onlineSnapshots.begin(), options.onlineSnapshots.end(),
                    [&](const ObservationSnapshotWindow& snapshot) {
                        return (snapshot.networks & mask) != 0
                            && ToRangeSysDays(date) > ToRangeSysDays(snapshot.lastKnownDate);
                    });
            });
            appendIncompleteRangeWarning(
                rangeTasks[taskIndex].network,
                rangeTasks[taskIndex].igs,
                incompleteDates);
        }
        result.usedLocalKnowledgeBase = true;
        result.usedIgsLocalIndex = result.usedIgsLocalIndex
            || rangeTasks[taskIndex].igs;
        publishManifestProgress(
            taskIndex,
            manifestTaskTotalUnits[taskIndex],
            manifestTaskTotalUnits[taskIndex]);
    }
    if (stopToken.stop_requested() && result.error.empty()) {
        result.error = L"查询已取消。";
        bulkRangeLoaded = false;
        cancelRangeReadersNow();
    }
    if (!bulkRangeLoaded) {
        return result;
    }

    /*
     * Compatibility implementation retained below for source-level
     * reference while the batched path is active.  It is deliberately not
     * executed; keeping it here makes it straightforward to diagnose an old
     * database with a debugger without restoring the expensive default.
     */
    if (false) {
    for (int dayIndex = 0; dayIndex < dayCount; ++dayIndex) {
        if (stopToken.stop_requested()) {
            result.error = L"查询已取消。";
            return result;
        }
        const QueryDate day = AddDays(startDate, dayIndex);
        auto daily = QueryDailyObservationFiles(
            day,
            selection,
            stopToken,
            KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly,
            true);
        if (!daily.error.empty()) {
            if (stopToken.stop_requested()
                || daily.error == L"查询已取消。") {
                result.error = L"查询已取消。";
                return result;
            }
            result.error = L"日期范围查询未完成：本地知识库缺少 "
                + FormatDate(day) + L"（" + daily.error + L"）。未联网。";
            return result;
        }
        // QueryDaily intentionally keeps a partial union when one selected
        // network is missing.  A range query must not silently count that
        // day, so promote the explicit local-index diagnostic to an error.
        if (hasMissingIndexMessage(daily.warning)) {
            result.error = L"日期范围查询未完成：本地知识库缺少 "
                + FormatDate(day) + L"（" + daily.warning + L"）。未联网。";
            return result;
        }
        appendWarning(day, daily.warning);
        result.usedIgsLocalIndex = result.usedIgsLocalIndex
            || daily.usedIgsLocalIndex;
        result.usedLocalKnowledgeBase = result.usedLocalKnowledgeBase
            || daily.usedLocalKnowledgeBase;
        result.networkTransportFailure = result.networkTransportFailure
            || daily.networkTransportFailure;
        if (result.transportErrorCode == 0) {
            result.transportErrorCode = daily.transportErrorCode;
        }
        if (result.catalogSource.empty()) {
            result.catalogSource = daily.catalogSource;
        }

        // Single-source indexes keep one preferred row in daily.stations for
        // the fast legacy view, but includeAllFiles makes the raw
        // daily_files rows available through knowledgeBaseFiles.  Add those
        // candidates back before applying the range filters; otherwise a
        // request for a non-preferred RINEX version/interval could be lost
        // during the daily physical-station merge.
        if (!daily.knowledgeBaseFiles.empty()) {
            // Keep the preferred rows as well as the raw rows.  The raw view
            // is needed for an alternate RINEX family/interval, but it is not
            // guaranteed to contain every selected network (for example an
            // IGS all-files table does not contain the preferred NGS/EPN row).
            // Replacing the preferred vector here used to make a mixed
            // IGS+regional range query silently lose those stations.  Add
            // only distinct file identities; the physical-station merge
            // below will still reduce them to one row per station/day.
            std::unordered_set<std::string> seenFiles;
            seenFiles.reserve(
                daily.stations.size() + daily.knowledgeBaseFiles.size());
            const auto fileIdentity = [](const StationFile& station) {
                const std::string id = !station.stationId.empty()
                    ? ToUpperAscii(station.stationId)
                    : ToUpperAscii(station.fourCharId);
                return id + "\x1f" + station.fileName + "\x1f"
                    + std::to_string(static_cast<int>(station.format))
                    + "\x1f" + ToUpperAscii(station.interval);
            };
            for (const auto& station : daily.stations) {
                seenFiles.insert(fileIdentity(station));
            }
            for (auto& station : daily.knowledgeBaseFiles) {
                if (seenFiles.insert(fileIdentity(station)).second) {
                    daily.stations.push_back(std::move(station));
                }
            }
        }

        std::size_t stationOrdinal = 0;
        for (auto& station : daily.stations) {
            if ((stationOrdinal++ & 0xFFU) == 0U
                && stopToken.stop_requested()) {
                result.error = L"查询已取消。";
                return result;
            }
            appendStation(day, dayIndex, std::move(station));
        }
    }
    }

    std::vector<DatedStationFile> onlineHighrateFiles;
    if (materializePeriodFiles && !options.onlineSnapshots.empty()) {
        for (int dayIndex = 0; dayIndex < dayCount; ++dayIndex) {
            if (stopToken.stop_requested()) {
                result.error = L"查询已取消。";
                return result;
            }
            const auto day = AddDays(startDate, dayIndex);
            NetworkSelection onlineSelection{};
            onlineSelection.mask = 0;
            for (const auto& snapshot : options.onlineSnapshots) {
                if (ToRangeSysDays(day) > ToRangeSysDays(snapshot.lastKnownDate))
                    onlineSelection.mask |= snapshot.networks & selection.mask;
            }
            if (!wantedStations.empty()) {
                ObservationNetworkMask wantedMask = 0;
                for (const auto& station : wantedStations) wantedMask |= station.availableNetworks;
                if (wantedMask != 0) onlineSelection.mask &= wantedMask;
            }
            if (!onlineSelection.Any()) continue;
            if (weather.active && !weather.Contains(static_cast<int>(
                    ToRangeSysDays(day).time_since_epoch().count()))) continue;
            if (manifestProgress) {
                ObservationManifestProgress progress;
                progress.totalUnits = static_cast<std::size_t>(dayCount);
                progress.completedUnits = static_cast<std::size_t>(dayIndex);
                progress.currentSource = L"在线确认 " + FormatDate(day);
                manifestProgress(progress);
            }
            const bool wantsHighrate = preparedFilters.intervals.empty()
                || std::find(preparedFilters.intervals.begin(), preparedFilters.intervals.end(), "01S")
                    != preparedFilters.intervals.end();
            const bool onlyHighrate = preparedFilters.intervals.size() == 1
                && preparedFilters.intervals.front() == "01S";
            const auto igsMask = NetworkBit(ObservationNetwork::Igs) | NetworkBit(ObservationNetwork::Mgex);
            auto dailySelection = onlineSelection;
            if (onlyHighrate) dailySelection.mask &= ~igsMask;
            if (dailySelection.Any()) {
                // MGEX membership comes from the selected station snapshot,
                // not from the filename version. Include the shared archive
                // so an R2 fallback is still possible for an MGEX station.
                if ((dailySelection.mask & NetworkBit(ObservationNetwork::Mgex)) != 0)
                    dailySelection.mask |= NetworkBit(ObservationNetwork::Igs);
                auto daily = QueryDailyObservationFiles(day, dailySelection, stopToken,
                    KnowledgeBaseQueryMode::OnlineDownloadOnly, true);
                if (!daily.error.empty()) appendWarning(day, L"在线确认失败：" + daily.error);
                if (!daily.warning.empty()) appendWarning(day, daily.warning);
                const auto appendOnline = [&](StationFile station) {
                    for (const auto& wanted : wantedStations) {
                        if (station.stationId == wanted.stationId) {
                            station.availableNetworks |= wanted.availableNetworks & onlineSelection.mask;
                            break;
                        }
                    }
                    appendStation(day, dayIndex, std::move(station));
                };
                for (auto& station : daily.stations) appendOnline(std::move(station));
                for (auto& station : daily.knowledgeBaseFiles) appendOnline(std::move(station));
            }
            if (wantsHighrate && (onlineSelection.mask & igsMask) != 0) {
                std::vector<StationFile> highrateWanted;
                for (const auto& station : wantedStations) {
                    if ((station.availableNetworks & onlineSelection.mask & igsMask) != 0)
                        highrateWanted.push_back(station);
                }
                std::wstring warning;
                auto highrate = QueryOnlineHighrateFiles(day, highrateWanted, stopToken, warning);
                if (!warning.empty()) appendWarning(day, warning);
                for (auto& file : highrate) {
                    if (!MatchesPreparedRangeOptions(file, preparedFilters)) continue;
                    appendStation(day, dayIndex, file);
                    onlineHighrateFiles.push_back({day, 0, std::move(file)});
                }
            }
        }
    }
    if (collectFrequencyCandidates) {
        const auto verified = FilterObservationSearchDatedFiles(
            DefaultObservationSearchIndexPath(), selection, options,
            frequencyCandidates, true, stopToken);
        if (!verified.complete || !verified.error.empty()) {
            result.error = verified.error.empty()
                ? L"无法按文件格式和日期核验观测频点。" : verified.error;
            return result;
        }
        collectFrequencyCandidates = false;
        for (auto& candidate : frequencyCandidates) {
            const auto offset = ToRangeSysDays(candidate.date) - ToRangeSysDays(startDate);
            appendStation(candidate.date, static_cast<int>(offset.count()),
                std::move(candidate.station));
        }
        frequencyCandidates.clear();
        frequencyCandidates.shrink_to_fit();
    }
    result.coversDateRange = true;

    std::vector<std::uint8_t> dailyCoverageGoodDays;
    if (options.dailyCoverageMode != DailyCoverageMode::Ignore) {
        const auto hasDay = [&](const RangeAggregate& aggregate, const int dayIndex) {
            if (useDenseDayIndex) {
                if (materializePeriodFiles) {
                    return dayIndex >= 0
                        && static_cast<std::size_t>(dayIndex)
                            < aggregate.recordByDayDense.size()
                        && aggregate.recordByDayDense[static_cast<std::size_t>(dayIndex)] >= 0;
                }
                return dayIndex >= 0
                    && static_cast<std::size_t>(dayIndex) < aggregate.daySeenDense.size()
                    && aggregate.daySeenDense[static_cast<std::size_t>(dayIndex)] != 0;
            }
            return materializePeriodFiles
                ? aggregate.recordByDaySparse.contains(dayIndex)
                : aggregate.daySeenSparse.contains(dayIndex);
        };
        const int stationCount = static_cast<int>(aggregates.size());
        const int requestedStations = std::max(0, options.minimumDailyStations);
        const double requestedPercent = std::isfinite(options.minimumDailyStationPercent)
            ? std::clamp(options.minimumDailyStationPercent, 0.0, 100.0) : 0.0;
        std::vector<std::uint8_t> dailyGood(static_cast<std::size_t>(dayCount), 0);
        int goodDays = 0;
        for (int dayIndex = 0; dayIndex < dayCount; ++dayIndex) {
            int count = 0;
            for (const auto& aggregate : aggregates) {
                if (hasDay(aggregate, dayIndex)) ++count;
            }
            const bool keep = options.dailyCoverageMode == DailyCoverageMode::StationsAtLeast
                ? count >= requestedStations
                : (stationCount > 0
                    && 100.0 * static_cast<double>(count) / stationCount + 1e-9
                        >= requestedPercent);
            if (keep) { dailyGood[static_cast<std::size_t>(dayIndex)] = 1; ++goodDays; }
        }
        for (auto& aggregate : aggregates) {
            int available = 0;
            for (int dayIndex = 0; dayIndex < dayCount; ++dayIndex) {
                if (dailyGood[static_cast<std::size_t>(dayIndex)]
                    && hasDay(aggregate, dayIndex)) ++available;
            }
            aggregate.availableDays = available;
            aggregate.expectedDays = goodDays;
        }
        // The emission loop below consults this mask through the local copy.
        // Store it in the existing per-query scratch by filtering records at
        // emission time (see condition added below).
        dailyCoverageGoodDays = std::move(dailyGood);
    }

    const double ratioThreshold = std::isfinite(
            options.minimumCompletenessPercent)
        ? std::clamp(options.minimumCompletenessPercent, 0.0, 100.0)
        : 0.0;
    const int daysThreshold = std::max(0, options.minimumAvailableDays);
    const auto qualifies = [&](const RangeAggregate& aggregate) {
        if (aggregate.availableDays <= 0) return false;
        const int denominator = aggregate.expectedDays > 0
            ? aggregate.expectedDays : expectedDays;
        const double percent = 100.0 * static_cast<double>(
            aggregate.availableDays) / static_cast<double>(denominator);
        switch (options.completenessMode) {
            case CompletenessMode::RatioAtLeast:
                return percent + 1e-9 >= ratioThreshold;
            case CompletenessMode::DaysAtLeast:
                return aggregate.availableDays >= daysThreshold;
            case CompletenessMode::Ignore:
            default:
                return true;
        }
    };

    std::vector<std::size_t> qualified;
    qualified.reserve(aggregates.size());
    for (std::size_t index = 0; index < aggregates.size(); ++index) {
        if (qualifies(aggregates[index])) qualified.push_back(index);
    }
    const auto aggregateRepresentative = [&](const RangeAggregate& aggregate)
        -> const StationFile& {
        return materializePeriodFiles
            ? aggregate.records.front().station
            : aggregate.summaryRepresentative;
    };
    std::sort(
        qualified.begin(), qualified.end(),
        [&](const std::size_t left, const std::size_t right) {
            const auto& leftStation = aggregateRepresentative(aggregates[left]);
            const auto& rightStation = aggregateRepresentative(aggregates[right]);
            const std::string leftKey = leftStation.stationId.empty()
                ? leftStation.fourCharId : leftStation.stationId;
            const std::string rightKey = rightStation.stationId.empty()
                ? rightStation.fourCharId : rightStation.stationId;
            return leftKey < rightKey;
        });

    result.stations.reserve(qualified.size());
    std::vector<std::size_t> outputIndexByAggregate(
        aggregates.size(), std::numeric_limits<std::size_t>::max());
    if (materializePeriodFiles) {
        std::size_t periodFileReserve = 0;
        for (const auto aggregateIndex : qualified) {
            periodFileReserve += aggregates[aggregateIndex].records.size();
        }
        result.periodFiles.reserve(periodFileReserve);
    }

    // Build the visible station representatives in their stable station-ID
    // order first.  Daily rows are emitted in a separate date-major pass
    // below; that avoids an O(N log N) sort over millions of period files.
    for (std::size_t outputIndex = 0;
         outputIndex < qualified.size();
         ++outputIndex) {
        const auto aggregateIndex = qualified[outputIndex];
        auto& aggregate = aggregates[aggregateIndex];
        if (materializePeriodFiles && aggregate.records.empty()) continue;
        if (!materializePeriodFiles && !aggregate.hasSummaryRepresentative) {
            continue;
        }
        outputIndexByAggregate[aggregateIndex] = result.stations.size();
        StationFile representative;
        if (materializePeriodFiles) {
            const auto best = std::max_element(
                aggregate.records.begin(), aggregate.records.end(),
                [](const DatedStationFile& left,
                   const DatedStationFile& right) {
                    return DownloadPreference(left.station)
                        < DownloadPreference(right.station);
                });
            representative = best->station;
        } else {
            representative = std::move(aggregate.summaryRepresentative);
        }
        representative.selected = false;
        representative.inSelectionScope = true;
        representative.expectedDays = aggregate.expectedDays > 0
            ? aggregate.expectedDays : expectedDays;
        representative.availableDays = aggregate.availableDays;
        representative.availableRinexFormats = aggregate.availableFormats;
        representative.completenessPercent = 100.0 * static_cast<double>(
            aggregate.availableDays) / static_cast<double>(
                representative.expectedDays);
        // A station may be supplied by different selected networks on
        // different days.  Preserve the union of those memberships (and a
        // coordinate from any day) on the representative row used by the
        // map/list, just as the single-day merge does.
        for (const auto& dated : aggregate.records) {
            representative.availableNetworks =
                static_cast<ObservationNetworkMask>(
                    representative.availableNetworks
                    | dated.station.availableNetworks);
            if (!representative.hasCoordinate && dated.station.hasCoordinate) {
                representative.hasCoordinate = true;
                representative.latitude = dated.station.latitude;
                representative.longitude = dated.station.longitude;
            }
            if (representative.country.empty() && !dated.station.country.empty()) {
                representative.country = dated.station.country;
            }
            for (const auto& source : dated.station.availabilitySources) {
                if (std::find(
                        representative.availabilitySources.begin(),
                        representative.availabilitySources.end(), source)
                    == representative.availabilitySources.end()) {
                    representative.availabilitySources.push_back(source);
                }
            }
        }
        result.stations.push_back(std::move(representative));
    }

    // The input loop is chronological, but records are stored grouped by
    // station.  Emit one day at a time to keep the download list's historical
    // order without paying a large comparison-sort cost.
    for (int dayIndex = 0;
         materializePeriodFiles && dayIndex < dayCount;
         ++dayIndex) {
        if ((dayIndex & 0x0FU) == 0 && stopToken.stop_requested()) {
            result.error = L"查询已取消。";
            return result;
        }
        if (!dailyCoverageGoodDays.empty()
            && !dailyCoverageGoodDays[static_cast<std::size_t>(dayIndex)]) {
            continue;
        }
        const QueryDate day = AddDays(startDate, dayIndex);
        for (const auto aggregateIndex : qualified) {
            auto& aggregate = aggregates[aggregateIndex];
            const auto outputIndex = outputIndexByAggregate[aggregateIndex];
            if (outputIndex == std::numeric_limits<std::size_t>::max()) {
                continue;
            }
            std::size_t recordIndex = std::numeric_limits<std::size_t>::max();
            if (useDenseDayIndex) {
                const auto value = aggregate.recordByDayDense[
                    static_cast<std::size_t>(dayIndex)];
                if (value >= 0) recordIndex = static_cast<std::size_t>(value);
            } else {
                const auto found = aggregate.recordByDaySparse.find(dayIndex);
                if (found != aggregate.recordByDaySparse.end()) {
                    recordIndex = found->second;
                }
            }
            if (recordIndex == std::numeric_limits<std::size_t>::max()) continue;
            auto& dated = aggregate.records[recordIndex];
            dated.stationIndex = outputIndex;
            dated.station.selected = false;
            dated.station.inSelectionScope = true;
            dated.station.expectedDays = aggregate.expectedDays > 0
                ? aggregate.expectedDays : expectedDays;
            dated.station.availableDays = aggregate.availableDays;
            dated.station.completenessPercent =
                result.stations[outputIndex].completenessPercent;
            result.periodFiles.push_back(std::move(dated));
        }
    }
    if (!onlineHighrateFiles.empty()) {
        std::vector<DatedStationFile> expanded;
        for (auto& dated : result.periodFiles) {
            if (dated.station.interval != "01S") { expanded.push_back(std::move(dated)); continue; }
            std::vector<DatedStationFile*> matches;
            bool hasSegments = false;
            for (auto& file : onlineHighrateFiles) {
                if (FormatDate(file.date) != FormatDate(dated.date)
                    || file.station.stationId != dated.station.stationId
                    || file.station.format != dated.station.format) continue;
                matches.push_back(&file);
                hasSegments |= file.station.fileName.find(".tar") == std::string::npos;
            }
            if (matches.empty()) { expanded.push_back(std::move(dated)); continue; }
            for (auto* file : matches) {
                if (hasSegments && file->station.fileName.find(".tar") != std::string::npos) continue;
                file->stationIndex = dated.stationIndex;
                expanded.push_back(std::move(*file));
            }
        }
        result.periodFiles = std::move(expanded);
    }
    result.periodFilesDeferred = !materializePeriodFiles;
    result.filesWithoutCoordinates = static_cast<int>(std::count_if(
        result.stations.begin(),
        result.stations.end(),
        [](const StationFile& station) {
            return !station.hasCoordinate;
        }));
    if (result.stations.empty()) {
        result.warning = result.warning.empty()
            ? L"日期范围内没有测站满足当前筛选条件。"
            : result.warning + L"；日期范围内没有测站满足当前筛选条件。";
    }
    result.emptyResultIsComplete = true;
    return result;
}

QueryResult QueryObservationDateRange(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const NetworkSelection selection,
    const ObservationRangeOptions& options,
    const std::stop_token stopToken) {
    return QueryObservationDateRangeImpl(
        startDate,
        endDate,
        selection,
        options,
        stopToken,
        true,
        {},
        false,
        {});
}

QueryResult QueryObservationDateRangeSummary(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const NetworkSelection selection,
    const ObservationRangeOptions& options,
    const std::stop_token stopToken) {
    return QueryObservationDateRangeImpl(
        startDate,
        endDate,
        selection,
        options,
        stopToken,
        false,
        {},
        false,
        {});
}

QueryResult LoadObservationDateRangeFilesForStations(
    const QueryDate& startDate,
    const QueryDate& endDate,
    const NetworkSelection selection,
    const ObservationRangeOptions& options,
    const std::span<const StationFile> selectedStations,
    const std::stop_token stopToken,
    ObservationManifestProgressCallback progress) {
    if (selectedStations.empty()) {
        QueryResult result;
        result.date = startDate;
        result.selection = selection;
        result.periodStart = startDate;
        result.periodEnd = endDate;
        result.coversDateRange = true;
        result.directoryUrl = L"本地 GNSS 观测知识库（已选测站下载明细）";
        result.emptyResultIsComplete = true;
        return result;
    }
    return QueryObservationDateRangeImpl(
        startDate,
        endDate,
        selection,
        options,
        stopToken,
        true,
        selectedStations,
        true,
        progress);
}

QueryResult QueryObservationDateRange(
    const ObservationDateRange& range,
    const NetworkSelection selection,
    const ObservationRangeOptions& options,
    const std::stop_token stopToken) {
    return QueryObservationDateRange(
        range.start,
        range.end,
        selection,
        options,
        stopToken);
}

QueryResult QueryGpsWeekObservationFiles(
    const QueryDate& dateWithinWeek,
    const NetworkSelection selection,
    const std::stop_token stopToken,
    const KnowledgeBaseQueryMode mode,
    const ObservationRangeOptions& options) {
    QueryResult result;
    result.date = dateWithinWeek;
    result.selection = selection;
    result.coversFullGpsWeek = true;
    result.gpsWeek = GpsWeekNumber(dateWithinWeek);
    result.periodStart = GpsWeekStartForDate(dateWithinWeek);
    result.periodEnd = AddDays(result.periodStart, 6);
    result.directoryUrl = L"GPS周7天观测文件交集";

    std::vector<QueryResult> dailyResults;
    dailyResults.reserve(7);
    const auto hasIncompleteLocalIndexMessage = [](const std::wstring& text) {
        return text.find(L"尚未收录该日期") != std::wstring::npos
            || text.find(L"本地知识库失败") != std::wstring::npos
            || text.find(L"本地知识库读取失败") != std::wstring::npos
            || text.find(L"本地索引暂不可用") != std::wstring::npos
            || text.find(L"部分七源") != std::wstring::npos
            || text.find(L"旧版") != std::wstring::npos
            || text.find(L"最近7天暂定") != std::wstring::npos
            || text.find(L"暂定记录") != std::wstring::npos
            || text.find(L"待复扫") != std::wstring::npos;
    };
    for (int offset = 0; offset < 7; ++offset) {
        if (stopToken.stop_requested()) {
            result.error = L"查询已取消。";
            return result;
        }
        const QueryDate day = AddDays(result.periodStart, offset);
        auto daily = QueryDailyObservationFiles(
            day,
            selection,
            stopToken,
            mode,
            true);
        if (!daily.error.empty()) {
            result.error = std::format(
                L"GPS周 {} 查询未完成：{} 的数据不可用（{}）",
                result.gpsWeek,
                FormatDate(day),
                daily.error);
            return result;
        }
        // A GPS-week result promises a strict seven-day intersection.  A
        // provisional/partial local index cannot establish that promise, so
        // fail explicitly instead of presenting a deceptively small 7/7 list.
        if (hasIncompleteLocalIndexMessage(daily.warning)) {
            result.error = std::format(
                L"GPS周 {} 查询未完成：{} 的本地知识库尚未完成（{}）。",
                result.gpsWeek,
                FormatDate(day),
                daily.warning);
            return result;
        }
        if (!daily.warning.empty()) {
            if (!result.warning.empty()) {
                result.warning += L"；";
            }
            result.warning += FormatDate(day) + L"：" + daily.warning;
        }
        result.usedIgsLocalIndex = result.usedIgsLocalIndex
            || daily.usedIgsLocalIndex;
        result.usedLocalKnowledgeBase = result.usedLocalKnowledgeBase
            || daily.usedLocalKnowledgeBase;
        result.networkTransportFailure = result.networkTransportFailure
            || daily.networkTransportFailure;
        if (result.transportErrorCode == 0) {
            result.transportErrorCode = daily.transportErrorCode;
        }
        if (result.catalogSource.empty()) {
            result.catalogSource = daily.catalogSource;
        }
        // A daily query normally exposes one preferred row per station.  For
        // GPS-week filtering we need the raw file rows first, otherwise a
        // non-preferred RINEX family or sampling interval can disappear
        // before the user's predicate is evaluated.  Reduce the matching
        // rows back to one best candidate per physical station/day for the
        // seven-day intersection.
        std::vector<StationFile> raw;
        raw.reserve(
            daily.stations.size() + daily.knowledgeBaseFiles.size());
        std::unordered_set<std::string> rawKeys;
        rawKeys.reserve(
            daily.stations.size() + daily.knowledgeBaseFiles.size());
        const auto fileIdentity = [](const StationFile& candidate) {
            const std::string id = !candidate.stationId.empty()
                ? ToUpperAscii(candidate.stationId)
                : ToUpperAscii(candidate.fourCharId);
            return id + "\x1f" + candidate.fileName + "\x1f"
                + std::to_string(static_cast<int>(candidate.format))
                + "\x1f" + ToUpperAscii(candidate.interval);
        };
        const auto appendDistinct =
            [&raw, &rawKeys, &fileIdentity](StationFile candidate) {
            if (rawKeys.insert(fileIdentity(candidate)).second) {
                raw.push_back(std::move(candidate));
            }
        };
        for (auto& candidate : daily.stations) {
            appendDistinct(std::move(candidate));
        }
        for (auto& candidate : daily.knowledgeBaseFiles) {
            appendDistinct(std::move(candidate));
        }
        std::vector<StationFile> filtered;
        std::unordered_map<std::string, std::vector<std::size_t>> byFourChar;
        filtered.reserve(raw.size());
        byFourChar.reserve(raw.size());
        for (auto& candidate : raw) {
            if (!MatchesObservationRangeOptions(candidate, options)) {
                continue;
            }
            MergeStation(filtered, byFourChar, std::move(candidate));
        }
        daily.stations = std::move(filtered);
        dailyResults.push_back(std::move(daily));
    }

    // A GPS-week result is intrinsically 7/7.  Reject thresholds that ask for
    // more than the available seven calendar days rather than returning a
    // misleading partially qualified list.
    if ((options.completenessMode == CompletenessMode::DaysAtLeast
            && options.minimumAvailableDays > 7)
        || (options.completenessMode == CompletenessMode::RatioAtLeast
            && std::isfinite(options.minimumCompletenessPercent)
            && options.minimumCompletenessPercent > 100.0)) {
        result.warning = L"当前完整性阈值高于 GPS 周可提供的 7/7，未找到符合条件的测站。";
        result.error = result.warning;
        return result;
    }

    if (dailyResults.empty() || dailyResults.front().stations.empty()) {
        result.error = std::format(
            L"GPS周 {}（{} 至 {}）没有可用于7天交集的测站。",
            result.gpsWeek,
            FormatDate(result.periodStart),
            FormatDate(result.periodEnd));
        return result;
    }

    for (const auto& base : dailyResults.front().stations) {
        std::vector<const StationFile*> matches;
        matches.reserve(7);
        matches.push_back(&base);
        bool availableEveryDay = true;
        for (std::size_t day = 1; day < dailyResults.size(); ++day) {
            const auto found = std::find_if(
                dailyResults[day].stations.begin(),
                dailyResults[day].stations.end(),
                [&](const StationFile& candidate) {
                    return SamePhysicalStation(base, candidate);
                });
            if (found == dailyResults[day].stations.end()) {
                availableEveryDay = false;
                break;
            }
            matches.push_back(&*found);
        }
        if (!availableEveryDay) {
            continue;
        }

        StationFile representative = base;
        representative.selected = false;
        representative.inSelectionScope = true;
        // A GPS-week result is, by definition, the seven-day intersection.
        // Populate the same availability fields as a date-range result so a
        // completeness filter of 100% does not accidentally hide every
        // station in the legacy week view.
        representative.expectedDays = 7;
        representative.availableDays = 7;
        representative.completenessPercent = 100.0;
        for (const auto* match : matches) {
            representative.availableNetworks =
                static_cast<ObservationNetworkMask>(
                    representative.availableNetworks
                    | match->availableNetworks);
        }
        const std::size_t stationIndex = result.stations.size();
        result.stations.push_back(std::move(representative));
        for (std::size_t day = 0; day < matches.size(); ++day) {
            StationFile file = *matches[day];
            file.selected = false;
            file.inSelectionScope = true;
            file.expectedDays = 7;
            file.availableDays = 7;
            file.completenessPercent = 100.0;
            result.periodFiles.push_back({
                AddDays(result.periodStart, static_cast<int>(day)),
                stationIndex,
                std::move(file)});
        }
    }

    if (result.stations.empty()) {
        result.error = std::format(
            L"GPS周 {}（{} 至 {}）每天都有观测数据的共同测站为0个。",
            result.gpsWeek,
            FormatDate(result.periodStart),
            FormatDate(result.periodEnd));
        return result;
    }
    result.filesWithoutCoordinates = static_cast<int>(std::count_if(
        result.stations.begin(),
        result.stations.end(),
        [](const StationFile& station) {
            return !station.hasCoordinate;
        }));
    result.emptyResultIsComplete = false;
    return result;
}
