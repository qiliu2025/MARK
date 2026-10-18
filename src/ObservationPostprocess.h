#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>
#include <cstdint>

namespace ObservationPostprocess {
struct VolumeSpace {
    std::filesystem::path volume;
    std::uint64_t required = 0;
    std::uint64_t available = 0;
};
struct Estimate {
    std::vector<std::filesystem::path> files;
    std::vector<VolumeSpace> volumes;
    std::wstring error;
    bool sufficient = true;
};
struct Result {
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    bool needsSpace = false;
    std::vector<std::filesystem::path> remaining;
    std::vector<std::wstring> errors;
};
using Progress = std::function<void(std::size_t, std::size_t, const std::wstring&)>;
// Tool executables are shipped unchanged in resources/download-tools.
std::filesystem::path ToolsDirectory();
bool IsProcessable(const std::filesystem::path& file);
std::filesystem::path StandardRinexPath(std::filesystem::path file);
Estimate Inspect(const std::vector<std::filesystem::path>& files,
                 const std::filesystem::path& tools);
Result Run(const Estimate& estimate, const std::filesystem::path& tools,
           std::stop_token stop, const Progress& progress = {}, unsigned workerLimit = 0);
}
