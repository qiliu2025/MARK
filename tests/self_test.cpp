#include "DateUtil.h"
#include "DownloadConcurrency.h"
#include "HttpClient.h"
#include "IgsAvailabilityDb.h"
#include "IgsData.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>

namespace {

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

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

bool IsTrustedUrl(const StationFile& station) {
    const bool trusted =
        station.downloadUrl.starts_with(
            L"https://igs.bkg.bund.de/root_ftp/IGS/obs/")
        || station.downloadUrl.starts_with(
            L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/data/daily/")
        || station.downloadUrl.starts_with(
            L"ftp://igs.gnsswhu.cn/pub/gps/data/daily/")
        || station.downloadUrl.starts_with(
            L"ftp://nfs.kasi.re.kr/gps/data/daily/")
        || station.downloadUrl.starts_with(
            L"ftp://igs.ign.fr/pub/igs/data/")
        || station.downloadUrl.starts_with(
            L"ftp://gssc.esa.int/gnss/data/daily/")
        || station.downloadUrl.starts_with(
            L"https://igs.bkg.bund.de/root_ftp/EUREF/obs/")
        || station.downloadUrl.starts_with(
            L"https://noaa-cors-pds.s3.amazonaws.com/rinex/")
        || station.downloadUrl.starts_with(
            L"https://rinex.geodetic.gov.hk/rinex3/");
    const bool expandedTrusted =
        station.downloadUrl.starts_with(
            L"https://ga-gnss-data-rinex-v1.s3.ap-southeast-2.amazonaws.com/public/daily/")
        || station.downloadUrl.starts_with(
            L"https://www.inegi.org.mx/app/geo2/rgna/")
        || station.downloadUrl.starts_with(
            L"ftp://ftp.sonel.org/gps/data/")
        || station.downloadUrl.starts_with(
            L"https://geoftp.ibge.gov.br/"
            L"informacoes_sobre_posicionamento_geodesico/rbmc/")
        || station.downloadUrl.starts_with(
            L"https://ramsac.ign.gob.ar/api/v1/rinex/download/")
        || station.downloadUrl.starts_with(
            L"ftp://pp.igm.gub.uy/hatanaka/")
        || station.downloadUrl.starts_with(
            L"ftp://rgpdata.ensg.eu/pub/data/")
        || station.downloadUrl.starts_with(
            L"ftp://ftp.trignet.co.za/RefData.");
    const bool newNetworkTrusted =
        station.downloadUrl.starts_with(
            L"http://saegnss2.curtin.edu.au/ldc/rinex3/daily/")
        || station.downloadUrl.starts_with(
            L"http://garner.ucsd.edu/pub/rinex/")
        || station.downloadUrl.starts_with(
            L"https://gps.csn.uchile.cl/data/")
        || station.downloadUrl.starts_with(
            L"https://data.epos-iceland.is/files/gps/")
        || station.downloadUrl.starts_with(
            L"https://cacsa.nrcan.gc.ca/gps/data/gpsdata/")
        || station.downloadUrl.starts_with(
            L"ftp://magnaeco:magnaeco1@srvstorage.igac.gov.co/")
        || station.downloadUrl.starts_with(
            L"sftp://corsigntg@cors.anati.gob.pa/")
        || station.downloadUrl.starts_with(
            L"https://www.ign.hn/");
    return (trusted || expandedTrusted || newNetworkTrusted)
        && station.fileName.find('/') == std::string::npos
        && station.fileName.find('\\') == std::string::npos
        && station.fileName.find("..") == std::string::npos;
}

}  // namespace

int wmain(const int argumentCount, wchar_t** arguments) {
    const QueryDate date = MakeQueryDate(2024, 6, 15);
    if (argumentCount >= 2
        && std::wstring(arguments[1]) == L"--auto-concurrency") {
        const auto s3 = ClassifyDownloadEndpoint(
            L"https://noaa-cors-pds.s3.amazonaws.com/rinex/file.gz");
        const auto https = ClassifyDownloadEndpoint(
            L"https://example.org/archive/file.gz");
        const auto terras = ClassifyDownloadEndpoint(
            L"geonet-jp://0001/2025-01-01/file.gz");
        const auto ftp = ClassifyDownloadEndpoint(
            L"ftp://example.org/archive/file.gz");
        const auto sftp = ClassifyDownloadEndpoint(
            L"sftp://example.org/archive/file.gz");
        const auto cddis = ClassifyDownloadEndpoint(
            L"ftp://gdc.cddis.eosdis.nasa.gov/gnss/data/daily/file.gz");
        const auto kasi = ClassifyDownloadEndpoint(
            L"ftp://nfs.kasi.re.kr/gps/data/daily/file.gz");
        const auto csn = ClassifyDownloadEndpoint(
            L"https://gps.csn.uchile.cl/data/file.gz");
        const auto sonel = ClassifyDownloadEndpoint(
            L"ftp://ftp.sonel.org/gps/data/file.gz");
        const auto esa = ClassifyDownloadEndpoint(
            L"ftp://gssc.esa.int/gnss/data/daily/file.gz");
        const auto magna = ClassifyDownloadEndpoint(
            L"ftp://magnaeco:password@srvstorage.igac.gov.co/data/file.gz");
        const auto bkg = ClassifyDownloadEndpoint(
            L"https://igs.bkg.bund.de/root_ftp/IGS/obs/file.gz");
        if (s3.initial != 6 || s3.minimum != 1 || s3.maximum != 12
            || https.initial != 4 || https.minimum != 1
            || https.maximum != 8
            || terras.initial != 3 || terras.maximum != 3
            || ftp.initial != 2 || ftp.maximum != 3
            || sftp.initial != 1 || sftp.maximum != 1
            || cddis.maximum != 1 || kasi.maximum != 1
            || csn.initial != 1 || csn.maximum != 2
            || sonel.initial != 1 || sonel.maximum != 2
            || esa.initial != 3 || esa.maximum != 6
            || magna.initial != 1 || magna.maximum != 2
            || bkg.initial != 6 || bkg.maximum != 8) {
            std::cerr << "FAIL endpoint profiles\n";
            return 80;
        }

        AutomaticDownloadConcurrency controller;
        std::vector<AutomaticDownloadConcurrency::Lease> s3Leases;
        for (int index = 0; index < 6; ++index) {
            auto lease = controller.tryAcquire(
                L"https://noaa-cors-pds.s3.amazonaws.com/rinex/file.gz");
            if (!lease.has_value()) {
                std::cerr << "FAIL S3 initial slot " << index << "\n";
                return 81;
            }
            s3Leases.push_back(std::move(*lease));
        }
        if (controller.tryAcquire(
                L"https://noaa-cors-pds.s3.amazonaws.com/rinex/file.gz")
                .has_value()) {
            std::cerr << "FAIL S3 exceeded initial limit\n";
            return 82;
        }
        HttpDownloadResult success;
        success.success = true;
        success.bytesWritten = 1024U * 1024U;
        for (auto& lease : s3Leases) lease.finish(success);
        for (int index = 0; index < 4; ++index) {
            auto lease = controller.tryAcquire(
                L"https://noaa-cors-pds.s3.amazonaws.com/rinex/file.gz");
            if (!lease.has_value()) {
                std::cerr << "FAIL S3 sample slot\n";
                return 83;
            }
            lease->finish(success);
        }
        const auto grownS3 = controller.snapshot(
            L"https://noaa-cors-pds.s3.amazonaws.com/rinex/file.gz");
        if (grownS3.limit != 7) {
            std::cerr << "FAIL S3 did not grow: " << grownS3.limit << "\n";
            return 84;
        }

        std::vector<AutomaticDownloadConcurrency::Lease> terrasLeases;
        for (int index = 0; index < 3; ++index) {
            auto lease = controller.tryAcquire(
                L"geonet-jp://0001/2025-01-01/file.gz");
            if (!lease.has_value()) {
                std::cerr << "FAIL Terras slot " << index << "\n";
                return 85;
            }
            terrasLeases.push_back(std::move(*lease));
        }
        if (controller.tryAcquire(
                L"geonet-jp://0001/2025-01-01/file.gz").has_value()) {
            std::cerr << "FAIL Terras exceeded limit 3\n";
            return 86;
        }
        std::atomic_bool cancelledWaitCompleted{false};
        const auto waitStarted = std::chrono::steady_clock::now();
        std::jthread blockedWaiter(
            [&](const std::stop_token token) {
                auto lease = controller.acquire(
                    L"geonet-jp://0001/2025-01-01/file.gz", token);
                cancelledWaitCompleted.store(
                    !lease.has_value(), std::memory_order_relaxed);
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        blockedWaiter.request_stop();
        blockedWaiter.join();
        const auto cancelledWaitMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - waitStarted).count();
        if (!cancelledWaitCompleted.load(std::memory_order_relaxed)
            || cancelledWaitMs > 500) {
            std::cerr << "FAIL cancelled host wait: "
                      << cancelledWaitMs << "ms\n";
            return 91;
        }
        for (auto& lease : terrasLeases) lease.finish(success);

        AutomaticDownloadConcurrency pausedController;
        auto pauseControl = std::make_shared<DownloadControl>();
        pauseControl->pause();
        std::atomic_bool pausedWaitCompleted{false};
        std::jthread pausedWaiter(
            [&](const std::stop_token token) {
                auto lease = pausedController.acquire(
                    L"https://paused.example.org/file.gz",
                    token,
                    pauseControl);
                pausedWaitCompleted.store(
                    !lease.has_value(), std::memory_order_relaxed);
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        if (pausedController.snapshot(
                L"https://paused.example.org/file.gz").active != 0) {
            std::cerr << "FAIL paused waiter acquired a host slot\n";
            return 92;
        }
        pausedWaiter.request_stop();
        pauseControl->resume();
        pausedWaiter.join();
        if (!pausedWaitCompleted.load(std::memory_order_relaxed)) {
            std::cerr << "FAIL paused waiter did not cancel\n";
            return 93;
        }

        HttpDownloadResult timeout;
        timeout.error = L"Connection timed out";
        for (int index = 0; index < 2; ++index) {
            auto lease = controller.tryAcquire(
                L"https://example.org/archive/file.gz");
            if (!lease.has_value()) {
                std::cerr << "FAIL HTTPS feedback slot\n";
                return 87;
            }
            lease->finish(timeout);
        }
        const auto backedOff = controller.snapshot(
            L"https://example.org/archive/file.gz");
        if (backedOff.limit != 2 || !backedOff.coolingDown) {
            std::cerr << "FAIL HTTPS backoff: limit=" << backedOff.limit
                      << ", cooldown=" << backedOff.coolingDown << "\n";
            return 88;
        }
        const std::array<std::wstring_view, 2> mirrorCandidates{
            L"https://example.org/archive/file.gz",
            L"https://fallback.example.net/archive/file.gz"};
        auto fallbackLease = controller.acquireAny(
            mirrorCandidates, std::stop_token{});
        if (!fallbackLease.has_value()
            || fallbackLease->candidateIndex != 1) {
            std::cerr << "FAIL acquire-any ignored ready mirror\n";
            return 94;
        }
        fallbackLease->lease.finish(success);

        HttpDownloadResult missing;
        missing.error = L"HTTP 404 Not Found";
        if (ClassifyDownloadConcurrencyFeedback(missing)
                != DownloadConcurrencyFeedback::NeutralFailure
            || ClassifyDownloadConcurrencyFeedback(timeout)
                != DownloadConcurrencyFeedback::TransientFailure) {
            std::cerr << "FAIL failure classification\n";
            return 89;
        }

        std::vector<DatedStationFile> files(8);
        for (std::size_t index = 0; index < files.size(); ++index) {
            files[index].station.downloadUrl = index < 4
                ? L"geonet-jp://0001/2025-01-01/file.gz"
                : L"https://noaa-cors-pds.s3.amazonaws.com/rinex/file.gz";
        }
        const auto order = BuildAutomaticDownloadDispatchOrder(files);
        const bool s3AppearsEarly = std::any_of(
            order.begin(),
            order.begin() + static_cast<std::ptrdiff_t>(
                std::min<std::size_t>(6, order.size())),
            [](const std::size_t index) { return index >= 4; });
        if (order.size() != files.size() || !s3AppearsEarly) {
            std::cerr << "FAIL weighted host dispatch\n";
            return 90;
        }

        auto& shared = SharedAutomaticDownloadConcurrency();
        if (&shared != &SharedAutomaticDownloadConcurrency()) {
            std::cerr << "FAIL download panes have separate controllers\n";
            return 92;
        }
        std::vector<AutomaticDownloadConcurrency::Lease> sharedLeases;
        for (int index = 0; index < 6; ++index) {
            auto lease = shared.tryAcquire(
                L"https://noaa-cors-pds.s3.amazonaws.com/rinex/file.gz");
            if (!lease) {
                std::cerr << "FAIL shared S3 slot " << index << "\n";
                return 93;
            }
            sharedLeases.push_back(std::move(*lease));
        }
        for (int index = 0; index < 6; ++index) {
            auto lease = shared.tryAcquire(
                L"https://igs.bkg.bund.de/root_ftp/IGS/obs/file.gz");
            if (!lease) {
                std::cerr << "FAIL shared BKG slot " << index << "\n";
                return 94;
            }
            sharedLeases.push_back(std::move(*lease));
        }
        const auto otherUrl = L"https://example.org/archive/file.gz";
        if (shared.tryAcquire(otherUrl)) {
            std::cerr << "FAIL shared transfer ceiling exceeded\n";
            return 95;
        }
        sharedLeases.front().finish(success);
        auto replacement = shared.tryAcquire(otherUrl);
        if (!replacement) {
            std::cerr << "FAIL released global slot was not reusable\n";
            return 96;
        }
        replacement->finish(success);
        for (auto& lease : sharedLeases) lease.finish(success);

        std::cout << "AUTO CONCURRENCY PASS: global="
                  << kAutomaticDownloadGlobalWorkerLimit
                  << ", S3 6->" << grownS3.limit << "/12"
                  << ", HTTPS 4->" << backedOff.limit
                  << ", Terras<=3, FTP<=3, SFTP=1"
                  << ", cancelled-wait=" << cancelledWaitMs << "ms\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--tolerant-range-kb") {
        const auto databaseDirectory =
            std::filesystem::absolute(arguments[2]);
        SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_INDEX_DIR",
            databaseDirectory.wstring().c_str());
        SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_DB_PATH",
            (databaseDirectory / L"gnss_availability.sqlite")
                .wstring().c_str());

        ObservationRangeOptions options;
        options.sampleInterval = "30S";

        const auto dateText = [](const QueryDate& value) {
            return FormatDate(value);
        };
        const auto selectedFromKnownGoodDay = [&](
            const ObservationNetwork network,
            const QueryDate& goodDate,
            const std::size_t limit) {
            NetworkSelection selection;
            selection.mask = NetworkBit(network);
            const auto daily = QueryDailyObservationFiles(
                goodDate,
                selection,
                {},
                KnowledgeBaseQueryMode::LocalKnowledgeBaseOnly,
                true);
            std::vector<StationFile> selected;
            if (daily.error.empty()) {
                const auto count = std::min(limit, daily.stations.size());
                selected.insert(
                    selected.end(),
                    daily.stations.begin(),
                    daily.stations.begin()
                        + static_cast<std::ptrdiff_t>(count));
            }
            return std::pair{std::move(selected), daily.error};
        };

        // 2026-01-21 is deliberately kept as a real IGS partial-source day:
        // WHU failed, while the other six mirrors have usable file routes.
        NetworkSelection igsSelection;
        igsSelection.mask = NetworkBit(ObservationNetwork::Igs);
        const QueryDate igsGap = MakeQueryDate(2026, 1, 21);
        const auto strictIgs = QueryObservationDateRange(
            igsGap, igsGap, igsSelection, options, {});
        const auto [igsSelected, igsSelectionError] =
            selectedFromKnownGoodDay(
                ObservationNetwork::Igs,
                MakeQueryDate(2026, 1, 20),
                256);
        if (strictIgs.error.empty()
            || !igsSelectionError.empty()
            || igsSelected.empty()) {
            std::cerr << "FAIL tolerant IGS fixture: strict-error="
                      << WideToUtf8(strictIgs.error)
                      << "; selection-error="
                      << WideToUtf8(igsSelectionError)
                      << "; selected=" << igsSelected.size() << "\n";
            return 66;
        }
        const auto tolerantIgs = LoadObservationDateRangeFilesForStations(
            igsGap,
            igsGap,
            igsSelection,
            options,
            std::span<const StationFile>(igsSelected),
            {});
        const bool igsHasUsableNonWhuRoute = std::any_of(
            tolerantIgs.periodFiles.begin(),
            tolerantIgs.periodFiles.end(),
            [](const DatedStationFile& dated) {
                return (!dated.station.downloadUrl.empty()
                        && dated.station.preferredSource != "WHU")
                    || std::any_of(
                        dated.station.downloadRoutes.begin(),
                        dated.station.downloadRoutes.end(),
                        [](const DownloadRoute& route) {
                            return route.source != "WHU"
                                && !route.url.empty();
                        });
            });
        if (!tolerantIgs.error.empty()
            || tolerantIgs.warning.empty()
            || tolerantIgs.periodFiles.empty()
            || !igsHasUsableNonWhuRoute) {
            std::cerr << "FAIL tolerant IGS hydration: files="
                      << tolerantIgs.periodFiles.size()
                      << "; warning=" << WideToUtf8(tolerantIgs.warning)
                      << "; error=" << WideToUtf8(tolerantIgs.error)
                      << "\n";
            return 67;
        }

        const auto checkFailedSingleSourceDay = [&]
            (const ObservationNetwork network,
             const char* label,
             const QueryDate& goodDate,
             const QueryDate& failedDate,
             const QueryDate& rangeStart,
             const QueryDate& rangeEnd,
             const int failureCode) {
            NetworkSelection selection;
            selection.mask = NetworkBit(network);
            const auto strict = QueryObservationDateRange(
                rangeStart, rangeEnd, selection, options, {});
            const auto [selected, selectionError] =
                selectedFromKnownGoodDay(network, goodDate, 32);
            if (strict.error.empty()
                || !selectionError.empty()
                || selected.empty()) {
                std::cerr << "FAIL tolerant " << label
                          << " fixture: strict-error="
                          << WideToUtf8(strict.error)
                          << "; selection-error="
                          << WideToUtf8(selectionError)
                          << "; selected=" << selected.size() << "\n";
                return failureCode;
            }
            const auto hydrated = LoadObservationDateRangeFilesForStations(
                rangeStart,
                rangeEnd,
                selection,
                options,
                std::span<const StationFile>(selected),
                {});
            const auto failedText = dateText(failedDate);
            const bool emittedFailedDate = std::any_of(
                hydrated.periodFiles.begin(),
                hydrated.periodFiles.end(),
                [&](const DatedStationFile& dated) {
                    return dateText(dated.date) == failedText;
                });
            if (!hydrated.error.empty()
                || hydrated.warning.empty()
                || hydrated.periodFiles.empty()
                || emittedFailedDate) {
                std::cerr << "FAIL tolerant " << label
                          << " hydration: files="
                          << hydrated.periodFiles.size()
                          << "; warning=" << WideToUtf8(hydrated.warning)
                          << "; error=" << WideToUtf8(hydrated.error)
                          << "; emitted-failed-date="
                          << emittedFailedDate << "\n";
                return failureCode + 1;
            }
            std::cout << label << " skipped failed day "
                      << WideToUtf8(failedText) << " and retained "
                      << hydrated.periodFiles.size()
                      << " known daily files\n";
            return 0;
        };

        const int csnStatus = checkFailedSingleSourceDay(
            ObservationNetwork::Csn,
            "CSN",
            MakeQueryDate(2020, 2, 4),
            MakeQueryDate(2020, 2, 5),
            MakeQueryDate(2020, 2, 4),
            MakeQueryDate(2020, 2, 5),
            68);
        if (csnStatus != 0) {
            return csnStatus;
        }
        const int rgpStatus = checkFailedSingleSourceDay(
            ObservationNetwork::Rgp,
            "RGP",
            MakeQueryDate(2022, 3, 20),
            MakeQueryDate(2022, 3, 21),
            MakeQueryDate(2022, 3, 20),
            MakeQueryDate(2022, 3, 21),
            70);
        if (rgpStatus != 0) {
            return rgpStatus;
        }
        const int rgnaStatus = checkFailedSingleSourceDay(
            ObservationNetwork::Rgna,
            "RGNA",
            MakeQueryDate(2020, 11, 28),
            MakeQueryDate(2020, 11, 27),
            MakeQueryDate(2020, 11, 27),
            MakeQueryDate(2020, 11, 28),
            72);
        if (rgnaStatus != 0) {
            return rgnaStatus;
        }
        std::cout << "TOLERANT RANGE PASS: IGS used six-source routes; "
                     "CSN/RGP failed days and RGNA's missing start day "
                     "were skipped with warnings\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--deferred-range-kb") {
        const auto databaseDirectory =
            std::filesystem::absolute(arguments[2]).wstring();
        SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_INDEX_DIR", databaseDirectory.c_str());
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::Epn);
        ObservationRangeOptions options;
        options.sampleInterval = "30S";
        const auto start = MakeQueryDate(2025, 1, 1);
        const auto end = MakeQueryDate(2025, 1, 3);
        const auto eager = QueryObservationDateRange(
            start, end, selection, options, {});
        const auto summary = QueryObservationDateRangeSummary(
            start, end, selection, options, {});
        const auto key = [](const StationFile& station) {
            return station.stationId.empty()
                ? station.fourCharId + ":" + station.country
                : station.stationId;
        };
        bool summariesMatch = eager.error.empty() && summary.error.empty()
            && summary.periodFilesDeferred
            && summary.periodFiles.empty()
            && eager.stations.size() == summary.stations.size();
        for (std::size_t index = 0;
             summariesMatch && index < eager.stations.size();
             ++index) {
            summariesMatch = key(eager.stations[index])
                    == key(summary.stations[index])
                && eager.stations[index].availableDays
                    == summary.stations[index].availableDays;
        }
        if (!summariesMatch || summary.stations.empty()) {
            std::cerr << "FAIL deferred summary: eager="
                      << eager.stations.size() << "; summary="
                      << summary.stations.size() << "; eager-error="
                      << WideToUtf8(eager.error) << "; summary-error="
                      << WideToUtf8(summary.error) << "\n";
            return 64;
        }
        std::vector<StationFile> selected;
        selected.push_back(summary.stations.front());
        if (summary.stations.size() > 1) {
            selected.push_back(summary.stations[1]);
        }
        std::size_t manifestProgressCalls = 0;
        std::size_t manifestCompletedUnits = 0;
        std::size_t manifestTotalUnits = 0;
        const auto hydrated = LoadObservationDateRangeFilesForStations(
            start,
            end,
            selection,
            options,
            std::span<const StationFile>(selected),
            {},
            [&](const ObservationManifestProgress& progress) {
                ++manifestProgressCalls;
                manifestCompletedUnits = progress.completedUnits;
                manifestTotalUnits = progress.totalUnits;
            });
        if (!hydrated.error.empty()
            || hydrated.periodFilesDeferred
            || hydrated.stations.empty()
            || hydrated.stations.size() > selected.size()
            || hydrated.periodFiles.empty()
            || hydrated.periodFiles.size() > selected.size() * 3U
            || manifestProgressCalls == 0
            || manifestTotalUnits == 0
            || manifestCompletedUnits != manifestTotalUnits) {
            std::cerr << "FAIL deferred hydration: stations="
                      << hydrated.stations.size() << "; files="
                      << hydrated.periodFiles.size() << "; progress="
                      << manifestCompletedUnits << "/"
                      << manifestTotalUnits << "; error="
                      << WideToUtf8(hydrated.error) << "\n";
            return 65;
        }
        std::cout << "DEFERRED RANGE PASS: " << summary.stations.size()
                  << " summary stations, " << hydrated.periodFiles.size()
                  << " selected daily files\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--igs-download-routes") {
        const auto databaseDirectory =
            std::filesystem::absolute(arguments[2]);
        const auto databasePath =
            (databaseDirectory / L"gnss_availability.sqlite").wstring();
        SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_DB_PATH", databasePath.c_str());
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::Igs);
        const auto result = QueryDailyObservationFiles(
            MakeQueryDate(2025, 1, 1), selection, {});
        const auto routed = std::count_if(
            result.stations.begin(), result.stations.end(),
            [](const StationFile& station) {
                return station.downloadRoutes.size() >= 2
                    && station.downloadRoutes.front().source == "BKG";
            });
        if (!result.error.empty() || routed < 100) {
            std::cerr << "FAIL IGS routes: stations="
                      << result.stations.size() << "; routed=" << routed
                      << "; error=" << WideToUtf8(result.error) << "\n";
            return 62;
        }
        std::cout << "IGS ROUTES PASS: " << routed
                  << " stations have ordered mirror fallback\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--igs-all-files") {
        const auto databaseDirectory =
            std::filesystem::absolute(arguments[2]);
        SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_DB_PATH",
            (databaseDirectory / L"gnss_availability.sqlite").c_str());
        const auto lookup = LoadIgsAvailabilityAllFiles(
            MakeQueryDate(2025, 1, 1));
        const auto routed = std::count_if(
            lookup.records.begin(), lookup.records.end(),
            [](const IndexedIgsAvailability& record) {
                return !record.station.downloadRoutes.empty()
                    && !record.station.downloadUrl.empty();
            });
        if (!lookup.error.empty() || lookup.records.empty() || routed == 0) {
            std::cerr << "FAIL IGS all-files: records="
                      << lookup.records.size() << "; routed=" << routed
                      << "; error=" << WideToUtf8(lookup.error) << "\n";
            return 63;
        }
        std::cout << "IGS ALL-FILES PASS: " << lookup.records.size()
                  << " logical files, " << routed << " routed\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--gps-week-kb") {
        const auto databaseDirectory =
            std::filesystem::absolute(arguments[2]).wstring();
        SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_INDEX_DIR", databaseDirectory.c_str());
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::GaGdc);
        const auto result = QueryGpsWeekObservationFiles(
            MakeQueryDate(2026, 8, 1), selection, {});
        if (!result.error.empty()
            || !result.coversFullGpsWeek
            || FormatDate(result.periodStart) != L"2026-07-26"
            || FormatDate(result.periodEnd) != L"2026-08-01"
            || result.periodFiles.size() != result.stations.size() * 7) {
            std::wcerr << L"FAIL GPS-week KB: " << result.error << L"\n";
            return 61;
        }
        std::cout << "GPS WEEK PASS: " << result.stations.size()
                  << " continuous stations, " << result.periodFiles.size()
                  << " daily files\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--benchmark-single-source-kb") {
        const auto databaseDirectory =
            std::filesystem::absolute(arguments[2]).wstring();
        SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_INDEX_DIR", databaseDirectory.c_str());
        const auto igsDatabase = (std::filesystem::path(databaseDirectory)
            / L"gnss_availability.sqlite").wstring();
        SetEnvironmentVariableW(
            L"GNSS_DOWNLOADER_DB_PATH", igsDatabase.c_str());
        const std::array<std::pair<ObservationNetwork, const char*>, 6> networks{{
            {ObservationNetwork::Igs, "IGS"},
            {ObservationNetwork::Ngs, "NGS"},
            {ObservationNetwork::Epn, "EPN"},
            {ObservationNetwork::GaGdc, "GA-GDC"},
            {ObservationNetwork::Sonel, "SONEL"},
            {ObservationNetwork::Rgp, "RGP"}
        }};
        for (const auto& [network, label] : networks) {
            NetworkSelection selection;
            selection.mask = NetworkBit(network);
            const auto started = std::chrono::steady_clock::now();
            const auto result = QueryDailyObservationFiles(
                MakeQueryDate(2025, 1, 1), selection, {});
            const auto milliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << label << ": " << result.stations.size()
                      << " stations, " << milliseconds << " ms\n";
        }
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--single-source-kb") {
        const auto databaseDirectory =
            std::filesystem::absolute(arguments[2]).wstring();
        if (!SetEnvironmentVariableW(
                L"GNSS_DOWNLOADER_INDEX_DIR",
                databaseDirectory.c_str())) {
            std::cerr << "FAIL single-source KB: cannot set index directory.\n";
            return 50;
        }
        NetworkSelection selection;
        selection.mask = static_cast<ObservationNetworkMask>(
            NetworkBit(ObservationNetwork::Ngs)
            | NetworkBit(ObservationNetwork::Epn)
            | NetworkBit(ObservationNetwork::GaGdc)
            | NetworkBit(ObservationNetwork::Rgp)
            | NetworkBit(ObservationNetwork::Sonel));
        const auto result = QueryDailyObservationFiles(
            MakeQueryDate(2025, 1, 1), selection, {});
        const auto count = [&](const ObservationNetwork network) {
            return std::count_if(
                result.stations.begin(), result.stations.end(),
                [network](const StationFile& station) {
                    return HasNetwork(station.availableNetworks, network);
                });
        };
        if (!result.error.empty() || !result.usedLocalKnowledgeBase
            || count(ObservationNetwork::Ngs) != 1723
            || count(ObservationNetwork::Epn) != 282
            || count(ObservationNetwork::GaGdc) != 1056
            || count(ObservationNetwork::Rgp) < 1
            || count(ObservationNetwork::Sonel) != 957
            || !std::all_of(
                result.stations.begin(), result.stations.end(), IsTrustedUrl)) {
            std::cerr << "FAIL single-source KB: stations="
                      << result.stations.size() << "; error="
                      << WideToUtf8(result.error) << "\n";
            return 51;
        }
        std::cout << "SINGLE-SOURCE KB PASS: all five databases loaded and merged\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--db-no-downgrade") {
        const auto databasePath = std::filesystem::absolute(arguments[2]);
        {
            std::ofstream empty(
                databasePath,
                std::ios::binary | std::ios::trunc);
        }
        if (!SetEnvironmentVariableW(
                L"GNSS_DOWNLOADER_DB_PATH",
                databasePath.c_str())) {
            std::cerr << "FAIL DB no-downgrade: cannot set database path.\n";
            return 46;
        }
        const QueryDate testDate = MakeQueryDate(2020, 1, 1);
        const std::array<const char*, 7> keys{
            "CDDIS", "BKG", "WHU", "KASI", "IGN", "ESA", "SOPAC"};
        std::vector<IgsAvailabilitySourceScan> complete;
        for (std::size_t index = 0; index < keys.size(); ++index) {
            IgsAvailabilitySourceScan source;
            source.sourceKey = keys[index];
            source.sourcePriority = static_cast<int>(index + 1);
            source.success = true;
            complete.push_back(std::move(source));
        }
        std::wstring error;
        if (!StoreIgsAvailability(testDate, complete, error)) {
            std::cerr << "FAIL DB no-downgrade initial store: "
                      << WideToUtf8(error) << "\n";
            return 47;
        }
        std::vector<IgsAvailabilitySourceScan> degraded = complete;
        for (std::size_t index = 1; index < degraded.size(); ++index) {
            degraded[index].success = false;
            degraded[index].networkFailure = true;
            degraded[index].error = L"simulated outage";
        }
        if (!StoreIgsAvailability(testDate, degraded, error)) {
            std::cerr << "FAIL DB no-downgrade degraded store: "
                      << WideToUtf8(error) << "\n";
            return 48;
        }
        const auto stats = GetIgsAvailabilityDateStats(testDate);
        const int completed = static_cast<int>(std::count_if(
            stats.sources.begin(),
            stats.sources.end(),
            [](const IgsAvailabilitySourceStats& source) {
                return source.complete;
            }));
        if (!stats.error.empty() || stats.sources.size() != 7
            || completed != 7) {
            std::cerr << "FAIL DB no-downgrade: completed=" << completed
                      << "; sources=" << stats.sources.size()
                      << "; error=" << WideToUtf8(stats.error) << "\n";
            return 49;
        }
        std::cout << "DB NO-DOWNGRADE PASS: retained 7/7 after simulated 1/7\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--seven-source-igs") {
        const auto databasePath = std::filesystem::absolute(arguments[2]);
        if (!SetEnvironmentVariableW(
                L"GNSS_DOWNLOADER_DB_PATH",
                databasePath.c_str())) {
            std::cerr << "FAIL seven-source IGS: cannot set database path.\n";
            return 42;
        }
        const QueryDate testDate = MakeQueryDate(2018, 1, 1);
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::Igs);
        const auto query =
            QueryDailyObservationFiles(testDate, selection, {});
        const auto stats = GetIgsAvailabilityDateStats(testDate);
        const bool routesPresent = std::all_of(
            query.stations.begin(),
            query.stations.end(),
            [](const StationFile& station) {
                return !station.preferredSource.empty()
                    && !station.availabilitySources.empty();
            });
        const auto cddis = std::find_if(
            stats.sources.begin(),
            stats.sources.end(),
            [](const IgsAvailabilitySourceStats& source) {
                return source.sourceKey == "CDDIS";
            });
        if (!query.error.empty()
            || !query.usedIgsLocalIndex
            || query.stations.size() != 483
            || !stats.error.empty()
            || stats.sourceRoutes != 2939
            || stats.multiSourceStations != 462
            || stats.logicalObservationFiles != 1112
            || stats.observationSourceRoutes != 4596
            || stats.physicalFileRoutes != 4598
            || stats.multiSourceObservationFiles != 661
            || cddis == stats.sources.end()
            || cddis->stations != 454
            || cddis->files != 649
            || cddis->preferredStations != 454
            || !routesPresent) {
            std::cerr << "FAIL seven-source IGS: stations="
                      << query.stations.size()
                      << "; routes=" << stats.sourceRoutes
                      << "; multi=" << stats.multiSourceStations
                      << "; error=" << WideToUtf8(query.error)
                      << "\n";
            return 43;
        }
        std::cout << "SEVEN-SOURCE IGS PASS: 483 stations, 1112 logical files, "
                     "4598 physical routes\n";
        return 0;
    }
    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--local-igs-miss") {
        const auto databasePath = std::filesystem::absolute(arguments[2]);
        if (!SetEnvironmentVariableW(
                L"GNSS_DOWNLOADER_DB_PATH",
                databasePath.c_str())) {
            std::cerr << "FAIL local IGS miss: cannot set database path.\n";
            return 44;
        }
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::Igs);
        const QueryDate missingDate = MakeQueryDate(2017, 12, 31);
        const auto started = std::chrono::steady_clock::now();
        const auto missing =
            QueryDailyObservationFiles(missingDate, selection, {});
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (!missing.stations.empty() || missing.error.empty()
            || seconds >= 1.0) {
            std::cerr << "FAIL local IGS miss: stations="
                      << missing.stations.size()
                      << "; seconds=" << seconds
                      << "; error=" << WideToUtf8(missing.error) << "\n";
            return 45;
        }
        std::cout << "LOCAL-ONLY IGS MISS PASS: no network fallback; "
                  << seconds << " s\n";
        return 0;
    }
    if (argumentCount >= 2
        && (std::wstring(arguments[1]) == L"--local-igs"
            || std::wstring(arguments[1]) == L"--seed-igs")) {
        if (std::wstring(arguments[1]) == L"--local-igs") {
            if (argumentCount < 3) {
                std::cerr << "FAIL local IGS: database path is required.\n";
                return 40;
            }
            const auto databasePath = std::filesystem::absolute(arguments[2]);
            if (!SetEnvironmentVariableW(
                    L"GNSS_DOWNLOADER_DB_PATH",
                    databasePath.c_str())) {
                std::cerr << "FAIL local IGS: cannot set database path.\n";
                return 40;
            }
        }
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::Igs);
        const auto local = QueryDailyObservationFiles(date, selection, {});
        if (!local.error.empty() || !local.usedIgsLocalIndex
            || local.stations.size() != 444
            || !std::all_of(
                local.stations.begin(),
                local.stations.end(),
                IsTrustedUrl)) {
            std::cerr << "FAIL local IGS: stations=" << local.stations.size()
                      << "; local=" << local.usedIgsLocalIndex
                      << "; error=" << WideToUtf8(local.error) << "\n";
            return 41;
        }
        std::cout << "LOCAL IGS PASS: 444 stations\n";
        return 0;
    }
    if (argumentCount >= 2
        && std::wstring(arguments[1]) == L"--csn-legacy") {
        const auto response = HttpGetText(
            L"https://gps.csn.uchile.cl/data/2025/001/",
            {},
            4U * 1024U * 1024U);
        std::wcout << L"CSN legacy status " << response.statusCode
                   << L"; bytes " << response.body.size()
                   << L"; error " << response.error << L"\n";
        return response.ok() ? 0 : 34;
    }
    if (argumentCount >= 2
        && (std::wstring(arguments[1]) == L"--csn"
            || std::wstring(arguments[1]) == L"--csn-download")) {
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::Csn);
        const QueryDate csnDate = MakeQueryDate(2025, 1, 1);
        const auto csn = QueryDailyObservationFiles(csnDate, selection, {});
        const auto mapped = std::count_if(
            csn.stations.begin(),
            csn.stations.end(),
            [](const StationFile& station) {
                return station.hasCoordinate;
            });
        const auto has = [&csn](const std::string_view prefix) {
            return std::any_of(
                csn.stations.begin(),
                csn.stations.end(),
                [prefix](const StationFile& station) {
                    return station.stationId.starts_with(prefix);
                });
        };
        std::wcout << L"CSN 2025-01-01: " << csn.stations.size()
                   << L"; mapped " << mapped
                   << L"; unmapped " << csn.filesWithoutCoordinates
                   << L"\n";
        if (!csn.error.empty()
            || csn.stations.size() < 100
            || mapped < 90
            || !has("ATJN")
            || !has("BTON")
            || !has("PMEJ")
            || !std::all_of(
                csn.stations.begin(),
                csn.stations.end(),
                IsTrustedUrl)) {
            std::cerr << "FAIL CSN: " << WideToUtf8(csn.error)
                      << "; warning: " << WideToUtf8(csn.warning) << "\n";
            return 30;
        }
        if (std::wstring(arguments[1]) == L"--csn-download") {
            if (argumentCount < 3) {
                std::cerr << "CSN download test requires a destination folder.\n";
                return 31;
            }
            const auto sample = std::find_if(
                csn.stations.begin(),
                csn.stations.end(),
                [](const StationFile& station) {
                    return station.fourCharId == "BING";
                });
            if (sample == csn.stations.end()) {
                std::cerr << "CSN BING sample is unavailable.\n";
                return 32;
            }
            const std::filesystem::path destination =
                std::filesystem::path(arguments[2]) / sample->fileName;
            const auto download = HttpDownloadFile(
                sample->downloadUrl,
                destination,
                {},
                {});
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(
                destination,
                sizeError);
            if (!download.success || sizeError || size < 100'000) {
                std::cerr << "FAIL CSN download: "
                          << WideToUtf8(download.error) << "\n";
                return 33;
            }
            std::wcout << L"CSN download PASS: "
                       << destination.wstring()
                       << L" (" << size << L" bytes)\n";
        }
        std::wcout << L"CSN PASS\n";
        return 0;
    }
    if (argumentCount >= 2
        && std::wstring(arguments[1]) == L"--americas-direct") {
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::Cacs);
        const auto cacs = QueryDailyObservationFiles(
            MakeQueryDate(2024, 6, 15),
            selection,
            {});
        const auto mapped = std::count_if(
            cacs.stations.begin(),
            cacs.stations.end(),
            [](const StationFile& station) {
                return station.hasCoordinate;
            });
        std::wcout << L"CACS 2024-06-15: "
                   << cacs.stations.size()
                   << L"; mapped " << mapped << L"\n";
        if (!cacs.error.empty()
            || cacs.stations.size() < 20
            || mapped < 15
            || !std::all_of(
                cacs.stations.begin(),
                cacs.stations.end(),
                IsTrustedUrl)) {
            std::cerr << "FAIL CACS: "
                      << WideToUtf8(cacs.error)
                      << "; warning: " << WideToUtf8(cacs.warning)
                      << "\n";
            return 35;
        }
        std::wcout << L"AMERICAS DIRECT PASS\n";
        return 0;
    }
    if (argumentCount >= 2
        && std::wstring(arguments[1]) == L"--panama") {
        NetworkSelection selection;
        selection.mask = NetworkBit(ObservationNetwork::PanamaCors);
        const auto panama = QueryDailyObservationFiles(
            MakeQueryDate(2026, 7, 1),
            selection,
            {});
        std::wcout << L"PAN CORS 2026-07-01: "
                   << panama.stations.size() << L"\n";
        if (!panama.error.empty()
            || panama.stations.size() < 5
            || !std::all_of(
                panama.stations.begin(),
                panama.stations.end(),
                IsTrustedUrl)) {
            std::cerr << "FAIL PAN CORS: "
                      << WideToUtf8(panama.error)
                      << "; warning: " << WideToUtf8(panama.warning)
                      << "\n";
            return 36;
        }
        if (argumentCount >= 3) {
            const std::filesystem::path destination =
                std::filesystem::path(arguments[2])
                / panama.stations.front().fileName;
            const auto download = HttpDownloadFile(
                panama.stations.front().downloadUrl,
                destination,
                {},
                {});
            std::error_code sizeError;
            const auto size = std::filesystem::file_size(
                destination,
                sizeError);
            if (!download.success || sizeError || size == 0) {
                std::cerr << "FAIL PAN CORS download: "
                          << WideToUtf8(download.error) << "\n";
                return 37;
            }
            std::wcout << L"PAN CORS download PASS: "
                       << destination.wstring()
                       << L" (" << size << L" bytes)\n";
        }
        std::wcout << L"PANAMA PASS\n";
        return 0;
    }
    if (argumentCount >= 2
        && std::wstring(arguments[1]) == L"--new-networks") {
        const auto run = [](const QueryDate& queryDate,
                            const ObservationNetworkMask mask) {
            NetworkSelection selected;
            selected.mask = mask;
            return QueryDailyObservationFiles(queryDate, selected, {});
        };
        const auto count = [](const QueryResult& query,
                              const ObservationNetwork network) {
            return std::count_if(
                query.stations.begin(),
                query.stations.end(),
                [network](const StationFile& station) {
                    return HasNetwork(station.availableNetworks, network);
                });
        };
        const auto has = [](const QueryResult& query,
                            const std::string_view prefix) {
            return std::any_of(
                query.stations.begin(),
                query.stations.end(),
                [prefix](const StationFile& station) {
                    return station.stationId.starts_with(prefix);
                });
        };
        const auto safe = [](const QueryResult& query) {
            return std::all_of(
                query.stations.begin(),
                query.stations.end(),
                IsTrustedUrl);
        };

        const auto cut = run(
            MakeQueryDate(2024, 4, 9),
            NetworkBit(ObservationNetwork::Cut));
        std::wcout << L"CUT 2024-04-09: " << cut.stations.size() << L"\n";
        if (!cut.error.empty() || count(cut, ObservationNetwork::Cut) != 5
            || !safe(cut)) {
            std::cerr << "FAIL CUT: " << WideToUtf8(cut.error) << "\n";
            return 20;
        }

        const auto csn = run(
            date,
            NetworkBit(ObservationNetwork::Csn));
        std::wcout << L"CSN 2024-06-15: " << csn.stations.size() << L"\n";
        if (!csn.error.empty() || count(csn, ObservationNetwork::Csn) < 100
            || !has(csn, "ATJN") || !has(csn, "BTON") || !has(csn, "PMEJ")
            || !safe(csn)) {
            std::cerr << "FAIL CSN: " << WideToUtf8(csn.error) << "\n";
            return 21;
        }

        const auto iceland = run(
            MakeQueryDate(2025, 1, 1),
            NetworkBit(ObservationNetwork::EposIceland));
        std::wcout << L"Iceland 2025-01-01: "
                   << iceland.stations.size()
                   << L"; EPOS "
                   << count(iceland, ObservationNetwork::EposIceland)
                   << L"; safe " << (safe(iceland) ? L"yes" : L"no")
                   << L"\n";
        if (!iceland.error.empty()
            || count(iceland, ObservationNetwork::EposIceland) < 1
            || !has(iceland, "MYVA") || !safe(iceland)) {
            std::cerr << "FAIL Iceland: "
                      << WideToUtf8(iceland.error) << "\n";
            return 22;
        }

        const auto regional = run(
            date,
            static_cast<ObservationNetworkMask>(
                NetworkBit(ObservationNetwork::Rbmc)
                | NetworkBit(ObservationNetwork::Ramsac)
                | NetworkBit(ObservationNetwork::Rgp)
                | NetworkBit(ObservationNetwork::SopacGarner)));
        std::wcout << L"Complete regional sources 2024-06-15: "
                   << regional.stations.size()
                   << L"; RBMC "
                   << count(regional, ObservationNetwork::Rbmc)
                   << L"; RAMSAC "
                   << count(regional, ObservationNetwork::Ramsac)
                   << L"; RGP "
                   << count(regional, ObservationNetwork::Rgp)
                   << L"; SOPAC/GARNER "
                   << count(regional, ObservationNetwork::SopacGarner)
                   << L"; unmapped "
                   << regional.filesWithoutCoordinates
                   << L"\n";
        const auto countUrl = [&regional](const std::wstring_view text) {
            return std::count_if(
                regional.stations.begin(),
                regional.stations.end(),
                [text](const StationFile& station) {
                    return station.downloadUrl.find(text)
                        != std::wstring::npos;
                });
        };
        std::wcout << L"Selected download sources: RBMC "
                   << countUrl(L"geoftp.ibge.gov.br")
                   << L"; RAMSAC " << countUrl(L"ramsac.ign.gob.ar")
                   << L"; REGNA-ROU " << countUrl(L"pp.igm.gub.uy")
                   << L"; RGP " << countUrl(L"rgpdata.ensg.eu")
                   << L"; SOPAC/GARNER " << countUrl(L"garner.ucsd.edu")
                   << L"\n";
        const auto regna = run(
            MakeQueryDate(2026, 7, 27),
            NetworkBit(ObservationNetwork::RegnaRou));
        std::wcout << L"REGNA-ROU current anonymous archive 2026-07-27: "
                   << count(regna, ObservationNetwork::RegnaRou)
                   << L"; unmapped "
                   << regna.filesWithoutCoordinates
                   << L"\n";
        if (argumentCount >= 3
            && std::wstring(arguments[2]) == L"--dump-regional") {
            for (const auto& station : regional.stations) {
                std::cout << "REGIONAL_STATION "
                          << station.fourCharId << " "
                          << station.availableNetworks << " "
                          << WideToUtf8(station.downloadUrl) << "\n";
            }
        }
        if (count(regional, ObservationNetwork::Rbmc) < 50
            || count(regional, ObservationNetwork::Ramsac) < 20
            || count(regional, ObservationNetwork::Rgp) < 20
            || count(regional, ObservationNetwork::SopacGarner) < 20
            || count(regna, ObservationNetwork::RegnaRou) < 20
            || !safe(regional)
            || !safe(regna)) {
            std::cerr << "FAIL complete regional sources: "
                      << WideToUtf8(regional.error)
                      << "; warning: " << WideToUtf8(regional.warning)
                      << "; REGNA: " << WideToUtf8(regna.error)
                      << "\n";
            return 23;
        }
        std::wcout << L"NEW NETWORKS PASS\n";
        return 0;
    }

    NetworkSelection selection;
    selection.mask = static_cast<ObservationNetworkMask>(
        NetworkBit(ObservationNetwork::Igs)
        | NetworkBit(ObservationNetwork::Mgex)
        | NetworkBit(ObservationNetwork::Ngs)
        | NetworkBit(ObservationNetwork::Epn));

    std::wcout << L"Query " << FormatDate(date)
               << L" / DOY " << FormatDoy(date.dayOfYear) << L"\n";
    const QueryResult result = QueryDailyObservationFiles(date, selection, {});
    if (!result.error.empty()) {
        std::cerr << "FAIL: " << WideToUtf8(result.error) << "\n";
        return 2;
    }

    const auto countNetwork = [&](const ObservationNetwork network) {
        return std::count_if(
            result.stations.begin(),
            result.stations.end(),
            [network](const StationFile& station) {
                return HasNetwork(station.availableNetworks, network);
            });
    };
    const auto containsPrefix = [&](const std::string& prefix) {
        return std::any_of(
            result.stations.begin(),
            result.stations.end(),
            [&](const StationFile& station) {
                return station.stationId.starts_with(prefix);
            });
    };
    const auto mapped = std::count_if(
        result.stations.begin(),
        result.stations.end(),
        [](const StationFile& station) {
            return station.hasCoordinate;
        });
    const bool urlsSafe = std::all_of(
        result.stations.begin(),
        result.stations.end(),
        IsTrustedUrl);
    std::unordered_set<std::string> stationIds;
    const bool uniqueIds = std::all_of(
        result.stations.begin(),
        result.stations.end(),
        [&](const StationFile& station) {
            return stationIds.insert(station.stationId).second;
        });

    const auto igsCount = countNetwork(ObservationNetwork::Igs);
    const auto mgexCount = countNetwork(ObservationNetwork::Mgex);
    const auto ngsCount = countNetwork(ObservationNetwork::Ngs);
    const auto epnCount = countNetwork(ObservationNetwork::Epn);

    std::wcout << L"De-duplicated stations: " << result.stations.size() << L"\n"
               << L"IGS: " << igsCount << L"\n"
               << L"MGEX: " << mgexCount << L"\n"
               << L"NGS: " << ngsCount << L"\n"
               << L"EPN: " << epnCount << L"\n"
               << L"Mapped: " << mapped << L"\n"
               << L"Unmapped: " << result.filesWithoutCoordinates << L"\n"
               << L"Unique IDs: " << (uniqueIds ? L"pass" : L"FAIL") << L"\n"
               << L"URL whitelist: " << (urlsSafe ? L"pass" : L"FAIL") << L"\n";
    if (!result.warning.empty()) {
        std::cerr << "Warning: " << WideToUtf8(result.warning) << "\n";
    }

    if (igsCount < 400
        || mgexCount < 250
        || ngsCount < 1'500
        || epnCount < 200
        || mapped < 1'800
        || !uniqueIds
        || !urlsSafe
        || !containsPrefix("ZIM2")
        || !containsPrefix("ZIMM")
        || !containsPrefix("1LSU")
        || !containsPrefix("ADAR")) {
        std::wcerr << L"FAIL: local knowledge-base expectations were not met.\n";
        return 3;
    }
    std::wcout << L"PASS\n";

    if (argumentCount >= 2
        && std::wstring(arguments[1]) == L"--expanded") {
        NetworkSelection expandedSelection;
        expandedSelection.mask = static_cast<ObservationNetworkMask>(
            NetworkBit(ObservationNetwork::GaGdc)
            | NetworkBit(ObservationNetwork::Sonel));
        const auto expanded = QueryDailyObservationFiles(
            date,
            expandedSelection,
            {});
        const auto expandedCount = [&](const ObservationNetwork network) {
            return std::count_if(
                expanded.stations.begin(),
                expanded.stations.end(),
                [network](const StationFile& station) {
                    return HasNetwork(station.availableNetworks, network);
                });
        };
        std::wcout << L"GA-GDC + SONEL: " << expanded.stations.size()
                   << L" de-duplicated; GA-GDC "
                   << expandedCount(ObservationNetwork::GaGdc)
                   << L"; SONEL "
                   << expandedCount(ObservationNetwork::Sonel) << L"\n";
        if (!expanded.error.empty()
            || expandedCount(ObservationNetwork::GaGdc) < 500
            || expandedCount(ObservationNetwork::Sonel) < 500
            || !std::all_of(
                expanded.stations.begin(),
                expanded.stations.end(),
                IsTrustedUrl)) {
            std::cerr << "FAIL expanded: "
                      << WideToUtf8(expanded.error) << "\n";
            return 7;
        }

        NetworkSelection rgnaSelection;
        rgnaSelection.mask = NetworkBit(ObservationNetwork::Rgna);
        const auto rgna = QueryDailyObservationFiles(
            MakeQueryDate(2026, 7, 25),
            rgnaSelection,
            {});
        std::wcout << L"RGNA 2026-07-25: " << rgna.stations.size()
                   << L" stations\n";
        if (!rgna.error.empty() || rgna.stations.size() < 15
            || !std::all_of(
                rgna.stations.begin(),
                rgna.stations.end(),
                [](const StationFile& station) {
                    return IsTrustedUrl(station)
                        && !station.remoteParts.empty()
                        && station.remoteParts.size() <= 24;
                })) {
            std::cerr << "FAIL RGNA: " << WideToUtf8(rgna.error) << "\n";
            return 8;
        }

        NetworkSelection trigNetSelection;
        trigNetSelection.mask = NetworkBit(ObservationNetwork::TrigNet);
        const auto trigNet = QueryDailyObservationFiles(
            date,
            trigNetSelection,
            {});
        if (!trigNet.error.empty()) {
            std::cerr << "TrigNet live warning: "
                      << WideToUtf8(trigNet.error) << "\n";
        } else {
            std::wcout << L"TrigNet 2024-06-15: "
                       << trigNet.stations.size() << L" stations\n";
        }
        std::wcout << L"EXPANDED PASS\n";

        if (argumentCount >= 3) {
            const std::filesystem::path sampleDirectory(arguments[2]);
            std::filesystem::create_directories(sampleDirectory);
            const auto gaSample = std::find_if(
                expanded.stations.begin(),
                expanded.stations.end(),
                [](const StationFile& station) {
                    return HasNetwork(
                        station.availableNetworks,
                        ObservationNetwork::GaGdc);
                });
            const auto sonelSample = std::find_if(
                expanded.stations.begin(),
                expanded.stations.end(),
                [](const StationFile& station) {
                    return HasNetwork(
                        station.availableNetworks,
                        ObservationNetwork::Sonel)
                        && !HasNetwork(
                            station.availableNetworks,
                            ObservationNetwork::GaGdc);
                });
            if (gaSample == expanded.stations.end()
                || sonelSample == expanded.stations.end()
                || rgna.stations.empty()) {
                std::cerr << "FAIL: expanded download samples unavailable.\n";
                return 9;
            }
            const auto gaDownload = HttpDownloadFile(
                gaSample->downloadUrl,
                sampleDirectory / Utf8ToWide(gaSample->fileName),
                {},
                {});
            const auto sonelDownload = HttpDownloadFile(
                sonelSample->downloadUrl,
                sampleDirectory / Utf8ToWide(sonelSample->fileName),
                {},
                {});
            auto rgnaParts = rgna.stations.front().remoteParts;
            rgnaParts.resize(std::min<std::size_t>(2, rgnaParts.size()));
            const auto rgnaDownload = HttpDownloadRgnaBundle(
                rgnaParts,
                sampleDirectory / L"RGNA-two-hour-sample.zip",
                {},
                {});
            std::wcout << L"Download samples: GA "
                       << gaDownload.bytesWritten << L", SONEL "
                       << sonelDownload.bytesWritten << L", RGNA "
                       << rgnaDownload.bytesWritten << L" bytes\n";
            if (!gaDownload.success
                || !sonelDownload.success
                || !rgnaDownload.success) {
                std::cerr << "FAIL expanded downloads: GA="
                          << WideToUtf8(gaDownload.error)
                          << " SONEL=" << WideToUtf8(sonelDownload.error)
                          << " RGNA=" << WideToUtf8(rgnaDownload.error)
                          << "\n";
                return 10;
            }
        }
    }

    if (argumentCount >= 3
        && std::wstring(arguments[1]) == L"--download-sample") {
        const auto sample = std::find_if(
            result.stations.begin(),
            result.stations.end(),
            [](const StationFile& station) {
                return station.stationId.starts_with("ZIM2");
            });
        if (sample == result.stations.end()) {
            std::cerr << "FAIL: ZIM2 sample is unavailable.\n";
            return 4;
        }
        const auto destination =
            std::filesystem::path(arguments[2]) / Utf8ToWide(sample->fileName);
        const auto download = HttpDownloadFile(
            sample->downloadUrl,
            destination,
            {},
            {});
        if (!download.success) {
            std::cerr << "FAIL download: " << WideToUtf8(download.error) << "\n";
            return 5;
        }
        std::ifstream input(destination, std::ios::binary);
        unsigned char magic[2]{};
        input.read(reinterpret_cast<char*>(magic), 2);
        if (!input || magic[0] != 0x1f || magic[1] != 0x8b
            || download.bytesWritten < 1024) {
            std::cerr << "FAIL: sample is not a valid gzip stream.\n";
            return 6;
        }
        std::wcout << L"Download sample: " << destination
                   << L" (" << download.bytesWritten << L" bytes)\n";
    }
    return 0;
}
