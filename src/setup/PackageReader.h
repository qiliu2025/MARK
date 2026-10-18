#pragma once
#include "InstallSpace.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace mark::setup {

struct Payload {
    std::filesystem::path file;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::array<unsigned char, 32> digest{};
    bool has_digest = false;
    SpacePlan space;
};

using Progress = std::function<void(int, const std::wstring&)>;

Payload ReadPayload(const std::filesystem::path& file, bool raw_package = false);
std::string DatabaseKey();
std::string FileDigest(const std::filesystem::path& file);
std::uint32_t DecodePayload(const Payload& payload,
                            const std::filesystem::path& output,
                            const std::atomic_bool& cancelled,
                            const Progress& progress);

}
