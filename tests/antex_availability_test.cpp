#include "AntexAvailability.h"
#include "DateUtil.h"

#include <windows.h>

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool Require(const bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << message << "\n";
    return false;
}

bool SameDate(const QueryDate& lhs, const QueryDate& rhs) {
    return lhs.year == rhs.year && lhs.month == rhs.month && lhs.day == rhs.day;
}

const AntexArchiveFile* Find(
    const std::vector<AntexArchiveFile>& files, const std::string& model,
    const int revision = -2, const std::string& variant = "") {
    const auto it = std::ranges::find_if(files, [&](const auto& file) {
        return file.modelName == model
            && (revision == -2 || file.revisionWeek == revision)
            && (variant.empty() || file.variant == variant);
    });
    return it == files.end() ? nullptr : &*it;
}

void TestRangeUsesEffectiveAntexRevisionWindows() {
    const auto temp = std::filesystem::temp_directory_path()
        / L"gnss_downloader_antex_range_revision_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    std::filesystem::remove(temp.wstring() + L"-wal", ignored);
    std::filesystem::remove(temp.wstring() + L"-shm", ignored);
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_ANTEX_DB_PATH", temp.c_str());

    const auto files = ParseAntexDirectoryListing(
        "igs20_2250.atx igs20_2300.atx",
        L"https://example.test/antex/", "TEST", "TEST");
    assert(files.size() == 2);
    std::vector<AntexSourceScan> scans;
    for (int source = 1; source <= 4; ++source) {
        AntexSourceScan scan;
        scan.sourceKey = "TEST" + std::to_string(source);
        scan.sourcePriority = source;
        scan.success = true;
        scan.files = files;
        for (auto& file : scan.files) {
            file.downloadUrl = L"https://example.test/"
                + std::to_wstring(source) + L"/"
                + std::wstring(file.fileName.begin(), file.fileName.end());
        }
        scans.push_back(std::move(scan));
    }
    std::wstring error;
    assert(StoreAntexCatalog(scans, error));
    const auto range = LoadAntexAvailabilityRange(
        MakeQueryDate(2023, 3, 1), MakeQueryDate(2024, 3, 1));
    assert(range.error.empty());
    assert(range.products.size() == 2);
    const auto old = std::ranges::find_if(range.products,
        [](const auto& item) { return item.revisionWeek == 2250; });
    const auto newer = std::ranges::find_if(range.products,
        [](const auto& item) { return item.revisionWeek == 2300; });
    assert(old != range.products.end() && newer != range.products.end());
    // GPS week 2250 starts on 2023-02-19 and week 2300 on 2024-02-04.
    assert(SameDate(old->validFrom, MakeQueryDate(2023, 2, 19)));
    assert(SameDate(old->validTo, MakeQueryDate(2024, 2, 3)));
    assert(SameDate(newer->validFrom, MakeQueryDate(2024, 2, 4)));

    std::filesystem::remove(temp, ignored);
    std::filesystem::remove(temp.wstring() + L"-wal", ignored);
    std::filesystem::remove(temp.wstring() + L"-shm", ignored);
}

bool TestSingleDayAndRangeUseSameRevision() {
    const auto temp = std::filesystem::temp_directory_path()
        / L"gnss_downloader_antex_consistent_dates_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_ANTEX_DB_PATH", temp.c_str());

    const auto files = ParseAntexDirectoryListing(
        "igs20_rcv.atx2.gz igs20_2425_rcv.atx2.gz",
        L"https://example.test/antex/", "IGS_TEST", "IGS");
    std::vector<AntexSourceScan> scans;
    for (int source = 1; source <= 4; ++source) {
        AntexSourceScan scan;
        scan.sourceKey = "TEST" + std::to_string(source);
        scan.sourcePriority = source;
        scan.success = true;
        scan.files = files;
        scans.push_back(std::move(scan));
    }
    std::wstring error;
    bool ok = Require(StoreAntexCatalog(scans, error),
        "store revision consistency fixture");
    if (ok) {
        const auto before = MakeQueryDate(2023, 6, 1);
        const auto singleBefore = LoadAntexAvailability(before);
        const auto rangeBefore = LoadAntexAvailabilityRange(
            before, MakeQueryDate(2023, 7, 1));
        ok &= Require(singleBefore.products.size() == 1
            && rangeBefore.products.size() == 1
            && singleBefore.products[0].logicalKey
                == rangeBefore.products[0].logicalKey
            && singleBefore.products[0].currentAlias,
            "early alias is available in both query modes");

        const auto numberedDate = MakeQueryDate(2026, 6, 28);
        const auto singleNumbered = LoadAntexAvailability(numberedDate);
        const auto rangeNumbered = LoadAntexAvailabilityRange(
            numberedDate, numberedDate);
        ok &= Require(singleNumbered.products.size() == 1
            && rangeNumbered.products.size() == 1
            && singleNumbered.products[0].logicalKey
                == rangeNumbered.products[0].logicalKey
            && !singleNumbered.products[0].currentAlias,
            "numbered revision is available in both query modes");

        const auto after = MakeQueryDate(2026, 7, 5);
        const auto singleAfter = LoadAntexAvailability(after);
        const auto rangeAfter = LoadAntexAvailabilityRange(after, after);
        ok &= Require(singleAfter.products.size() == 1
            && rangeAfter.products.size() == 1
            && singleAfter.products[0].logicalKey
                == rangeAfter.products[0].logicalKey
            && singleAfter.products[0].currentAlias,
            "single-day and range select the same effective alias");
    }
    std::filesystem::remove(temp, ignored);
    std::filesystem::remove(temp.wstring() + L"-wal", ignored);
    std::filesystem::remove(temp.wstring() + L"-shm", ignored);
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    const std::string igsListing = R"(
      <a href="igs20.atx">igs20.atx</a>
      <a href="igs20_2425.atx.gz">igs20_2425.atx.gz</a>
      <a href="igs20_2425_rcv.atx2.gz">igs20_2425_rcv.atx2.gz</a>
      <a href="igs14_2188.atx">igs14_2188.atx</a>
      <a href="antex14.txt">antex14.txt</a>)";
    const auto igs = ParseAntexDirectoryListing(igsListing,
        L"https://files.igs.org/pub/station/general/", "IGS_TEST", "IGS");
    ok &= Require(igs.size() == 4, "IGS parser count");
    const auto* receiver = Find(igs, "IGS20", 2425, "RCV");
    ok &= Require(receiver != nullptr && receiver->scope == "RECEIVER"
        && receiver->formatVersion == "2.0", "ANTEX 2 receiver classification");

    const std::string epnListing = R"(
      epn_20.atx epn_20_r3.atx epnc_20.atx igs20_2425.atx)";
    const auto epn = ParseAntexDirectoryListing(epnListing,
        L"https://epncb.oma.be/pub/station/general/", "EPN_TEST", "EPN");
    ok &= Require(epn.size() == 4, "EPN parser count");
    const auto* epnc = Find(epn, "EPNC20");
    ok &= Require(epnc != nullptr && epnc->scope == "RECEIVER"
        && epnc->productFamily == "EPN_SUPPLEMENT", "EPNC classification");

    const std::string codeListing = R"(
      <a>I20.ATX</a><a>I20_2417.ATX</a><a>M20.ATX</a><a>ATX</a>)";
    const auto code = ParseAntexDirectoryListing(codeListing,
        L"https://www.aiub.unibe.ch/download/CODE/", "CODE_TEST", "CODE");
    ok &= Require(code.size() == 4, "CODE parser count including bare ATX");
    ok &= Require(Find(code, "M20") != nullptr, "CODE MGEX classification");

    const std::string ngsListing = R"(
      LoadFile?file=ngs20.atx LoadFile?file=ngs14.atx)";
    const auto ngs = ParseAntexDirectoryListing(ngsListing,
        L"https://nweb.ngs.noaa.gov/ANTCAL/LoadFile?file=", "NGS_TEST", "NGS");
    ok &= Require(ngs.size() == 2, "NGS parser count");

    const auto temp = std::filesystem::temp_directory_path()
        / L"gnss_downloader_antex_test.sqlite";
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    SetEnvironmentVariableW(L"GNSS_DOWNLOADER_ANTEX_DB_PATH", temp.c_str());

    std::vector<AntexSourceScan> scans;
    scans.push_back({"IGS", 1, true, false, 0, {}, L"", igs});
    scans.push_back({"CODE", 2, true, false, 0, {}, L"", code});
    scans.push_back({"EPN", 3, true, false, 0, {}, L"", epn});
    scans.push_back({"NGS", 4, true, false, 0, {}, L"", ngs});
    std::wstring error;
    ok &= Require(StoreAntexCatalog(scans, error), "SQLite catalog store");
    if (!error.empty()) std::wcerr << error << L"\n";

    const auto lookup2020 = LoadAntexAvailability(MakeQueryDate(2020, 1, 1));
    ok &= Require(lookup2020.indexed && lookup2020.complete,
        "SQLite 2020 lookup indexed");
    ok &= Require(std::ranges::any_of(lookup2020.products, [](const auto& item) {
        return item.referenceFrame == "IGS14";
    }), "2020 selects IGS14-era products");

    const auto lookup2025 = LoadAntexAvailability(MakeQueryDate(2025, 1, 1));
    ok &= Require(std::ranges::none_of(lookup2025.products, [](const auto& item) {
        return item.referenceFrame == "IGS14";
    }), "2025 excludes IGS14 products");
    ok &= Require(std::ranges::any_of(lookup2025.products, [](const auto& item) {
        return item.referenceFrame == "IGS20";
    }), "2025 selects IGS20-era products");

    const auto stats = GetAntexAvailabilityStats();
    ok &= Require(stats.complete && stats.sourcesSuccessful == 4,
        "SQLite catalog statistics");

    TestRangeUsesEffectiveAntexRevisionWindows();
    ok &= TestSingleDayAndRangeUseSameRevision();

    std::filesystem::remove(temp, ignored);
    std::filesystem::remove(temp.wstring() + L"-wal", ignored);
    std::filesystem::remove(temp.wstring() + L"-shm", ignored);
    if (ok) std::cout << "ANTEX availability tests passed.\n";
    return ok ? 0 : 1;
}
