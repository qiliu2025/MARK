#include "MapView.h"
#include "CountryFlags.h"

#include <windowsx.h>
#include <objidl.h>  // IStream/PROPID declarations required by GDI+ headers.

// GDI+ is part of Windows.  It gives the native map a small, dependency-free
// rendering upgrade (anti-aliased coastlines, labels, and station markers)
// without changing the public Win32 control API or requiring a new runtime.
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include "../resources/resource.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using Gdiplus::Color;
using Gdiplus::Font;
using Gdiplus::FontFamily;
using Gdiplus::FontStyle;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::LinearGradientBrush;
using Gdiplus::Pen;
using Gdiplus::PointF;
using Gdiplus::RectF;
using Gdiplus::SolidBrush;
using Gdiplus::StringAlignment;
using Gdiplus::StringFormat;
using Gdiplus::StringFormatFlags;
using Gdiplus::TextRenderingHint;
using Gdiplus::Unit;

// Keep GDI+ active until process exit to avoid racing WM_PAINT during teardown.
std::once_flag g_gdiplusStartupOnce;
ULONG_PTR g_gdiplusToken = 0;

bool EnsureGdiplus() {
    std::call_once(g_gdiplusStartupOnce, [] {
        Gdiplus::GdiplusStartupInput startupInput;
        (void)Gdiplus::GdiplusStartup(
            &g_gdiplusToken,
            &startupInput,
            nullptr);
    });
    return g_gdiplusToken != 0;
}

void ConfigureGraphics(Graphics& graphics) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetTextRenderingHint(TextRenderingHint::TextRenderingHintAntiAliasGridFit);
}

PointF ToPointF(const POINT point) {
    return PointF(
        static_cast<Gdiplus::REAL>(point.x),
        static_cast<Gdiplus::REAL>(point.y));
}

RectF ToRectF(const RECT& rectangle) {
    return RectF(
        static_cast<Gdiplus::REAL>(rectangle.left),
        static_cast<Gdiplus::REAL>(rectangle.top),
        static_cast<Gdiplus::REAL>(std::max<LONG>(0, rectangle.right - rectangle.left)),
        static_cast<Gdiplus::REAL>(std::max<LONG>(0, rectangle.bottom - rectangle.top)));
}

Color FromColorRef(const COLORREF color, const BYTE alpha = 255) {
    return Color(
        alpha,
        GetRValue(color),
        GetGValue(color),
        GetBValue(color));
}

constexpr wchar_t kMapClassName[] = L"IGSDownloaderMapView";
constexpr COLORREF kOceanColor = RGB(243, 248, 252);
constexpr COLORREF kLandColor = RGB(222, 230, 235);
constexpr COLORREF kCoastColor = RGB(172, 185, 194);
constexpr COLORREF kCountryBorderColor = RGB(153, 166, 177);
constexpr COLORREF kCountryLabelColor = RGB(67, 82, 94);
constexpr COLORREF kGridColor = RGB(224, 231, 236);
constexpr COLORREF kStationColor = RGB(15, 132, 158);
constexpr COLORREF kSelectedColor = RGB(241, 111, 55);
constexpr COLORREF kHoverColor = RGB(20, 82, 122);
constexpr double kMinimumMapZoom = 1.0;
constexpr double kMaximumMapZoom = 24.0;

struct GeoPoint {
    double longitude = 0.0;
    double latitude = 0.0;
};

using GeoRing = std::vector<GeoPoint>;

struct CountryShape {
    std::wstring name;
    std::wstring iso;
    std::vector<GeoRing> rings;
    double minimumLongitude = 180.0;
    double maximumLongitude = -180.0;
    double minimumLatitude = 90.0;
    double maximumLatitude = -90.0;
    double labelLongitude = 0.0;
    double labelLatitude = 0.0;
    double area = 0.0;
    int labelRank = 6;
    bool hasLabel = false;
    bool labelOnly = false;
    bool indicator = false;
    bool disputed = false;
    bool island = false;
    bool boundaryLine = false;
    bool controlLine = false;
    bool coastline = false;
    double minimumLabelZoom = 1.0;
};

struct MapState {
    const std::vector<StationFile>* stations = nullptr;
    std::vector<GeoRing> landRings;
    std::vector<CountryShape> countries;
    double zoom = 1.0;
    double centerLongitude = 0.0;
    double centerLatitude = 0.0;
    bool trackingMouse = false;
    bool mouseDown = false;
    bool dragging = false;
    bool rectangleSelectionMode = false;
    bool selectingRectangle = false;
    POINT downPoint{};
    POINT lastPoint{};
    int hoverIndex = -1;

    // The coastlines, country borders/labels, grid, and footer do not change
    // when only a station is hovered or selected.  Keep those relatively
    // expensive GDI+ passes in a compatible bitmap and redraw just the
    // dynamic station layer on subsequent WM_PAINT messages.  The cache is
    // owned by the map window and is always rebuilt on a view/size/DPI change.
    HBITMAP staticMapBitmap = nullptr;
    int staticMapWidth = 0;
    int staticMapHeight = 0;
    RECT staticMapRect{};
    double staticZoom = 0.0;
    double staticCenterLongitude = 0.0;
    double staticCenterLatitude = 0.0;
    int staticDpi = 0;
    bool staticRectangleSelectionMode = false;
};

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (length <= 0) {
        return std::wstring(text.begin(), text.end());
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length);
    return result;
}

std::string LoadResourceText(const int resourceId) {
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
    const void* bytes = LockResource(loaded);
    if (bytes == nullptr || size == 0) {
        return {};
    }
    return std::string(static_cast<const char*>(bytes), size);
}

bool StartsNumber(const char value) {
    return value == '-' || value == '+' || value == '.'
        || (value >= '0' && value <= '9');
}

std::vector<GeoRing> ParseGeoJsonRings(const std::string& json) {
    // Polygon nests rings of [lon, lat] pairs; MultiPolygon adds one level.
    // Recursive parsing distinguishes point endings from ring endings.
    std::vector<GeoRing> rings;
    std::size_t cursor = 0;

    const auto skipWhitespace = [&](std::size_t& position) {
        while (position < json.size()
               && std::isspace(static_cast<unsigned char>(json[position])) != 0) {
            ++position;
        }
    };

    const auto readNumber = [&](std::size_t& position, double& value) {
        skipWhitespace(position);
        if (position >= json.size() || !StartsNumber(json[position])) {
            return false;
        }
        char* end = nullptr;
        const double parsed = std::strtod(json.c_str() + position, &end);
        if (end == json.c_str() + position || !std::isfinite(parsed)) {
            return false;
        }
        position = static_cast<std::size_t>(end - json.c_str());
        value = parsed;
        return true;
    };

    std::function<bool(std::size_t&)> readArray;
    readArray = [&](std::size_t& position) {
        skipWhitespace(position);
        if (position >= json.size() || json[position] != '[') {
            return false;
        }
        ++position;

        GeoRing ring;
        bool containsCoordinatePair = false;
        while (position < json.size()) {
            skipWhitespace(position);
            if (position >= json.size()) {
                return false;
            }
            if (json[position] == ']') {
                ++position;
                if (containsCoordinatePair && ring.size() >= 2) {
                    rings.push_back(std::move(ring));
                }
                return true;
            }
            if (json[position] == ',') {
                ++position;
                continue;
            }
            if (json[position] != '[') {
                // Geometry coordinates are arrays only.  Skip an unexpected
                // scalar so a malformed feature cannot make the whole map
                // parser loop forever.
                ++position;
                continue;
            }

            // A coordinate pair starts with a number immediately after its
            // opening bracket.  If it starts with another bracket, recurse
            // into a polygon/ring level instead.
            std::size_t probe = position + 1U;
            skipWhitespace(probe);
            if (probe < json.size() && StartsNumber(json[probe])) {
                double longitude = 0.0;
                double latitude = 0.0;
                if (!readNumber(probe, longitude)) {
                    ++position;
                    continue;
                }
                skipWhitespace(probe);
                if (probe >= json.size() || json[probe] != ',') {
                    ++position;
                    continue;
                }
                ++probe;
                if (!readNumber(probe, latitude)) {
                    ++position;
                    continue;
                }
                // GeoJSON permits an optional third altitude ordinate.  It
                // is irrelevant to a 2-D map; consume the rest of this point
                // until its own closing bracket.
                while (probe < json.size()
                       && json[probe] != ']') {
                    ++probe;
                }
                if (probe >= json.size()) {
                    return false;
                }
                ++probe;
                if (longitude >= -180.001 && longitude <= 180.001
                    && latitude >= -90.001 && latitude <= 90.001) {
                    ring.push_back(GeoPoint{longitude, latitude});
                    containsCoordinatePair = true;
                }
                position = probe;
                continue;
            }

            if (!readArray(position)) {
                return false;
            }
        }
        return false;
    };

    (void)readArray(cursor);
    return rings;
}

std::size_t FindJsonContainerEnd(
    const std::string& json,
    const std::size_t start,
    const char opening,
    const char closing) {
    if (start >= json.size() || json[start] != opening) {
        return std::string::npos;
    }
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t index = start; index < json.size(); ++index) {
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
        } else if (character == opening) {
            ++depth;
        } else if (character == closing) {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::string::npos;
}

int HexDigit(const char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

void AppendUtf8(std::string& output, const std::uint32_t codePoint) {
    if (codePoint <= 0x7FU) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    } else if (codePoint <= 0x10FFFFU) {
        output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }
}

std::string DecodeJsonString(
    const std::string& json,
    const std::size_t quotePosition) {
    if (quotePosition >= json.size() || json[quotePosition] != '"') {
        return {};
    }
    std::string value;
    value.reserve(32);
    for (std::size_t index = quotePosition + 1; index < json.size(); ++index) {
        const char character = json[index];
        if (character == '"') {
            break;
        }
        if (character != '\\') {
            value.push_back(character);
            continue;
        }
        if (++index >= json.size()) {
            break;
        }
        const char escaped = json[index];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            value.push_back(escaped);
            break;
        case 'b':
            value.push_back('\b');
            break;
        case 'f':
            value.push_back('\f');
            break;
        case 'n':
            value.push_back('\n');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case 't':
            value.push_back('\t');
            break;
        case 'u': {
            if (index + 4 >= json.size()) {
                break;
            }
            std::uint32_t codePoint = 0;
            bool valid = true;
            for (std::size_t digit = 1; digit <= 4; ++digit) {
                const int valueOfDigit = HexDigit(json[index + digit]);
                if (valueOfDigit < 0) {
                    valid = false;
                    break;
                }
                codePoint = (codePoint << 4U)
                    | static_cast<std::uint32_t>(valueOfDigit);
            }
            if (valid) {
                index += 4;
                // Join a UTF-16 surrogate pair when a producer escaped a
                // non-BMP character instead of writing UTF-8 directly.
                if (codePoint >= 0xD800U && codePoint <= 0xDBFFU
                    && index + 6 < json.size()
                    && json[index + 1] == '\\'
                    && json[index + 2] == 'u') {
                    std::uint32_t low = 0;
                    bool lowValid = true;
                    for (std::size_t digit = 3; digit <= 6; ++digit) {
                        const int valueOfDigit = HexDigit(json[index + digit]);
                        if (valueOfDigit < 0) {
                            lowValid = false;
                            break;
                        }
                        low = (low << 4U)
                            | static_cast<std::uint32_t>(valueOfDigit);
                    }
                    if (lowValid && low >= 0xDC00U && low <= 0xDFFFU) {
                        codePoint = 0x10000U
                            + ((codePoint - 0xD800U) << 10U)
                            + (low - 0xDC00U);
                        index += 6;
                    }
                }
                AppendUtf8(value, codePoint);
            }
            break;
        }
        default:
            value.push_back(escaped);
            break;
        }
    }
    return value;
}

std::string JsonStringProperty(
    const std::string& json,
    const char* const key) {
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t searchPosition = 0;
    while (searchPosition < json.size()) {
        const std::size_t keyPosition = json.find(needle, searchPosition);
        if (keyPosition == std::string::npos) {
            return {};
        }
        std::size_t valuePosition = keyPosition + needle.size();
        while (valuePosition < json.size()
               && std::isspace(static_cast<unsigned char>(json[valuePosition])) != 0) {
            ++valuePosition;
        }
        if (valuePosition < json.size() && json[valuePosition] == ':') {
            ++valuePosition;
            while (valuePosition < json.size()
                   && std::isspace(static_cast<unsigned char>(json[valuePosition])) != 0) {
                ++valuePosition;
            }
            if (valuePosition < json.size() && json[valuePosition] == '"') {
                return DecodeJsonString(json, valuePosition);
            }
            return {};
        }
        searchPosition = keyPosition + needle.size();
    }
    return {};
}

bool JsonNumberProperty(
    const std::string& json,
    const char* const key,
    double& value) {
    const std::string needle = std::string("\"") + key + "\"";
    const std::size_t keyPosition = json.find(needle);
    if (keyPosition == std::string::npos) {
        return false;
    }
    std::size_t valuePosition = keyPosition + needle.size();
    while (valuePosition < json.size()
           && std::isspace(static_cast<unsigned char>(json[valuePosition])) != 0) {
        ++valuePosition;
    }
    if (valuePosition >= json.size() || json[valuePosition] != ':') {
        return false;
    }
    ++valuePosition;
    while (valuePosition < json.size()
           && std::isspace(static_cast<unsigned char>(json[valuePosition])) != 0) {
        ++valuePosition;
    }
    if (valuePosition >= json.size() || !StartsNumber(json[valuePosition])) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(json.c_str() + valuePosition, &end);
    if (end == json.c_str() + valuePosition || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

std::vector<CountryShape> ParseGeoJsonCountries(const std::string& json) {
    std::vector<CountryShape> countries;
    const std::size_t featuresKey = json.find("\"features\"");
    if (featuresKey == std::string::npos) {
        return countries;
    }
    const std::size_t featuresStart = json.find('[', featuresKey);
    if (featuresStart == std::string::npos) {
        return countries;
    }
    const std::size_t featuresEnd = FindJsonContainerEnd(
        json,
        featuresStart,
        '[',
        ']');
    if (featuresEnd == std::string::npos) {
        return countries;
    }

    std::size_t objectStart = featuresStart + 1;
    while (objectStart < featuresEnd) {
        if (json[objectStart] != '{') {
            ++objectStart;
            continue;
        }
        const std::size_t objectEnd = FindJsonContainerEnd(
            json,
            objectStart,
            '{',
            '}');
        if (objectEnd == std::string::npos || objectEnd > featuresEnd) {
            break;
        }
        const std::string object = json.substr(
            objectStart,
            objectEnd - objectStart + 1);
        const std::size_t geometryKey = object.find("\"geometry\"");
        const std::size_t coordinatesKey = geometryKey == std::string::npos
            ? std::string::npos
            : object.find("\"coordinates\"", geometryKey);
        if (coordinatesKey != std::string::npos) {
            const std::size_t coordinatesStart = object.find('[', coordinatesKey);
            if (coordinatesStart != std::string::npos) {
                const std::size_t coordinatesEnd = FindJsonContainerEnd(
                    object,
                    coordinatesStart,
                    '[',
                    ']');
                if (coordinatesEnd != std::string::npos) {
                    CountryShape country;
                    const std::string chineseName = JsonStringProperty(object, "NAME_ZH");
                    const std::string englishName = JsonStringProperty(object, "NAME");
                    country.name = Utf8ToWide(
                        chineseName.empty() ? englishName : chineseName);
                    const auto prescribedName = JsonStringProperty(object, "MARK_NAME_ZH");
                    if (!prescribedName.empty()) country.name = Utf8ToWide(prescribedName);
                    const auto kind = JsonStringProperty(object, "MARK_KIND");
                    country.labelOnly = kind == "region" || kind == "island"
                        || JsonStringProperty(object, "MARK_DRAW") == "label";
                    country.boundaryLine = kind == "boundary";
                    country.coastline = kind == "coastline";
                    country.indicator = kind == "indicator" || country.boundaryLine || country.coastline;
                    country.island = kind == "island";
                    const auto boundaryType = JsonStringProperty(object, "MARK_BOUNDARY_TYPE");
                    country.controlLine = boundaryType == "control";
                    country.disputed = JsonStringProperty(object, "MARK_STATUS") == "disputed"
                        || boundaryType == "undetermined" || country.controlLine;
                    country.iso = Utf8ToWide(JsonStringProperty(object, "ISO_A3"));
                    std::wstring mapIso = country.iso;
                    std::transform(mapIso.begin(), mapIso.end(), mapIso.begin(),
                        [](const wchar_t character) {
                            return static_cast<wchar_t>(std::towupper(character));
                        });
                    // Keep the island geometry visible, while presenting it
                    // as "中国台湾" and canonicalizing its country code to
                    // CHN in the map layer.
                    if (mapIso == L"TWN" || mapIso == L"ROC") {
                        // Keep the island geometry visible, but present it in
                        // the UI as part of China rather than as a separate
                        // country entry.
                        country.name = L"中国台湾";
                        country.iso = L"CHN";
                    }
                    country.rings = ParseGeoJsonRings(object.substr(
                        coordinatesStart,
                        coordinatesEnd - coordinatesStart + 1));
                    if (country.island || (country.labelOnly && country.rings.empty())) {
                        double longitude = 0.0, latitude = 0.0;
                        if (JsonNumberProperty(object, "LABEL_X", longitude)
                            && JsonNumberProperty(object, "LABEL_Y", latitude)) {
                            // Internal repeated points provide label/symbol bounds.
                            // They never provide a political or land surface.
                            country.rings = {{ {longitude, latitude}, {longitude, latitude}, {longitude, latitude} }};
                        }
                    }
                    if (!country.rings.empty()) {
                        double largestRingArea = 0.0;
                        const GeoRing* largestRing = nullptr;
                        for (const auto& ring : country.rings) {
                            if (ring.size() < 2) {
                                continue;
                            }
                            double ringArea = 0.0;
                            for (std::size_t point = 0; point < ring.size(); ++point) {
                                const auto& current = ring[point];
                                const auto& next = ring[(point + 1) % ring.size()];
                                ringArea += current.longitude * next.latitude
                                    - next.longitude * current.latitude;
                                country.minimumLongitude = std::min(
                                    country.minimumLongitude,
                                    current.longitude);
                                country.maximumLongitude = std::max(
                                    country.maximumLongitude,
                                    current.longitude);
                                country.minimumLatitude = std::min(
                                    country.minimumLatitude,
                                    current.latitude);
                                country.maximumLatitude = std::max(
                                    country.maximumLatitude,
                                    current.latitude);
                            }
                            ringArea = std::abs(ringArea) * 0.5;
                            country.area += ringArea;
                            if (ringArea > largestRingArea) {
                                largestRingArea = ringArea;
                                largestRing = &ring;
                            }
                        }
                        double labelLongitude = 0.0;
                        double labelLatitude = 0.0;
                        if (JsonNumberProperty(object, "LABEL_X", labelLongitude)
                            && JsonNumberProperty(object, "LABEL_Y", labelLatitude)
                            && labelLongitude >= -180.0 && labelLongitude <= 180.0
                            && labelLatitude >= -90.0 && labelLatitude <= 90.0) {
                            country.labelLongitude = labelLongitude;
                            country.labelLatitude = labelLatitude;
                            country.hasLabel = true;
                        } else if (largestRing != nullptr && !largestRing->empty()) {
                            for (const auto& point : *largestRing) {
                                country.labelLongitude += point.longitude;
                                country.labelLatitude += point.latitude;
                            }
                            country.labelLongitude /= static_cast<double>(largestRing->size());
                            country.labelLatitude /= static_cast<double>(largestRing->size());
                        }
                        double labelRank = 6.0;
                        if (JsonNumberProperty(object, "LABELRANK", labelRank)) {
                            country.labelRank = std::clamp(
                                static_cast<int>(std::lround(labelRank)),
                                0,
                                20);
                        }
                        if (country.labelOnly) JsonNumberProperty(object, "MARK_LABEL_AREA", country.area);
                        JsonNumberProperty(object, "MIN_LABEL", country.minimumLabelZoom);
                        if (!country.name.empty() || country.indicator) {
                            countries.push_back(std::move(country));
                        }
                    }
                }
            }
        }
        objectStart = objectEnd + 1;
    }
    return countries;
}

RECT GetMapRect(const HWND window) {
    RECT rectangle{};
    GetClientRect(window, &rectangle);
    rectangle.left += 14;
    rectangle.right -= 14;
    rectangle.top += 14;
    rectangle.bottom -= 42;
    if (rectangle.right < rectangle.left) {
        rectangle.right = rectangle.left;
    }
    if (rectangle.bottom < rectangle.top) {
        rectangle.bottom = rectangle.top;
    }
    return rectangle;
}

POINT Project(const MapState& state, const RECT& map, const double lon, const double lat) {
    const double width = static_cast<double>(map.right - map.left);
    const double height = static_cast<double>(map.bottom - map.top);
    const double scaleX = width / 360.0 * state.zoom;
    const double scaleY = height / 180.0 * state.zoom;
    return POINT{
        static_cast<LONG>(std::lround(
            (map.left + map.right) / 2.0
            + (lon - state.centerLongitude) * scaleX)),
        static_cast<LONG>(std::lround(
            (map.top + map.bottom) / 2.0
            - (lat - state.centerLatitude) * scaleY))
    };
}

GeoPoint Unproject(const MapState& state, const RECT& map, const POINT point) {
    const double width = static_cast<double>(map.right - map.left);
    const double height = static_cast<double>(map.bottom - map.top);
    const double scaleX = width / 360.0 * state.zoom;
    const double scaleY = height / 180.0 * state.zoom;
    if (scaleX <= 0.0 || scaleY <= 0.0) {
        return {};
    }
    return {
        std::clamp(
            state.centerLongitude
                + (point.x - (map.left + map.right) / 2.0) / scaleX,
            -180.0,
            180.0),
        std::clamp(
            state.centerLatitude
                - (point.y - (map.top + map.bottom) / 2.0) / scaleY,
            -90.0,
            90.0)};
}

void ClampCenter(MapState& state) {
    const double maxLongitude = 180.0 - 180.0 / state.zoom;
    const double maxLatitude = 90.0 - 90.0 / state.zoom;
    state.centerLongitude = std::clamp(
        state.centerLongitude,
        -std::max(0.0, maxLongitude),
        std::max(0.0, maxLongitude));
    state.centerLatitude = std::clamp(
        state.centerLatitude,
        -std::max(0.0, maxLatitude),
        std::max(0.0, maxLatitude));
}

int HitTestStation(
    const MapState& state,
    const RECT& map,
    const POINT cursor,
    const int radius) {
    if (state.stations == nullptr) {
        return -1;
    }
    int best = -1;
    long bestDistanceSquared = radius * radius + 1;
    for (std::size_t index = 0; index < state.stations->size(); ++index) {
        const auto& station = (*state.stations)[index];
        if (!station.inSelectionScope || !station.hasCoordinate) {
            continue;
        }
        const POINT point = Project(
            state,
            map,
            station.longitude,
            station.latitude);
        const long dx = point.x - cursor.x;
        const long dy = point.y - cursor.y;
        const long distanceSquared = dx * dx + dy * dy;
        if (distanceSquared <= radius * radius
            && distanceSquared < bestDistanceSquared) {
            best = static_cast<int>(index);
            bestDistanceSquared = distanceSquared;
        }
    }
    return best;
}

void DrawGrid(HDC device, const MapState& state, const RECT& map) {
    if (!EnsureGdiplus()) {
        const HPEN pen = CreatePen(PS_SOLID, 1, kGridColor);
        const HGDIOBJ oldPen = SelectObject(device, pen);
        for (int longitude = -150; longitude <= 150; longitude += 30) {
            const POINT top = Project(state, map, longitude, 90.0);
            const POINT bottom = Project(state, map, longitude, -90.0);
            MoveToEx(device, top.x, top.y, nullptr);
            LineTo(device, bottom.x, bottom.y);
        }
        for (int latitude = -60; latitude <= 60; latitude += 30) {
            const POINT left = Project(state, map, -180.0, latitude);
            const POINT right = Project(state, map, 180.0, latitude);
            MoveToEx(device, left.x, left.y, nullptr);
            LineTo(device, right.x, right.y);
        }
        SelectObject(device, oldPen);
        DeleteObject(pen);
        return;
    }

    Graphics graphics(device);
    ConfigureGraphics(graphics);
    graphics.SetClip(ToRectF(map));
    Pen pen(FromColorRef(kGridColor, 185), 1.0F);

    // Use a denser grid only when it conveys useful scale at close zoom.  The
    // global view remains quiet while regional navigation gets 10-degree
    // reference lines instead of a nearly empty background.
    const int step = state.zoom >= 5.0 ? 10 : (state.zoom >= 2.0 ? 15 : 30);
    for (int longitude = -180 + step; longitude < 180; longitude += step) {
        const PointF top = ToPointF(Project(state, map, longitude, 90.0));
        const PointF bottom = ToPointF(Project(state, map, longitude, -90.0));
        graphics.DrawLine(&pen, top, bottom);
    }
    for (int latitude = -90 + step; latitude < 90; latitude += step) {
        const PointF left = ToPointF(Project(state, map, -180.0, latitude));
        const PointF right = ToPointF(Project(state, map, 180.0, latitude));
        graphics.DrawLine(&pen, left, right);
    }
}

// Defined below the land renderer because it is also shared by the country
// boundary and label passes.
bool CountryIntersectsView(const MapState& state, const CountryShape& country);

void DrawLand(HDC device, const MapState& state, const RECT& map) {
    if (!EnsureGdiplus()) {
        // The fallback keeps the map usable on a system where GDI+ startup
        // fails for an unusual reason.  Normal Windows installations use the
        // anti-aliased path below.
        const HPEN coastPen = CreatePen(PS_SOLID, 1, kCoastColor);
        const HBRUSH landBrush = CreateSolidBrush(kLandColor);
        const HGDIOBJ oldPen = SelectObject(device, coastPen);
        const HGDIOBJ oldBrush = SelectObject(device, landBrush);
        SaveDC(device);
        IntersectClipRect(device, map.left, map.top, map.right, map.bottom);
        for (const auto& ring : state.landRings) {
            if (ring.size() < 3) {
                continue;
            }
            std::vector<POINT> points;
            points.reserve(ring.size());
            for (const auto& coordinate : ring) {
                points.push_back(Project(
                    state,
                    map,
                    coordinate.longitude,
                    coordinate.latitude));
            }
            Polygon(device, points.data(), static_cast<int>(points.size()));
        }
        RestoreDC(device, -1);
        SelectObject(device, oldPen);
        SelectObject(device, oldBrush);
        DeleteObject(coastPen);
        DeleteObject(landBrush);
        return;
    }

    Graphics graphics(device);
    ConfigureGraphics(graphics);
    graphics.SetClip(ToRectF(map));
    SolidBrush landBrush(FromColorRef(kLandColor));

    // The reference map uses 50m physical land at every zoom. Its compatibility
    // resource filename remains ne_110m_land.geojson. Older country-polygon
    // maps still use the fallback land only at global / Antarctic views.
    GraphicsPath coarseLand(Gdiplus::FillModeWinding);
    const bool referenceMap = std::any_of(state.countries.begin(), state.countries.end(),
        [](const CountryShape& country) { return country.boundaryLine; });
    const bool needCoarseLand = referenceMap || state.zoom < 1.8
        || state.centerLatitude - 90.0 / state.zoom < -55.0;
    if (needCoarseLand) {
        for (const auto& ring : state.landRings) {
            if (ring.size() < 3) {
                continue;
            }
            std::vector<PointF> segment;
            segment.reserve(ring.size());
            const auto flush = [&]() {
                if (segment.size() >= 3) {
                    coarseLand.AddPolygon(
                        segment.data(),
                        static_cast<INT>(segment.size()));
                }
                segment.clear();
            };
            for (std::size_t index = 0; index < ring.size(); ++index) {
                if (index > 0
                    && std::abs(
                        ring[index].longitude - ring[index - 1].longitude) > 180.0) {
                    flush();
                }
                segment.push_back(ToPointF(Project(
                    state,
                    map,
                    ring[index].longitude,
                    ring[index].latitude)));
            }
            flush();
        }
        graphics.FillPath(&landBrush, &coarseLand);
    }

    const std::size_t vertexStride = state.zoom < 1.5
        ? 4U
        : (state.zoom < 3.0 ? 2U : 1U);
    for (const auto& country : state.countries) {
        if (country.labelOnly || country.indicator || !CountryIntersectsView(state, country)) {
            continue;
        }
        GraphicsPath countryPath(Gdiplus::FillModeWinding);
        for (const auto& ring : country.rings) {
            if (ring.size() < 3) {
                continue;
            }
            std::vector<PointF> segment;
            segment.reserve(ring.size() / vertexStride + 2);
            const auto flush = [&]() {
                if (segment.size() >= 3) {
                    countryPath.AddPolygon(
                        segment.data(),
                        static_cast<INT>(segment.size()));
                }
                segment.clear();
            };
            for (std::size_t index = 0; index < ring.size(); ++index) {
                if (index > 0
                    && std::abs(
                        ring[index].longitude - ring[index - 1].longitude) > 180.0) {
                    flush();
                }
                if (index != 0 && index + 1 != ring.size()
                    && index % vertexStride != 0) {
                    continue;
                }
                segment.push_back(ToPointF(Project(
                    state,
                    map,
                    ring[index].longitude,
                    ring[index].latitude)));
            }
            flush();
        }
        graphics.FillPath(&landBrush, &countryPath);
    }
}

bool CountryIntersectsView(
    const MapState& state,
    const CountryShape& country) {
    const double halfLongitude = 180.0 / state.zoom;
    const double halfLatitude = 90.0 / state.zoom;
    const double viewMinimumLongitude = state.centerLongitude - halfLongitude;
    const double viewMaximumLongitude = state.centerLongitude + halfLongitude;
    const double viewMinimumLatitude = state.centerLatitude - halfLatitude;
    const double viewMaximumLatitude = state.centerLatitude + halfLatitude;
    if (country.maximumLatitude < viewMinimumLatitude
        || country.minimumLatitude > viewMaximumLatitude) {
        return false;
    }
    // A polygon crossing the date line has a very wide bounding box and is
    // deliberately kept visible.  For ordinary polygons this inexpensive
    // test avoids projecting thousands of off-screen vertices while zoomed in.
    if (country.maximumLongitude - country.minimumLongitude > 300.0) {
        return true;
    }
    return !(country.maximumLongitude < viewMinimumLongitude
             || country.minimumLongitude > viewMaximumLongitude);
}

void DrawCountryBoundaries(
    HDC device,
    const MapState& state,
    const RECT& map) {
    if (state.countries.empty()) {
        return;
    }
    std::vector<const CountryShape*> ordered;
    for (const auto& country : state.countries) if (!country.disputed && country.iso != L"CHN") ordered.push_back(&country);
    for (const auto& country : state.countries) if (country.disputed) ordered.push_back(&country);
    // Preserve China's prescribed outline above an adjacent unresolved-region halo.
    for (const auto& country : state.countries) if (!country.disputed && country.iso == L"CHN") ordered.push_back(&country);
    if (!EnsureGdiplus()) {
        const HPEN borderPen = CreatePen(
            PS_SOLID,
            state.zoom >= 8.0 ? 2 : 1,
            kCountryBorderColor);
        const HGDIOBJ oldPen = SelectObject(device, borderPen);
        const HPEN disputedPen = CreatePen(PS_DASH, 1, kCountryBorderColor);
        const HPEN referenceDotPen = CreatePen(PS_DOT, 1, kCountryBorderColor);
        const HPEN controlPen = CreatePen(PS_DASHDOT, 1, kCountryBorderColor);
        const HPEN coastPen = CreatePen(PS_SOLID, 1, kCoastColor);
        const HPEN clearingPen = CreatePen(PS_SOLID, state.zoom >= 8.0 ? 4 : 3, kLandColor);
        const HGDIOBJ oldBrush = SelectObject(device, GetStockObject(NULL_BRUSH));
        SaveDC(device);
        IntersectClipRect(device, map.left, map.top, map.right, map.bottom);
        for (const auto* item : ordered) {
            const auto& country = *item;
            if (country.labelOnly || !CountryIntersectsView(state, country)) {
                continue;
            }
            for (const auto& ring : country.rings) {
                if (ring.size() < 2) {
                    continue;
                }
                std::vector<POINT> segment;
                segment.reserve(ring.size());
                for (const auto& coordinate : ring) {
                    segment.push_back(Project(
                        state,
                        map,
                        coordinate.longitude,
                        coordinate.latitude));
                }
                if (country.disputed && !country.boundaryLine) {
                    SelectObject(device, clearingPen);
                    Polyline(device, segment.data(), static_cast<int>(segment.size()));
                }
                SelectObject(device, country.coastline ? coastPen : country.controlLine ? controlPen
                    : country.disputed ? (country.boundaryLine ? referenceDotPen : disputedPen) : borderPen);
                Polyline(device, segment.data(), static_cast<int>(segment.size()));
            }
        }
        RestoreDC(device, -1);
        SelectObject(device, oldBrush);
        SelectObject(device, oldPen);
        DeleteObject(borderPen);
        DeleteObject(disputedPen);
        DeleteObject(referenceDotPen);
        DeleteObject(controlPen);
        DeleteObject(coastPen);
        DeleteObject(clearingPen);
        return;
    }

    Graphics graphics(device);
    ConfigureGraphics(graphics);
    graphics.SetClip(ToRectF(map));
    const Gdiplus::REAL width = state.zoom >= 8.0
        ? 1.65F
        : (state.zoom >= 3.0 ? 1.15F : 0.85F);
    Pen borderPen(FromColorRef(kCountryBorderColor, 225), width);
    Pen clearingPen(FromColorRef(kLandColor), width+2.0F);
    Pen disputedPen(FromColorRef(kCountryBorderColor), width);
    disputedPen.SetDashStyle(Gdiplus::DashStyleDash);
    Pen referenceDotPen(FromColorRef(kCountryBorderColor), width);
    referenceDotPen.SetDashStyle(Gdiplus::DashStyleDot);
    Pen controlPen(FromColorRef(kCountryBorderColor), width);
    controlPen.SetDashStyle(Gdiplus::DashStyleDashDot);
    Pen coastPen(FromColorRef(kCoastColor, 200), width * .8F);
    const std::size_t vertexStride = state.zoom < 1.5
        ? 4U
        : (state.zoom < 3.0 ? 2U : 1U);
    for (const auto* item : ordered) {
        const auto& country = *item;
        if (country.labelOnly || !CountryIntersectsView(state, country)) {
            continue;
        }
        for (const auto& ring : country.rings) {
            if (ring.size() < 2) {
                continue;
            }
            std::vector<PointF> segment;
            segment.reserve(ring.size());
            const auto flush = [&]() {
                if (segment.size() >= 2) {
                    if (country.disputed && !country.boundaryLine) graphics.DrawLines(&clearingPen, segment.data(), static_cast<INT>(segment.size()));
                    graphics.DrawLines(
                        country.coastline ? &coastPen : country.controlLine ? &controlPen
                            : country.disputed ? (country.boundaryLine ? &referenceDotPen : &disputedPen) : &borderPen,
                        segment.data(),
                        static_cast<INT>(segment.size()));
                }
                segment.clear();
            };
            for (std::size_t index = 0; index < ring.size(); ++index) {
                if (index > 0
                    && std::abs(
                        ring[index].longitude - ring[index - 1].longitude) > 180.0) {
                    flush();
                }
                if (index != 0 && index + 1 != ring.size()
                    && index % vertexStride != 0) {
                    continue;
                }
                segment.push_back(ToPointF(Project(
                    state,
                    map,
                    ring[index].longitude,
                    ring[index].latitude)));
            }
            flush();
        }
    }
}

bool RectanglesOverlap(const RECT& first, const RECT& second) {
    RECT intersection{};
    return IntersectRect(&intersection, &first, &second) != FALSE;
}

void DrawCountryLabels(
    HDC device,
    const MapState& state,
    const RECT& map) {
    if (state.countries.empty()) {
        return;
    }
    std::vector<const CountryShape*> candidates;
    candidates.reserve(state.countries.size());
    for (const auto& country : state.countries) {
        if (state.zoom < country.minimumLabelZoom || (country.island && state.zoom < 8.0)) continue;
        if (country.name.empty() || !CountryIntersectsView(state, country)) {
            continue;
        }
        // Keep the global view readable.  More names become available as the
        // user zooms in; tiny islands are still available at close range.
        if (state.zoom < 1.25 && country.labelRank > 3) {
            continue;
        }
        if (state.zoom >= 1.25 && state.zoom < 2.5
            && country.labelRank > 5) {
            continue;
        }
        if (state.zoom >= 2.5 && state.zoom < 5.0
            && country.labelRank > 7) {
            continue;
        }
        candidates.push_back(&country);
    }
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const CountryShape* first, const CountryShape* second) {
            if (first->labelRank != second->labelRank) {
                return first->labelRank < second->labelRank;
            }
            return first->area > second->area;
        });

    // GDI's ClearType text is not composited reliably on a memory bitmap
    // during rapid map repaints.  GDI+ gives labels a stable anti-aliased
    // glyph edge and lets us draw a small halo without changing the control's
    // Win32 contract.  Keep the existing GDI path as a fallback below.
    if (EnsureGdiplus()) {
        Graphics graphics(device);
        ConfigureGraphics(graphics);
        graphics.SetClip(ToRectF(map));
        const int dpi = std::max(96, GetDeviceCaps(device, LOGPIXELSY));
        const Gdiplus::REAL fontSize = static_cast<Gdiplus::REAL>(
            (state.zoom >= 8.0 ? 14.0 : (state.zoom >= 2.0 ? 13.0 : 12.0))
            * static_cast<double>(dpi) / 96.0);
        FontFamily familyProbe(L"Microsoft YaHei UI");
        const wchar_t* familyName = familyProbe.GetLastStatus() == Gdiplus::Ok
            ? L"Microsoft YaHei UI"
            : L"Segoe UI";
        FontFamily family(familyName);
        Font font(&family, fontSize, FontStyle::FontStyleRegular, Unit::UnitPixel);
        StringFormat format;
        format.SetAlignment(StringAlignment::StringAlignmentCenter);
        format.SetLineAlignment(StringAlignment::StringAlignmentCenter);
        format.SetFormatFlags(StringFormatFlags::StringFormatFlagsNoWrap);
        SolidBrush halo(Color(225, 255, 255, 255));
        SolidBrush textBrush(FromColorRef(kCountryLabelColor, 245));
        std::vector<RECT> occupied;
        occupied.reserve(candidates.size());

        for (const CountryShape* country : candidates) {
            const POINT labelPoint = Project(
                state,
                map,
                country->labelLongitude,
                country->labelLatitude);
            if (!PtInRect(&map, labelPoint)) {
                continue;
            }
            if (country->island) {
                graphics.FillEllipse(&textBrush, static_cast<Gdiplus::REAL>(labelPoint.x-2),
                    static_cast<Gdiplus::REAL>(labelPoint.y-2), 4.0F, 4.0F);
            }
            Gdiplus::RectF measured{};
            graphics.MeasureString(
                country->name.c_str(),
                -1,
                &font,
                PointF(0.0F, 0.0F),
                &measured);
            const int textWidth = std::max(
                12,
                static_cast<int>(std::ceil(measured.Width)));
            const int textHeight = std::max(
                12,
                static_cast<int>(std::ceil(measured.Height)));
            RECT labelRect{
                labelPoint.x - textWidth / 2 - 2,
                labelPoint.y - textHeight / 2 - 1,
                labelPoint.x + (textWidth + 1) / 2 + 2,
                labelPoint.y + (textHeight + 1) / 2 + 1};
            if (labelRect.left < map.left || labelRect.right > map.right
                || labelRect.top < map.top || labelRect.bottom > map.bottom) {
                continue;
            }
            RECT padded = labelRect;
            InflateRect(&padded, 3, 2);
            bool overlaps = false;
            for (const auto& previous : occupied) {
                if (RectanglesOverlap(padded, previous)) {
                    overlaps = true;
                    break;
                }
            }
            if (overlaps) {
                continue;
            }
            occupied.push_back(padded);

            const RectF textRect(
                static_cast<Gdiplus::REAL>(labelPoint.x - textWidth / 2),
                static_cast<Gdiplus::REAL>(labelPoint.y - textHeight / 2),
                static_cast<Gdiplus::REAL>(textWidth),
                static_cast<Gdiplus::REAL>(textHeight));
            // A two-pixel translucent halo preserves readability over both
            // the blue ocean and the darker country fill.
            for (const auto& offset : {
                     PointF(-1.0F, 0.0F),
                     PointF(1.0F, 0.0F),
                     PointF(0.0F, -1.0F),
                     PointF(0.0F, 1.0F)}) {
                RectF haloRect = textRect;
                haloRect.X += offset.X;
                haloRect.Y += offset.Y;
                graphics.DrawString(
                    country->name.c_str(),
                    -1,
                    &font,
                    haloRect,
                    &format,
                    &halo);
            }
            graphics.DrawString(
                country->name.c_str(),
                -1,
                &font,
                textRect,
                &format,
                &textBrush);
        }
        return;
    }

    const int fontHeight = state.zoom >= 8.0 ? -14 : (state.zoom >= 2.0 ? -12 : -11);
    const HFONT font = CreateFontW(
        fontHeight,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI");
    const HGDIOBJ oldFont = SelectObject(device, font);
    const int oldBkMode = SetBkMode(device, TRANSPARENT);
    const COLORREF oldTextColor = SetTextColor(device, kCountryLabelColor);
    std::vector<RECT> occupied;
    occupied.reserve(candidates.size());

    for (const CountryShape* country : candidates) {
        const POINT labelPoint = Project(
            state,
            map,
            country->labelLongitude,
            country->labelLatitude);
        if (!PtInRect(&map, labelPoint)) {
            continue;
        }
        SIZE textSize{};
        GetTextExtentPoint32W(
            device,
            country->name.c_str(),
            static_cast<int>(country->name.size()),
            &textSize);
        const int textWidth = static_cast<int>(std::max(12L, textSize.cx));
        const int textHeight = static_cast<int>(std::max(12L, textSize.cy));
        RECT labelRect{
            labelPoint.x - textWidth / 2 - 2,
            labelPoint.y - textHeight / 2 - 1,
            labelPoint.x + (textWidth + 1) / 2 + 2,
            labelPoint.y + (textHeight + 1) / 2 + 1};
        if (labelRect.left < map.left || labelRect.right > map.right
            || labelRect.top < map.top || labelRect.bottom > map.bottom) {
            continue;
        }
        RECT padded = labelRect;
        InflateRect(&padded, 3, 2);
        bool overlaps = false;
        for (const auto& previous : occupied) {
            if (RectanglesOverlap(padded, previous)) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) {
            continue;
        }
        occupied.push_back(padded);

        const int textX = labelPoint.x - textWidth / 2;
        const int textY = labelPoint.y - textHeight / 2;
        // A one-pixel light halo keeps names legible over both land and grid.
        SetTextColor(device, RGB(255, 255, 255));
        TextOutW(
            device,
            textX + 1,
            textY + 1,
            country->name.c_str(),
            static_cast<int>(country->name.size()));
        SetTextColor(device, kCountryLabelColor);
        TextOutW(
            device,
            textX,
            textY,
            country->name.c_str(),
            static_cast<int>(country->name.size()));
    }

    SetTextColor(device, oldTextColor);
    SetBkMode(device, oldBkMode);
    SelectObject(device, oldFont);
    DeleteObject(font);
}

void DrawStationDots(HDC device, const MapState& state, const RECT& map) {
    if (state.stations == nullptr) {
        return;
    }
    // Keep the station marker renderer identical to the original map: a
    // small solid coloured dot with a one-pixel white outline.  The map base
    // may use the higher-resolution GDI+ country layer, but station markers
    // deliberately remain on this native GDI path so their appearance and
    // hit-test behaviour do not change when the background is upgraded.
    SaveDC(device);
    IntersectClipRect(device, map.left, map.top, map.right, map.bottom);

    const HBRUSH normalBrush = CreateSolidBrush(kStationColor);
    const HBRUSH selectedBrush = CreateSolidBrush(kSelectedColor);
    const HBRUSH hoverBrush = CreateSolidBrush(kHoverColor);
    const HPEN whitePen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    const HGDIOBJ oldPen = SelectObject(device, whitePen);
    SetBkMode(device, TRANSPARENT);

    for (std::size_t index = 0; index < state.stations->size(); ++index) {
        const auto& station = (*state.stations)[index];
        if (!station.inSelectionScope || !station.hasCoordinate) {
            continue;
        }
        const POINT point = Project(
            state,
            map,
            station.longitude,
            station.latitude);
        const bool hovered = static_cast<int>(index) == state.hoverIndex;
        const int radius = hovered ? 6 : (station.selected ? 5 : 3);
        HBRUSH brush = normalBrush;
        if (hovered) {
            brush = hoverBrush;
        } else if (station.selected) {
            brush = selectedBrush;
        }
        const HGDIOBJ oldBrush = SelectObject(device, brush);
        Ellipse(
            device,
            point.x - radius,
            point.y - radius,
            point.x + radius + 1,
            point.y + radius + 1);
        SelectObject(device, oldBrush);
    }

    SelectObject(device, oldPen);
    DeleteObject(normalBrush);
    DeleteObject(selectedBrush);
    DeleteObject(hoverBrush);
    DeleteObject(whitePen);
    RestoreDC(device, -1);
}

void DrawHoverCard(HDC device, const MapState& state, const RECT& map) {
    if (state.stations == nullptr
        || state.hoverIndex < 0
        || static_cast<std::size_t>(state.hoverIndex) >= state.stations->size()) {
        return;
    }
    const auto& station = (*state.stations)[static_cast<std::size_t>(state.hoverIndex)];
    const POINT point = Project(
        state,
        map,
        station.longitude,
        station.latitude);

    const std::wstring title = Utf8ToWide(station.stationId);
    const std::wstring details =
        Utf8ToWide(CountryPolicy::DisplayName(CountryFlags::ResolveStationArea(
            station.country, station.siteLogCountry, station.stationId,
            station.preferredSource == "NGS", station.latitude, station.longitude)))
        + L"  "
        + std::to_wstring(station.latitude).substr(0, 8)
        + L", "
        + std::to_wstring(station.longitude).substr(0, 9);

    if (EnsureGdiplus()) {
        Graphics graphics(device);
        ConfigureGraphics(graphics);
        graphics.SetClip(ToRectF(map));
        FontFamily family(L"Segoe UI");
        Font titleFont(&family, 12.0F, FontStyle::FontStyleBold, Unit::UnitPixel);
        Font detailFont(&family, 10.5F, FontStyle::FontStyleRegular, Unit::UnitPixel);
        StringFormat leftFormat;
        leftFormat.SetAlignment(StringAlignment::StringAlignmentNear);
        leftFormat.SetLineAlignment(StringAlignment::StringAlignmentCenter);
        leftFormat.SetFormatFlags(StringFormatFlags::StringFormatFlagsNoWrap);
        Gdiplus::RectF titleSize{};
        Gdiplus::RectF detailSize{};
        graphics.MeasureString(
            title.c_str(), -1, &titleFont, PointF(0.0F, 0.0F), &titleSize);
        graphics.MeasureString(
            details.c_str(), -1, &detailFont, PointF(0.0F, 0.0F), &detailSize);
        const int width = std::max(
            120,
            static_cast<int>(std::ceil(std::max(titleSize.Width, detailSize.Width))) + 24);
        const int height = 56;
        int left = point.x + 12;
        int top = point.y - height - 10;
        if (left + width > map.right) {
            left = point.x - width - 12;
        }
        if (top < map.top) {
            top = point.y + 12;
        }
        const int mapLeft = static_cast<int>(map.left);
        const int mapTop = static_cast<int>(map.top);
        const int mapRight = static_cast<int>(map.right);
        const int mapBottom = static_cast<int>(map.bottom);
        left = std::clamp(left, mapLeft, std::max(mapLeft, mapRight - width));
        top = std::clamp(top, mapTop, std::max(mapTop, mapBottom - height));
        const RectF shadowRect(
            static_cast<Gdiplus::REAL>(left + 3),
            static_cast<Gdiplus::REAL>(top + 4),
            static_cast<Gdiplus::REAL>(width),
            static_cast<Gdiplus::REAL>(height));
        const RectF cardRect(
            static_cast<Gdiplus::REAL>(left),
            static_cast<Gdiplus::REAL>(top),
            static_cast<Gdiplus::REAL>(width),
            static_cast<Gdiplus::REAL>(height));
        auto buildRoundedRect = [](GraphicsPath& path,
                                   const RectF& rectangle,
                                   const Gdiplus::REAL radius) {
            const Gdiplus::REAL diameter = radius * 2.0F;
            path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180.0F, 90.0F);
            path.AddArc(
                rectangle.X + rectangle.Width - diameter,
                rectangle.Y,
                diameter,
                diameter,
                270.0F,
                90.0F);
            path.AddArc(
                rectangle.X + rectangle.Width - diameter,
                rectangle.Y + rectangle.Height - diameter,
                diameter,
                diameter,
                0.0F,
                90.0F);
            path.AddArc(
                rectangle.X,
                rectangle.Y + rectangle.Height - diameter,
                diameter,
                diameter,
                90.0F,
                90.0F);
            path.CloseFigure();
        };
        GraphicsPath shadowPath;
        GraphicsPath cardPath;
        buildRoundedRect(shadowPath, shadowRect, 7.0F);
        buildRoundedRect(cardPath, cardRect, 7.0F);
        SolidBrush shadow(FromColorRef(RGB(58, 75, 88), 42));
        SolidBrush background(Color(248, 251, 253, 248));
        Pen border(FromColorRef(RGB(142, 163, 175), 220), 1.0F);
        graphics.FillPath(&shadow, &shadowPath);
        graphics.FillPath(&background, &cardPath);
        graphics.DrawPath(&border, &cardPath);
        SolidBrush titleBrush(FromColorRef(RGB(23, 49, 66), 255));
        SolidBrush detailBrush(FromColorRef(RGB(84, 104, 117), 255));
        const RectF titleRect(
            static_cast<Gdiplus::REAL>(left + 11),
            static_cast<Gdiplus::REAL>(top + 5),
            static_cast<Gdiplus::REAL>(width - 22),
            20.0F);
        const RectF detailRect(
            static_cast<Gdiplus::REAL>(left + 11),
            static_cast<Gdiplus::REAL>(top + 27),
            static_cast<Gdiplus::REAL>(width - 22),
            20.0F);
        graphics.DrawString(
            title.c_str(), -1, &titleFont, titleRect, &leftFormat, &titleBrush);
        graphics.DrawString(
            details.c_str(), -1, &detailFont, detailRect, &leftFormat, &detailBrush);
        return;
    }

    SIZE titleSize{};
    SIZE detailSize{};
    GetTextExtentPoint32W(
        device,
        title.c_str(),
        static_cast<int>(title.size()),
        &titleSize);
    GetTextExtentPoint32W(
        device,
        details.c_str(),
        static_cast<int>(details.size()),
        &detailSize);
    const int width = std::max(titleSize.cx, detailSize.cx) + 22;
    const int height = 52;
    int left = point.x + 12;
    int top = point.y - height - 10;
    if (left + width > map.right) {
        left = point.x - width - 12;
    }
    if (top < map.top) {
        top = point.y + 12;
    }
    RECT card{left, top, left + width, top + height};

    const HBRUSH shadow = CreateSolidBrush(RGB(202, 211, 217));
    RECT shadowRect{card.left + 3, card.top + 3, card.right + 3, card.bottom + 3};
    FillRect(device, &shadowRect, shadow);
    DeleteObject(shadow);

    const HBRUSH background = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(device, &card, background);
    DeleteObject(background);
    FrameRect(device, &card, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));

    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, RGB(23, 49, 66));
    TextOutW(device, card.left + 10, card.top + 7, title.c_str(), static_cast<int>(title.size()));
    SetTextColor(device, RGB(84, 104, 117));
    TextOutW(device, card.left + 10, card.top + 28, details.c_str(), static_cast<int>(details.size()));
}

void DrawSelectionRectangle(
    HDC device,
    const MapState& state,
    const RECT& map) {
    if (!state.selectingRectangle || !state.dragging) {
        return;
    }
    RECT selection{
        std::clamp(
            std::min(state.downPoint.x, state.lastPoint.x),
            map.left,
            map.right),
        std::clamp(
            std::min(state.downPoint.y, state.lastPoint.y),
            map.top,
            map.bottom),
        std::clamp(
            std::max(state.downPoint.x, state.lastPoint.x),
            map.left,
            map.right),
        std::clamp(
            std::max(state.downPoint.y, state.lastPoint.y),
            map.top,
            map.bottom)};
    if (EnsureGdiplus()) {
        Graphics graphics(device);
        ConfigureGraphics(graphics);
        graphics.SetClip(ToRectF(map));
        const RectF rectangle = ToRectF(selection);
        SolidBrush fill(Color(42, 36, 112, 179));
        Pen border(Color(235, 36, 112, 179), 1.7F);
        border.SetDashStyle(Gdiplus::DashStyleDash);
        graphics.FillRectangle(&fill, rectangle);
        graphics.DrawRectangle(&border, rectangle);
        return;
    }
    const HPEN pen = CreatePen(PS_DASH, 2, RGB(36, 112, 179));
    const HGDIOBJ oldPen = SelectObject(device, pen);
    const HGDIOBJ oldBrush = SelectObject(device, GetStockObject(NULL_BRUSH));
    Rectangle(
        device,
        selection.left,
        selection.top,
        selection.right,
        selection.bottom);
    SelectObject(device, oldBrush);
    SelectObject(device, oldPen);
    DeleteObject(pen);
}

void DestroyStaticMapCache(MapState& state) {
    if (state.staticMapBitmap != nullptr) {
        DeleteObject(state.staticMapBitmap);
        state.staticMapBitmap = nullptr;
    }
    state.staticMapWidth = 0;
    state.staticMapHeight = 0;
    state.staticMapRect = RECT{};
    state.staticZoom = 0.0;
    state.staticCenterLongitude = 0.0;
    state.staticCenterLatitude = 0.0;
    state.staticDpi = 0;
    state.staticRectangleSelectionMode = false;
}

// Render everything that is independent of the currently visible/selected
// stations.  Keeping this in one routine means the cache and the allocation
// failure fallback use exactly the same drawing order and styling.
void DrawStaticMapLayers(
    HDC target,
    const MapState& state,
    const RECT& client,
    const RECT& map) {
    const HBRUSH windowBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(target, &client, windowBrush);
    DeleteObject(windowBrush);

    const HBRUSH oceanBrush = CreateSolidBrush(kOceanColor);
    FillRect(target, &map, oceanBrush);
    DeleteObject(oceanBrush);
    FrameRect(target, &map, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));

    // A very light vertical gradient separates the plotting surface from the
    // surrounding controls without relying on a theme-specific window color.
    if (EnsureGdiplus()) {
        Graphics background(target);
        ConfigureGraphics(background);
        const RectF mapRect = ToRectF(map);
        LinearGradientBrush oceanGradient(
            mapRect,
            Color(255, 248, 252, 255),
            Color(255, 232, 242, 249),
            Gdiplus::LinearGradientModeVertical);
        background.FillRectangle(&oceanGradient, mapRect);
        Pen frame(FromColorRef(RGB(147, 169, 183), 230), 1.0F);
        background.DrawRectangle(&frame, mapRect);
    }

    DrawGrid(target, state, map);
    DrawLand(target, state, map);
    DrawCountryBoundaries(target, state, map);
    DrawCountryLabels(target, state, map);

    static constexpr wchar_t normalHelp[] =
        L"单击圆点选择 · 拖动平移 · 滚轮缩放（最高24倍） · 双击恢复全球视图";
    static constexpr wchar_t selectionHelp[] =
        L"地图框选模式：按住左键拖出候选范围 · 单击按钮可取消";
    const wchar_t* help = state.rectangleSelectionMode
        ? selectionHelp
        : normalHelp;
    if (EnsureGdiplus()) {
        Graphics footer(target);
        ConfigureGraphics(footer);
        const int dpi = std::max(96, GetDeviceCaps(target, LOGPIXELSY));
        const Gdiplus::REAL footerSize = static_cast<Gdiplus::REAL>(
            10.0 * static_cast<double>(dpi) / 96.0);
        FontFamily familyProbe(L"Microsoft YaHei UI");
        const wchar_t* familyName = familyProbe.GetLastStatus() == Gdiplus::Ok
            ? L"Microsoft YaHei UI"
            : L"Segoe UI";
        FontFamily family(familyName);
        Font footerFont(&family, footerSize, FontStyle::FontStyleRegular, Unit::UnitPixel);
        StringFormat footerFormat;
        footerFormat.SetAlignment(StringAlignment::StringAlignmentNear);
        footerFormat.SetLineAlignment(StringAlignment::StringAlignmentCenter);
        footerFormat.SetFormatFlags(StringFormatFlags::StringFormatFlagsNoWrap);
        SolidBrush footerBrush(FromColorRef(RGB(78, 101, 115), 240));
        const RectF helpRect(
            static_cast<Gdiplus::REAL>(map.left + 2),
            static_cast<Gdiplus::REAL>(map.bottom + 5),
            static_cast<Gdiplus::REAL>(std::max<LONG>(20, map.right - map.left - 205)),
            static_cast<Gdiplus::REAL>(std::max<LONG>(20, client.bottom - map.bottom)));
        footer.DrawString(
            help,
            -1,
            &footerFont,
            helpRect,
            &footerFormat,
            &footerBrush);

        SolidBrush normalLegend(FromColorRef(kStationColor, 240));
        SolidBrush selectedLegend(FromColorRef(kSelectedColor, 245));
        Pen legendOutline(FromColorRef(RGB(255, 255, 255), 230), 0.8F);
        const RectF normalRect(
            static_cast<Gdiplus::REAL>(map.right - 188),
            static_cast<Gdiplus::REAL>(map.bottom + 9),
            10.0F,
            10.0F);
        const RectF selectedRect(
            static_cast<Gdiplus::REAL>(map.right - 95),
            static_cast<Gdiplus::REAL>(map.bottom + 9),
            10.0F,
            10.0F);
        footer.FillEllipse(&normalLegend, normalRect);
        footer.DrawEllipse(&legendOutline, normalRect);
        footer.FillEllipse(&selectedLegend, selectedRect);
        footer.DrawEllipse(&legendOutline, selectedRect);
        footer.DrawString(
            L"可选测站",
            -1,
            &footerFont,
            RectF(
                static_cast<Gdiplus::REAL>(map.right - 172),
                static_cast<Gdiplus::REAL>(map.bottom + 5),
                66.0F,
                20.0F),
            &footerFormat,
            &footerBrush);
        footer.DrawString(
            L"已选择",
            -1,
            &footerFont,
            RectF(
                static_cast<Gdiplus::REAL>(map.right - 79),
                static_cast<Gdiplus::REAL>(map.bottom + 5),
                55.0F,
                20.0F),
            &footerFormat,
            &footerBrush);
    } else {
        SetBkMode(target, TRANSPARENT);
        SetTextColor(target, RGB(92, 111, 124));
        TextOutW(
            target,
            map.left + 2,
            map.bottom + 12,
            help,
            static_cast<int>(wcslen(help)));

        const HBRUSH normalLegend = CreateSolidBrush(kStationColor);
        const HBRUSH selectedLegend = CreateSolidBrush(kSelectedColor);
        Ellipse(target, map.right - 188, map.bottom + 15, map.right - 178, map.bottom + 25);
        RECT legendNormal{map.right - 188, map.bottom + 15, map.right - 178, map.bottom + 25};
        FillRect(target, &legendNormal, normalLegend);
        RECT legendSelected{map.right - 95, map.bottom + 15, map.right - 85, map.bottom + 25};
        FillRect(target, &legendSelected, selectedLegend);
        DeleteObject(normalLegend);
        DeleteObject(selectedLegend);
        TextOutW(target, map.right - 172, map.bottom + 12, L"可选测站", 4);
        TextOutW(target, map.right - 79, map.bottom + 12, L"已选择", 3);
    }
}

bool EnsureStaticMapCache(
    HDC device,
    MapState& state,
    const RECT& client,
    const RECT& map) {
    const int width = std::max<LONG>(1, client.right - client.left);
    const int height = std::max<LONG>(1, client.bottom - client.top);
    const int dpi = std::max(96, GetDeviceCaps(device, LOGPIXELSY));
    const bool matches = state.staticMapBitmap != nullptr
        && state.staticMapWidth == width
        && state.staticMapHeight == height
        && state.staticMapRect.left == map.left
        && state.staticMapRect.top == map.top
        && state.staticMapRect.right == map.right
        && state.staticMapRect.bottom == map.bottom
        && state.staticDpi == dpi
        && state.staticRectangleSelectionMode == state.rectangleSelectionMode
        && std::abs(state.staticZoom - state.zoom) < 1.0e-12
        && std::abs(state.staticCenterLongitude - state.centerLongitude) < 1.0e-12
        && std::abs(state.staticCenterLatitude - state.centerLatitude) < 1.0e-12;
    if (matches) {
        return true;
    }

    const HDC cacheDevice = CreateCompatibleDC(device);
    if (cacheDevice == nullptr) {
        return false;
    }
    const HBITMAP nextBitmap = CreateCompatibleBitmap(device, width, height);
    if (nextBitmap == nullptr) {
        DeleteDC(cacheDevice);
        return false;
    }
    const HGDIOBJ oldBitmap = SelectObject(cacheDevice, nextBitmap);
    RECT cacheClient{0, 0, width, height};
    DrawStaticMapLayers(cacheDevice, state, cacheClient, map);
    SelectObject(cacheDevice, oldBitmap);
    DeleteDC(cacheDevice);

    if (state.staticMapBitmap != nullptr) {
        DeleteObject(state.staticMapBitmap);
    }
    state.staticMapBitmap = nextBitmap;
    state.staticMapWidth = width;
    state.staticMapHeight = height;
    state.staticMapRect = map;
    state.staticZoom = state.zoom;
    state.staticCenterLongitude = state.centerLongitude;
    state.staticCenterLatitude = state.centerLatitude;
    state.staticDpi = dpi;
    state.staticRectangleSelectionMode = state.rectangleSelectionMode;
    return true;
}

void PaintMap(const HWND window, MapState& state) {
    PAINTSTRUCT paint{};
    const HDC device = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);

    const HDC memory = CreateCompatibleDC(device);
    if (memory == nullptr) {
        EndPaint(window, &paint);
        return;
    }
    const HBITMAP bitmap = CreateCompatibleBitmap(
        device,
        std::max(1L, client.right - client.left),
        std::max(1L, client.bottom - client.top));
    if (bitmap == nullptr) {
        DeleteDC(memory);
        EndPaint(window, &paint);
        return;
    }
    const HGDIOBJ oldBitmap = SelectObject(memory, bitmap);

    const RECT map = GetMapRect(window);
    bool useStaticCache = EnsureStaticMapCache(
        device,
        state,
        client,
        map);
    if (useStaticCache) {
        const HDC staticDevice = CreateCompatibleDC(device);
        if (staticDevice != nullptr) {
            const HGDIOBJ oldStaticBitmap = SelectObject(
                staticDevice,
                state.staticMapBitmap);
            const BOOL copied = BitBlt(
                memory,
                0,
                0,
                state.staticMapWidth,
                state.staticMapHeight,
                staticDevice,
                0,
                0,
                SRCCOPY);
            SelectObject(staticDevice, oldStaticBitmap);
            DeleteDC(staticDevice);
            if (copied == FALSE) {
                useStaticCache = false;
            }
        } else {
            // Fall back to a direct render if a display driver cannot create
            // the short-lived source DC.  This frame remains correct and the
            // next one will retry the cache path.
            useStaticCache = false;
        }
    }

    if (!useStaticCache) {
    const HBRUSH windowBrush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(memory, &client, windowBrush);
    DeleteObject(windowBrush);

    const HBRUSH oceanBrush = CreateSolidBrush(kOceanColor);
    FillRect(memory, &map, oceanBrush);
    DeleteObject(oceanBrush);
    FrameRect(memory, &map, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));

    // A very light vertical gradient separates the plotting surface from the
    // surrounding controls without relying on a theme-specific window color.
    // It is painted only into the memory bitmap, so panning remains tear-free.
    if (EnsureGdiplus()) {
        Graphics background(memory);
        ConfigureGraphics(background);
        const RectF mapRect = ToRectF(map);
        LinearGradientBrush oceanGradient(
            mapRect,
            Color(255, 248, 252, 255),
            Color(255, 232, 242, 249),
            Gdiplus::LinearGradientModeVertical);
        background.FillRectangle(&oceanGradient, mapRect);
        Pen frame(FromColorRef(RGB(147, 169, 183), 230), 1.0F);
        background.DrawRectangle(&frame, mapRect);
    }

    DrawGrid(memory, state, map);
    DrawLand(memory, state, map);
    DrawCountryBoundaries(memory, state, map);
    DrawCountryLabels(memory, state, map);

    static constexpr wchar_t normalHelp[] =
        L"单击圆点选择 · 拖动平移 · 滚轮缩放（最高24倍） · 双击恢复全球视图";
    static constexpr wchar_t selectionHelp[] =
        L"地图框选模式：按住左键拖出候选范围 · 单击按钮可取消";
    const wchar_t* help = state.rectangleSelectionMode
        ? selectionHelp
        : normalHelp;
    if (EnsureGdiplus()) {
        Graphics footer(memory);
        ConfigureGraphics(footer);
        const int dpi = std::max(96, GetDeviceCaps(memory, LOGPIXELSY));
        const Gdiplus::REAL footerSize = static_cast<Gdiplus::REAL>(
            10.0 * static_cast<double>(dpi) / 96.0);
        FontFamily familyProbe(L"Microsoft YaHei UI");
        const wchar_t* familyName = familyProbe.GetLastStatus() == Gdiplus::Ok
            ? L"Microsoft YaHei UI"
            : L"Segoe UI";
        FontFamily family(familyName);
        Font footerFont(&family, footerSize, FontStyle::FontStyleRegular, Unit::UnitPixel);
        StringFormat footerFormat;
        footerFormat.SetAlignment(StringAlignment::StringAlignmentNear);
        footerFormat.SetLineAlignment(StringAlignment::StringAlignmentCenter);
        footerFormat.SetFormatFlags(StringFormatFlags::StringFormatFlagsNoWrap);
        SolidBrush footerBrush(FromColorRef(RGB(78, 101, 115), 240));
        const RectF helpRect(
            static_cast<Gdiplus::REAL>(map.left + 2),
            static_cast<Gdiplus::REAL>(map.bottom + 5),
            static_cast<Gdiplus::REAL>(map.right - map.left - 205),
            static_cast<Gdiplus::REAL>(std::max<LONG>(20, client.bottom - map.bottom)));
        footer.DrawString(
            help,
            -1,
            &footerFont,
            helpRect,
            &footerFormat,
            &footerBrush);

        SolidBrush normalLegend(FromColorRef(kStationColor, 240));
        SolidBrush selectedLegend(FromColorRef(kSelectedColor, 245));
        Pen legendOutline(FromColorRef(RGB(255, 255, 255), 230), 0.8F);
        const RectF normalRect(
            static_cast<Gdiplus::REAL>(map.right - 188),
            static_cast<Gdiplus::REAL>(map.bottom + 9),
            10.0F,
            10.0F);
        const RectF selectedRect(
            static_cast<Gdiplus::REAL>(map.right - 95),
            static_cast<Gdiplus::REAL>(map.bottom + 9),
            10.0F,
            10.0F);
        footer.FillEllipse(&normalLegend, normalRect);
        footer.DrawEllipse(&legendOutline, normalRect);
        footer.FillEllipse(&selectedLegend, selectedRect);
        footer.DrawEllipse(&legendOutline, selectedRect);
        footer.DrawString(
            L"可选测站",
            -1,
            &footerFont,
            RectF(
                static_cast<Gdiplus::REAL>(map.right - 172),
                static_cast<Gdiplus::REAL>(map.bottom + 5),
                66.0F,
                20.0F),
            &footerFormat,
            &footerBrush);
        footer.DrawString(
            L"已选择",
            -1,
            &footerFont,
            RectF(
                static_cast<Gdiplus::REAL>(map.right - 79),
                static_cast<Gdiplus::REAL>(map.bottom + 5),
                55.0F,
                20.0F),
            &footerFormat,
            &footerBrush);
    } else {
        SetBkMode(memory, TRANSPARENT);
        SetTextColor(memory, RGB(92, 111, 124));
        TextOutW(
            memory,
            map.left + 2,
            map.bottom + 12,
            help,
            static_cast<int>(wcslen(help)));

        const HBRUSH normalLegend = CreateSolidBrush(kStationColor);
        const HBRUSH selectedLegend = CreateSolidBrush(kSelectedColor);
        Ellipse(memory, map.right - 188, map.bottom + 15, map.right - 178, map.bottom + 25);
        RECT legendNormal{map.right - 188, map.bottom + 15, map.right - 178, map.bottom + 25};
        FillRect(memory, &legendNormal, normalLegend);
        RECT legendSelected{map.right - 95, map.bottom + 15, map.right - 85, map.bottom + 25};
        FillRect(memory, &legendSelected, selectedLegend);
        DeleteObject(normalLegend);
        DeleteObject(selectedLegend);
        TextOutW(memory, map.right - 172, map.bottom + 12, L"可选测站", 4);
        TextOutW(memory, map.right - 79, map.bottom + 12, L"已选择", 3);
    }
    }

    // Station markers, the transient rectangle, and the hover card are kept
    // outside the cached layer so selection/hover updates only redraw the
    // inexpensive dynamic pass.
    DrawStationDots(memory, state, map);
    DrawSelectionRectangle(memory, state, map);
    DrawHoverCard(memory, state, map);
    const int clientWidth = std::max<LONG>(1, client.right - client.left);
    const int clientHeight = std::max<LONG>(1, client.bottom - client.top);
    BitBlt(
        device,
        0,
        0,
        clientWidth,
        clientHeight,
        memory,
        0,
        0,
        SRCCOPY);

    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(window, &paint);
}

LRESULT CALLBACK MapWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    auto* state = reinterpret_cast<MapState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        auto createdState = std::make_unique<MapState>();
        createdState->landRings = ParseGeoJsonRings(
            LoadResourceText(IDR_WORLD_LAND));
        createdState->countries = ParseGeoJsonCountries(
            LoadResourceText(IDR_WORLD_COUNTRIES));
        state = createdState.release();
        SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    case WM_NCDESTROY:
        if (state != nullptr) {
            DestroyStaticMapCache(*state);
        }
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (state != nullptr) {
            PaintMap(window, *state);
        }
        return 0;
    case WM_SIZE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        // A compatible bitmap can become invalid after a monitor/DPI switch
        // even when the client rectangle itself did not change.
        if (state != nullptr) {
            DestroyStaticMapCache(*state);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (state != nullptr && !state->rectangleSelectionMode) {
            state->zoom = kMinimumMapZoom;
            state->centerLongitude = 0.0;
            state->centerLatitude = 0.0;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (state != nullptr) {
            const POINT cursor{
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)};
            const RECT map = GetMapRect(window);
            if (!PtInRect(&map, cursor)) {
                return 0;
            }
            SetFocus(window);
            SetCapture(window);
            state->mouseDown = true;
            state->dragging = false;
            state->selectingRectangle = state->rectangleSelectionMode;
            state->downPoint = cursor;
            state->lastPoint = state->downPoint;
        }
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        if (state != nullptr && state->mouseDown) {
            // Capture can be lost to Alt-Tab, a modal dialog, or a page
            // switch.  Clear the transient drag state so the next click
            // starts a fresh gesture instead of completing an old rectangle.
            state->mouseDown = false;
            state->dragging = false;
            state->selectingRectangle = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (state == nullptr) {
            return 0;
        }
        if (!state->trackingMouse) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window;
            TrackMouseEvent(&tracking);
            state->trackingMouse = true;
        }
        if (state->mouseDown) {
            const POINT current{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int totalDx = current.x - state->downPoint.x;
            const int totalDy = current.y - state->downPoint.y;
            if (std::abs(totalDx) > 4 || std::abs(totalDy) > 4) {
                state->dragging = true;
            }
            if (state->selectingRectangle) {
                state->lastPoint = current;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (state->dragging) {
                const RECT map = GetMapRect(window);
                const double scaleX =
                    static_cast<double>(map.right - map.left) / 360.0 * state->zoom;
                const double scaleY =
                    static_cast<double>(map.bottom - map.top) / 180.0 * state->zoom;
                if (scaleX > 0.0 && scaleY > 0.0) {
                    state->centerLongitude -= (current.x - state->lastPoint.x) / scaleX;
                    state->centerLatitude += (current.y - state->lastPoint.y) / scaleY;
                    ClampCenter(*state);
                }
                state->lastPoint = current;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        {
            const POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int hover = HitTestStation(
                *state,
                GetMapRect(window),
                cursor,
                9);
            if (hover != state->hoverIndex) {
                state->hoverIndex = hover;
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (state != nullptr && state->mouseDown) {
            ReleaseCapture();
            const bool wasDragging = state->dragging;
            const bool wasSelectingRectangle = state->selectingRectangle;
            state->lastPoint = POINT{
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)};
            state->mouseDown = false;
            state->dragging = false;
            state->selectingRectangle = false;
            if (wasSelectingRectangle) {
                if (wasDragging) {
                    const RECT map = GetMapRect(window);
                    const POINT first{
                        std::clamp(state->downPoint.x, map.left, map.right),
                        std::clamp(state->downPoint.y, map.top, map.bottom)};
                    const POINT second{
                        std::clamp(state->lastPoint.x, map.left, map.right),
                        std::clamp(state->lastPoint.y, map.top, map.bottom)};
                    const GeoPoint firstGeo = Unproject(*state, map, first);
                    const GeoPoint secondGeo = Unproject(*state, map, second);
                    const MapSelectionBounds bounds{
                        std::min(firstGeo.latitude, secondGeo.latitude),
                        std::max(firstGeo.latitude, secondGeo.latitude),
                        std::min(firstGeo.longitude, secondGeo.longitude),
                        std::max(firstGeo.longitude, secondGeo.longitude)};
                    SendMessageW(
                        GetParent(window),
                        WM_IGS_MAP_AREA_SELECTED,
                        0,
                        reinterpret_cast<LPARAM>(&bounds));
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (!wasDragging) {
                const POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const int hit = HitTestStation(
                    *state,
                    GetMapRect(window),
                    cursor,
                    11);
                if (hit >= 0) {
                    SendMessageW(
                        GetParent(window),
                        WM_IGS_MAP_TOGGLE_STATION,
                        static_cast<WPARAM>(hit),
                        0);
                }
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (state != nullptr) {
            state->trackingMouse = false;
            state->hoverIndex = -1;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (state != nullptr) {
            const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const RECT map = GetMapRect(window);
            POINT cursor{
                GET_X_LPARAM(lParam),
                GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &cursor);
            const GeoPoint beforeZoom = Unproject(*state, map, cursor);
            const double factor = delta > 0 ? 1.25 : 0.8;
            state->zoom = std::clamp(
                state->zoom * factor,
                kMinimumMapZoom,
                kMaximumMapZoom);
            // Keep the geographic location under the cursor fixed while
            // zooming.  This makes it practical to inspect a dense country
            // without repeated pan corrections.
            const double width = static_cast<double>(map.right - map.left);
            const double height = static_cast<double>(map.bottom - map.top);
            const double scaleX = width / 360.0 * state->zoom;
            const double scaleY = height / 180.0 * state->zoom;
            if (scaleX > 0.0 && scaleY > 0.0) {
                state->centerLongitude = beforeZoom.longitude
                    - (cursor.x - (map.left + map.right) / 2.0) / scaleX;
                state->centerLatitude = beforeZoom.latitude
                    + (cursor.y - (map.top + map.bottom) / 2.0) / scaleY;
            }
            ClampCenter(*state);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_SETCURSOR:
        if (state != nullptr && state->rectangleSelectionMode) {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return TRUE;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

bool RegisterMapViewClass(const HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = MapWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_HAND);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kMapClassName;
    return RegisterClassExW(&windowClass) != 0
        || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND CreateMapView(
    const HWND parent,
    const HINSTANCE instance,
    const int controlId) {
    return CreateWindowExW(
        WS_EX_CLIENTEDGE,
        kMapClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
        0,
        0,
        100,
        100,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        instance,
        nullptr);
}

void MapViewSetStations(
    const HWND mapWindow,
    const std::vector<StationFile>* stations) {
    auto* state = reinterpret_cast<MapState*>(
        GetWindowLongPtrW(mapWindow, GWLP_USERDATA));
    if (state != nullptr) {
        state->stations = stations;
        state->hoverIndex = -1;
        InvalidateRect(mapWindow, nullptr, FALSE);
    }
}

void MapViewRefresh(const HWND mapWindow) {
    InvalidateRect(mapWindow, nullptr, FALSE);
}

void MapViewResetView(const HWND mapWindow) {
    auto* state = reinterpret_cast<MapState*>(
        GetWindowLongPtrW(mapWindow, GWLP_USERDATA));
    if (state != nullptr) {
        state->zoom = kMinimumMapZoom;
        state->centerLongitude = 0.0;
        state->centerLatitude = 0.0;
        InvalidateRect(mapWindow, nullptr, FALSE);
    }
}

void MapViewSetRectangleSelectionMode(
    const HWND mapWindow,
    const bool enabled) {
    auto* state = reinterpret_cast<MapState*>(
        GetWindowLongPtrW(mapWindow, GWLP_USERDATA));
    if (state != nullptr) {
        if (!enabled && state->mouseDown && state->selectingRectangle) {
            ReleaseCapture();
            state->mouseDown = false;
            state->dragging = false;
            state->selectingRectangle = false;
        }
        state->rectangleSelectionMode = enabled;
        state->hoverIndex = -1;
        InvalidateRect(mapWindow, nullptr, FALSE);
    }
}
