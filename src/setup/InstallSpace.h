#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace mark::setup {
struct SpacePlan {
    std::uint64_t unpacked = 0, installed = 0;
    std::array<std::uint64_t, 2> conversionExtra{};
};
struct SpaceBudget {
    std::uint64_t peak = 0, margin = 0, required = 0;
};
inline std::uint64_t AddBytes(std::uint64_t first, std::uint64_t second) {
    if (second > std::numeric_limits<std::uint64_t>::max() - first)
        throw std::runtime_error("The package size information is invalid.");
    return first + second;
}
inline SpaceBudget CalculateSpace(const SpacePlan& plan, std::uint64_t archive,
    std::uint64_t tools, unsigned workers) {
    if (!plan.unpacked || !plan.installed || !plan.conversionExtra[0]
        || plan.conversionExtra[1] < plan.conversionExtra[0] || workers < 1 || workers > 2)
        throw std::runtime_error("This installer has no valid disk-space information. Download the complete installer again.");
    SpaceBudget result;
    result.peak = AddBytes(tools, std::max({AddBytes(archive, plan.unpacked),
        AddBytes(plan.unpacked, plan.conversionExtra[workers - 1]), plan.installed}));
    // File allocation and database page layout can differ between volumes.
    result.margin = std::max<std::uint64_t>(256ULL * 1024 * 1024,
        AddBytes(result.peak / 100 * 3, (result.peak % 100 * 3 + 99) / 100));
    result.required = AddBytes(result.peak, result.margin);
    return result;
}
inline unsigned ChoosePreparationWorkers(const SpacePlan& plan, std::uint64_t archive,
    std::uint64_t tools, std::uint64_t available, bool parallelAllowed) {
    if (parallelAllowed && available >= CalculateSpace(plan, archive, tools, 2).required) return 2;
    return 1;
}
}
