#include "DownloadJournal.h"
#include "DownloadFileValidation.h"
#include "VmfDownloadPath.h"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
int wmain(int count, wchar_t** arguments) {
    try {
        Require(count == 2, "Pass a temporary output directory.");
        const fs::path root(arguments[1]);
        fs::create_directories(root);
        fs::path log;
        {
            DownloadJournal journal(root, L"test");
            log = journal.Path();
            Require(!log.empty(), "Log was not created.");
            std::vector<std::jthread> workers;
            for (int thread = 0; thread != 8; ++thread) workers.emplace_back([&, thread] {
                for (int file = 0; file != 10; ++file)
                    journal.Record(L"SUCCEEDED", std::to_wstring(thread * 10 + file), root / L"file.gz",
                        L"https://user:secret@example.org/data.gz?token=private", 123,
                        L"Done\nhttps://other:password@example.org/a?session=hidden");
            });
            workers.clear();
            const auto during = Read(log);
            Require(during.find("SUCCEEDED") != std::string::npos, "Running task log was not flushed.");
            journal.Record(L"ALREADY_PRESENT", L"existing", root / L"file.gz", L"", 123);
            journal.Record(L"FAILED", L"missing", {}, L"", 0, L"HTTP 404");
            journal.Finish(80, 1, 1, true, {L"HTTP 404"});
        }
        const auto contents = Read(log);
        for (const auto* secret : {"secret", "private", "password", "hidden"})
            Require(contents.find(secret) == std::string::npos, "A URL credential or token leaked into the log.");
        Require(contents.find("Succeeded: 80; already present: 1; failed: 1") != std::string::npos, "Summary counts were lost.");
        Require(contents.find("CANCELLED") != std::string::npos, "Cancelled state was lost.");
        std::size_t outcomes = 0, offset = 0;
        while ((offset = contents.find("\tSUCCEEDED\t", offset)) != std::string::npos) { ++outcomes; ++offset; }
        Require(outcomes == 80, "Concurrent records were lost or interleaved.");
        fs::path interrupted;
        { DownloadJournal journal(root, L"interrupted"); interrupted = journal.Path(); }
        Require(Read(interrupted).find("INTERRUPTED") != std::string::npos, "Interrupted task was silently reported as complete.");
        const auto fake = root / L"error.gz";
        { std::ofstream output(fake); output << "<!doctype html><html>Access denied</html>"; }
        Require(!DownloadFileLooksValid(fake), "Server page accepted as a gzip file.");
        { std::ofstream output(fake, std::ios::binary); output.write("\x1f\x8b", 2); }
        Require(!DownloadFileLooksValid(fake), "Truncated gzip header accepted.");
        const auto text = root / L"test.rnx";
        { std::ofstream output(text); output << "     3.02           OBSERVATION DATA    M: Mixed            RINEX VERSION / TYPE\n"; }
        Require(DownloadFileLooksValid(text), "RINEX text rejected.");
        const auto forecast = VmfVariantDirectory(L"VMF1_2.5x2_FC_H00");
        const auto operational = VmfVariantDirectory(L"VMF1_2.5x2_OP_H00");
        Require(forecast != operational, "VMF forecast and operational files collide.");
        Require(forecast == VmfVariantDirectory(L"VMF1_2.5x2_FC_H06"), "VMF hours are split into unrelated folders.");
        Require(forecast != VmfVariantDirectory(L"VMF1_5x5_FC_H00"), "VMF resolutions collide.");
        bool rejected = false;
        try { VmfVariantDirectory(L"../VMF1_H00"); } catch (const std::runtime_error&) { rejected = true; }
        Require(rejected, "Unsafe VMF folder accepted.");
        std::cout << "Download journal and existing-file checks passed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
