#include "GeonetJapan.h"

#include "DateUtil.h"
#include "HttpClient.h"

#include <windows.h>

#include "../resources/resource.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <format>
#include <iterator>
#include <mutex>
#include <numeric>
#include <regex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace {

struct CatalogEntry {
    std::string stationNumber;
    std::string stationId;
    std::string fileCode;
    double latitude = 0.0;
    double longitude = 0.0;
    bool hasCoordinate = false;
};

std::string LoadEmbeddedCatalog() {
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(
        module,
        MAKEINTRESOURCEW(IDR_GEONET_JAPAN_CATALOG),
        RT_RCDATA);
    if (resource == nullptr) return {};
    const HGLOBAL loaded = LoadResource(module, resource);
    if (loaded == nullptr) return {};
    const auto* data = static_cast<const char*>(LockResource(loaded));
    const DWORD size = SizeofResource(module, resource);
    return data == nullptr || size == 0
        ? std::string{}
        : std::string(data, data + size);
}

const std::vector<CatalogEntry>& Catalog() {
    static const std::vector<CatalogEntry> entries = []() {
        std::vector<CatalogEntry> result;
        std::istringstream input(LoadEmbeddedCatalog());
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line.front() == '#') continue;
            std::istringstream row(line);
            CatalogEntry entry;
            int hasCoordinate = 0;
            if (!(row >> entry.stationNumber >> entry.stationId
                      >> entry.fileCode >> entry.latitude >> entry.longitude
                      >> hasCoordinate)) {
                continue;
            }
            entry.hasCoordinate = hasCoordinate != 0;
            if ((entry.stationNumber.size() != 5
                    && entry.stationNumber.size() != 6)
                || entry.stationId.size() != 9
                || entry.fileCode.size() != 4) {
                continue;
            }
            result.push_back(std::move(entry));
        }
        return result;
    }();
    return entries;
}

std::chrono::sys_days ToDays(const QueryDate& date) {
    return std::chrono::sys_days{
        std::chrono::year{date.year}
        / std::chrono::month{static_cast<unsigned>(date.month)}
        / std::chrono::day{static_cast<unsigned>(date.day)}};
}

QueryDate FromDays(const std::chrono::sys_days value) {
    const std::chrono::year_month_day date{value};
    return MakeQueryDate(
        static_cast<int>(date.year()),
        static_cast<unsigned>(date.month()),
        static_cast<unsigned>(date.day()));
}

std::string DateText(const QueryDate& date) {
    return std::format("{:04}-{:02}-{:02}", date.year, date.month, date.day);
}

bool IsSafeFilePath(const std::string_view path) {
    if (path.empty() || path.size() > 180
        || path.front() != '/' || path.find("..") != std::string_view::npos) {
        return false;
    }
    return std::all_of(path.begin(), path.end(), [](const unsigned char value) {
        return std::isalnum(value) != 0
            || value == '/' || value == '.' || value == '_'
            || value == '-';
    });
}

std::string FileName(const std::string_view path) {
    const auto slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos
        ? path : path.substr(slash + 1));
}

std::string TrimAscii(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
    return value;
}

std::wstring SftpDailyDirectory(const QueryDate& date) {
    // The live Terras SFTP archive switches exactly at the RINEX 4.01
    // publication boundary: GRJE_3.02 ends on 2024-09-02 and GRJE_4.01
    // begins on 2024-09-03.  Both retain the documented short daily names.
    const auto rinex4Start = std::chrono::sys_days{
        std::chrono::year{2024}/std::chrono::September/3};
    const wchar_t* archive = ToDays(date) >= rinex4Start
        ? L"GRJE_4.01"
        : L"GRJE_3.02";
    return std::format(
        L"/data/{}/{:04}/{:03}",
        archive,
        date.year,
        date.dayOfYear);
}

bool ParseSftpObservationName(
    const std::string& name,
    std::string& stationCode,
    int& dayOfYear,
    int& twoDigitYear) {
    // Terras officially documents compact/ordinary short RINEX names.  Keep
    // both d and o, and both .gz/.Z outer compression, because the SFTP
    // directory itself is the source of truth.
    static const std::regex pattern(
        R"(^([A-Za-z0-9]{4})([0-9]{3})0\.([0-9]{2})([dDoO])(?:\.(?:gz|GZ|Z))?$)",
        std::regex::optimize);
    std::smatch match;
    if (!std::regex_match(name, match, pattern)) return false;
    stationCode = UpperAscii(match[1].str());
    dayOfYear = std::stoi(match[2].str());
    twoDigitYear = std::stoi(match[3].str());
    return true;
}

bool IsCurlTransportFailure(const long code) {
    switch (code) {
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

bool WaitForRetryDelay(
    const std::chrono::seconds delay,
    const std::stop_token stopToken) {
    const auto deadline = std::chrono::steady_clock::now() + delay;
    while (!stopToken.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return true;
        std::this_thread::sleep_for(std::min(
            std::chrono::milliseconds{100},
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now)));
    }
    return false;
}

std::string BuildListingForm(
    const std::vector<CatalogEntry>& catalog,
    const std::size_t firstStation,
    const std::size_t stationCount,
    const QueryDate& firstDate,
    const QueryDate& lastDate) {
    std::string body = "category=gis";
    for (std::size_t offset = 0; offset < stationCount; ++offset) {
        body += "&sel_obs%5B" + std::to_string(offset) + "%5D="
            + catalog[firstStation + offset].stationNumber;
    }
    body += std::format(
        "&day_time=UTC"
        "&day_st_year={:04}&day_st_mon={:02}&day_st_day={:02}"
        "&day_ed_year={:04}&day_ed_mon={:02}&day_ed_day={:02}"
        "&day_satellite=GRJE&day_rinex_ver=3.02"
        "&day_filename=normal&day_navfile=normal",
        firstDate.year, firstDate.month, firstDate.day,
        lastDate.year, lastDate.month, lastDate.day);
    return body;
}

struct ParsedListing {
    std::unordered_map<std::string, std::vector<StationFile>> byDate;
};

ParsedListing ParseListing(
    const std::string& html,
    const std::unordered_map<std::string, const CatalogEntry*>& byNumber) {
    ParsedListing result;
    // data type 1 is the observation file; type 2 is the navigation product.
    static const std::regex linkPattern(
        R"GEONET(one_file_download\('([0-9]{5,6})','([^']+)','([0-9]{4}-[0-9]{2}-[0-9]{2})','1','[^']*'\))GEONET",
        std::regex::optimize);
    for (std::sregex_iterator match(html.begin(), html.end(), linkPattern), end;
         match != end; ++match) {
        const std::string stationNumber = (*match)[1].str();
        const std::string remotePath = (*match)[2].str();
        const std::string observationDate = (*match)[3].str();
        const auto catalog = byNumber.find(stationNumber);
        if (catalog == byNumber.end() || !IsSafeFilePath(remotePath)) continue;
        const std::string remoteFileName = FileName(remotePath);
        if (remoteFileName.empty()) continue;
        std::string deliveredFileName = remoteFileName;
        // Terras lists the archive object as compact RINEX *.d.gz, but its
        // anonymous download endpoint expands it and returns *.o.gz (also
        // declared by Content-Disposition).  Store the name users actually
        // receive while retaining the remote archive name in the virtual URL.
        if (deliveredFileName.ends_with("d.gz")) {
            deliveredFileName[deliveredFileName.size() - 4] = 'o';
        }

        StationFile station;
        station.stationId = catalog->second->stationId;
        station.fourCharId = catalog->second->fileCode;
        station.country = "JPN";
        station.fileName = deliveredFileName;
        station.downloadUrl = L"geonet-jp://"
            + std::wstring(stationNumber.begin(), stationNumber.end())
            + L"/" + std::wstring(observationDate.begin(), observationDate.end())
            + L"/" + std::wstring(remoteFileName.begin(), remoteFileName.end());
        station.preferredSource = "Terras";
        station.availabilitySources = {"Terras"};
        station.format = RinexFormat::Rinex34;
        station.interval = "30S";
        station.hasCoordinate = catalog->second->hasCoordinate;
        station.latitude = catalog->second->latitude;
        station.longitude = catalog->second->longitude;
        station.availableNetworks = NetworkBit(ObservationNetwork::GeoNetJapan);
        result.byDate[observationDate].push_back(std::move(station));
    }
    return result;
}

}  // namespace

GeonetJapanBlockResult QueryGeonetJapanBlock(
    const QueryDate& firstDate,
    const QueryDate& lastDate,
    int jobs,
    const std::stop_token stopToken,
    const GeonetJapanProgressCallback& progressCallback) {
    GeonetJapanBlockResult result;
    const auto firstDay = ToDays(firstDate);
    const auto lastDay = ToDays(lastDate);
    if (lastDay < firstDay || lastDay - firstDay >= std::chrono::days{10}) {
        result.error = L"GEONET/Terras 日期块必须为 1～10 个连续日期。";
        return result;
    }
    const auto& catalog = Catalog();
    if (catalog.empty()) {
        result.error = L"无法读取内置 GEONET 测站目录。";
        return result;
    }
    jobs = std::clamp(jobs, 1, 6);
    for (auto day = firstDay; day <= lastDay; day += std::chrono::days{1}) {
        result.days.push_back({FromDays(day), {}, {}});
    }
    std::unordered_map<std::string, std::size_t> dayIndices;
    for (std::size_t index = 0; index < result.days.size(); ++index) {
        dayIndices.emplace(DateText(result.days[index].date), index);
    }
    std::unordered_map<std::string, const CatalogEntry*> byNumber;
    byNumber.reserve(catalog.size());
    for (const auto& entry : catalog) {
        byNumber.emplace(entry.stationNumber, &entry);
    }

    constexpr std::size_t stationGroupSize = 10;
    const std::size_t groupCount =
        (catalog.size() + stationGroupSize - 1) / stationGroupSize;
    std::atomic<int> attempted{0};
    std::atomic<int> completed{0};
    std::atomic<int> succeeded{0};
    std::mutex resultMutex;
    struct GroupFailure {
        std::wstring error;
        long transportCode = 0;
    };
    std::vector<GroupFailure> lastFailures(groupCount);

    int roundIndex = 0;
    const auto executeRound = [&](
        const std::vector<std::size_t>& groups,
        const int concurrency) {
        ++roundIndex;
        std::atomic<std::size_t> nextGroup{0};
        std::atomic<std::size_t> roundCompleted{0};
        std::vector<std::size_t> failedGroups;
        std::mutex failureMutex;

        const auto reportProgress = [&]() {
            if (!progressCallback) return;
            progressCallback({
                roundIndex,
                groupCount,
                groups.size(),
                roundCompleted.load(std::memory_order_relaxed),
                attempted.load(std::memory_order_relaxed),
                completed.load(std::memory_order_relaxed),
                succeeded.load(std::memory_order_relaxed)});
        };
        reportProgress();

        const auto worker = [&]() {
            while (!stopToken.stop_requested()) {
                const std::size_t pendingIndex = nextGroup.fetch_add(
                    1, std::memory_order_relaxed);
                if (pendingIndex >= groups.size()) return;
                const std::size_t group = groups[pendingIndex];
                const std::size_t start = group * stationGroupSize;
                const std::size_t count = std::min(
                    stationGroupSize, catalog.size() - start);
                const std::string form = BuildListingForm(
                    catalog, start, count, firstDate, lastDate);
                attempted.fetch_add(1, std::memory_order_relaxed);
                reportProgress();
                auto response = HttpPostFormText(
                    L"https://terras.gsi.go.jp/day_download.php",
                    form,
                    stopToken,
                    8U * 1024U * 1024U);
                if (!response.ok()) {
                    {
                        std::scoped_lock lock(failureMutex);
                        lastFailures[group].error = std::move(response.error);
                        lastFailures[group].transportCode =
                            response.transportErrorCode;
                        failedGroups.push_back(group);
                    }
                    completed.fetch_add(1, std::memory_order_relaxed);
                    roundCompleted.fetch_add(1, std::memory_order_relaxed);
                    reportProgress();
                    continue;
                }

                auto parsed = ParseListing(response.body, byNumber);
                {
                    std::scoped_lock lock(resultMutex);
                    for (auto& [dateText, files] : parsed.byDate) {
                        const auto day = dayIndices.find(dateText);
                        if (day == dayIndices.end()) continue;
                        auto& target = result.days[day->second];
                        target.files.insert(
                            target.files.end(),
                            std::make_move_iterator(files.begin()),
                            std::make_move_iterator(files.end()));
                    }
                }
                // A group is removed from the pending list after its first
                // successful response, so its files are never merged twice.
                succeeded.fetch_add(1, std::memory_order_relaxed);
                completed.fetch_add(1, std::memory_order_relaxed);
                roundCompleted.fetch_add(1, std::memory_order_relaxed);
                reportProgress();
            }
        };

        const std::size_t workerCount = std::min<std::size_t>(
            static_cast<std::size_t>(std::max(1, concurrency)),
            groups.size());
        std::vector<std::jthread> workers;
        workers.reserve(workerCount);
        for (std::size_t index = 0; index < workerCount; ++index) {
            workers.emplace_back(worker);
        }
        workers.clear();
        std::sort(failedGroups.begin(), failedGroups.end());
        return failedGroups;
    };

    std::vector<std::size_t> pendingGroups(groupCount);
    std::iota(pendingGroups.begin(), pendingGroups.end(), 0);
    pendingGroups = executeRound(pendingGroups, jobs);
    if (!pendingGroups.empty()
        && WaitForRetryDelay(std::chrono::seconds{2}, stopToken)) {
        pendingGroups = executeRound(
            pendingGroups,
            std::max(1, jobs / 2));
    }
    if (!pendingGroups.empty()
        && WaitForRetryDelay(std::chrono::seconds{6}, stopToken)) {
        pendingGroups = executeRound(pendingGroups, 1);
    }

    result.requestsAttempted = attempted.load();
    result.requestsSuccessful = succeeded.load();
    if (stopToken.stop_requested()) {
        result.error = L"GEONET 知识库查询已取消。";
        return result;
    }
    if (!pendingGroups.empty()) {
        const auto firstFailedGroup = pendingGroups.front();
        result.error = L"GEONET/Terras 无法读取匿名日观测目录";
        if (!lastFailures[firstFailedGroup].error.empty()) {
            result.error += L"（" + lastFailures[firstFailedGroup].error + L"）";
        }
        result.error += L"；最终仍有 "
            + std::to_wstring(pendingGroups.size()) + L"/"
            + std::to_wstring(groupCount) + L" 个站组失败。";
        result.transportErrorCode =
            lastFailures[firstFailedGroup].transportCode;
        for (const auto group : pendingGroups) {
            const long code = lastFailures[group].transportCode;
            if (!IsCurlTransportFailure(code)) continue;
            result.networkTransportFailure = true;
            result.transportErrorCode = code;
            break;
        }
        return result;
    }
    for (auto& day : result.days) {
        std::sort(day.files.begin(), day.files.end(),
            [](const StationFile& left, const StationFile& right) {
                return left.stationId < right.stationId;
            });
        day.stations = day.files;
    }
    return result;
}

GeonetJapanBlockResult QueryGeonetJapanSftpBlock(
    const QueryDate& firstDate,
    const QueryDate& lastDate,
    const std::wstring& username,
    const std::wstring& password,
    const std::stop_token stopToken,
    const GeonetJapanProgressCallback& progressCallback) {
    GeonetJapanBlockResult result;
    const auto firstDay = ToDays(firstDate);
    const auto lastDay = ToDays(lastDate);
    if (lastDay < firstDay || lastDay - firstDay >= std::chrono::days{62}) {
        result.error = L"GEONET/Terras SFTP 日期块必须为 1～62 个连续日期。";
        return result;
    }
    const auto& catalog = Catalog();
    if (catalog.empty()) {
        result.error = L"无法读取内置 GEONET 测站目录。";
        return result;
    }

    std::unordered_map<std::string, const CatalogEntry*> byFileCode;
    byFileCode.reserve(catalog.size());
    for (const auto& entry : catalog) {
        byFileCode.emplace(UpperAscii(entry.fileCode), &entry);
    }

    std::vector<std::wstring> directories;
    std::unordered_map<std::string, std::size_t> directoryDayIndex;
    for (auto day = firstDay; day <= lastDay; day += std::chrono::days{1}) {
        const QueryDate date = FromDays(day);
        result.days.push_back({date, {}, {}});
        const std::wstring directory = SftpDailyDirectory(date);
        directories.push_back(directory);
        std::string narrowDirectory;
        narrowDirectory.reserve(directory.size());
        std::transform(
            directory.begin(), directory.end(),
            std::back_inserter(narrowDirectory),
            [](const wchar_t c) { return static_cast<char>(c); });
        directoryDayIndex.emplace(
            std::move(narrowDirectory),
            result.days.size() - 1U);
    }
    if (progressCallback) {
        progressCallback({1, 1, 1, 0, 1, 0, 0});
    }
    const auto response = HttpListTerrasSftpDirectories(
        username,
        password,
        directories,
        stopToken,
        64U * 1024U * 1024U);
    result.requestsAttempted = 1;
    if (!response.ok()) {
        result.error = response.error;
        result.transportErrorCode = response.transportErrorCode;
        result.networkTransportFailure = response.transportErrorCode != 0;
        return result;
    }
    result.requestsSuccessful = 1;
    if (progressCallback) {
        progressCallback({1, 1, 1, 1, 1, 1, 1});
    }

    // Batch-mode OpenSSH normally echoes each command and emits full paths
    // for a wildcard.  Parse both that form and basename-only output so the
    // code remains compatible across Windows OpenSSH versions.
    std::size_t activeDay = static_cast<std::size_t>(-1);
    bool sawRequestedDirectory = false;
    std::size_t parsedFileCount = 0;
    std::istringstream input(response.body);
    std::string line;
    while (std::getline(input, line)) {
        line = TrimAscii(std::move(line));
        if (line.empty()) continue;

        // OpenSSH echoes interactive commands as `sftp> ls ...`.  The echo
        // is useful for associating basename-only output with a date, but it
        // is not proof that the remote directory was actually listed.
        const bool commandEcho = line.starts_with("sftp>");

        for (const auto& [directory, dayIndex] : directoryDayIndex) {
            if (line.find(directory) != std::string::npos) {
                activeDay = dayIndex;
                if (!commandEcho) sawRequestedDirectory = true;
                break;
            }
        }
        if (commandEcho) continue;

        std::string pathOrName = line;
        const auto slash = pathOrName.find_last_of('/');
        if (slash != std::string::npos) {
            pathOrName = pathOrName.substr(slash + 1U);
        } else if (line.starts_with("sftp>")
                   || line.starts_with("Connected to ")
                   || line.find("No such file") != std::string::npos
                   || line.find("not found") != std::string::npos) {
            continue;
        }

        std::string fileCode;
        int fileDoy = 0;
        int fileYear = 0;
        if (!ParseSftpObservationName(
                pathOrName, fileCode, fileDoy, fileYear)) {
            continue;
        }

        std::size_t dayIndex = activeDay;
        if (slash != std::string::npos) {
            dayIndex = static_cast<std::size_t>(-1);
            for (const auto& [directory, candidate] : directoryDayIndex) {
                if (line.find(directory + "/") != std::string::npos) {
                    dayIndex = candidate;
                    break;
                }
            }
        }
        if (dayIndex >= result.days.size()) continue;
        const auto& date = result.days[dayIndex].date;
        if (fileDoy != date.dayOfYear || fileYear != date.year % 100) {
            continue;
        }

        const auto known = byFileCode.find(fileCode);
        StationFile station;
        station.fourCharId = fileCode;
        station.country = "JPN";
        std::string stationNumber = "00000";
        if (known != byFileCode.end()) {
            station.stationId = known->second->stationId;
            stationNumber = known->second->stationNumber;
            station.hasCoordinate = known->second->hasCoordinate;
            station.latitude = known->second->latitude;
            station.longitude = known->second->longitude;
        } else {
            // Preserve a historical/retired file even if the current official
            // coordinate catalogue lacks the code.  The synthetic 9-char ID
            // is deterministic and is explicitly marked as coordinate-less.
            station.stationId = fileCode + "00JPN";
        }
        // The SFTP archive stores Compact RINEX as *.d.gz, while the public
        // Terras HTTPS download endpoint expands the same object and delivers
        // *.o.gz.  The knowledge base is also consumed by the anonymous
        // downloader, so expose the name the user will actually receive while
        // retaining the archive name in the virtual route below.
        std::string deliveredFileName = pathOrName;
        if (deliveredFileName.ends_with("d.gz")
            || deliveredFileName.ends_with("D.GZ")) {
            deliveredFileName[deliveredFileName.size() - 4U] = 'o';
        }
        station.fileName = std::move(deliveredFileName);
        station.downloadUrl = L"geonet-jp://"
            + std::wstring(stationNumber.begin(), stationNumber.end())
            + L"/" + FormatDate(date)
            + L"/" + std::wstring(pathOrName.begin(), pathOrName.end());
        station.preferredSource = "Terras-SFTP";
        station.availabilitySources = {"Terras-SFTP"};
        station.format = RinexFormat::Rinex34;
        station.interval = "30S";
        station.availableNetworks = NetworkBit(ObservationNetwork::GeoNetJapan);
        result.days[dayIndex].files.push_back(std::move(station));
        ++parsedFileCount;
    }
    if (!sawRequestedDirectory && parsedFileCount == 0) {
        result.error = L"Terras SFTP 已连接，但返回内容无法验证任何请求目录；"
                       L"为避免写入空的错误知识库，本批未保存。";
        return result;
    }
    for (auto& day : result.days) {
        std::sort(day.files.begin(), day.files.end(),
            [](const StationFile& left, const StationFile& right) {
                if (left.stationId != right.stationId) {
                    return left.stationId < right.stationId;
                }
                return left.fileName < right.fileName;
            });
        day.files.erase(std::unique(day.files.begin(), day.files.end(),
            [](const StationFile& left, const StationFile& right) {
                return left.fileName == right.fileName;
            }), day.files.end());
        day.stations = day.files;
        if (day.files.empty()) {
            result.error = L"Terras SFTP 没有为 " + FormatDate(day.date)
                + L" 返回任何可识别的观测文件；本批未写入，避免把空结果误标完整。";
            return result;
        }
    }
    return result;
}
