#pragma once

#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

// All endpoints are UTC calendar ordinals; source equipment ends are exclusive.
struct ObservationEquipmentEpoch {
    int kind = 0, first = 0, end = 0;
    std::string model, serial, firmware, radome, referencePoint;
};
struct ObservationCodeEpoch {
    int first = 0, end = 0;
    std::string code;
};
struct ObservationEquipmentPeriod {
    int firstOrdinal = 0, lastOrdinal = 0;
    std::string receiver, receiverSerial, firmware;
    std::string antenna, antennaSerial, radome, referencePoint;
    std::vector<std::string> signals;

    auto Values() const {
        return std::tie(receiver, receiverSerial, firmware, antenna,
            antennaSerial, radome, referencePoint, signals);
    }
    bool Empty() const {
        return receiver.empty() && receiverSerial.empty() && firmware.empty()
            && antenna.empty() && antennaSerial.empty() && radome.empty()
            && referencePoint.empty() && signals.empty();
    }
};

inline std::vector<ObservationEquipmentPeriod> BuildObservationEquipmentHistory(
    const std::vector<ObservationEquipmentEpoch>& equipment,
    const std::vector<ObservationCodeEpoch>& codes, int first, int last) {
    if (last < first) return {};
    std::vector<int> boundaries{first, last + 1};
    const auto add = [&](int a, int b) {
        if (b <= a || b <= first || a > last) return;
        boundaries.push_back(std::max(first, a));
        boundaries.push_back(std::min(last + 1, b));
    };
    for (const auto& item : equipment) add(item.first, item.end);
    for (const auto& item : codes) add(item.first, item.end);
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    std::vector<ObservationEquipmentPeriod> result;
    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        ObservationEquipmentPeriod period;
        period.firstOrdinal = boundaries[i];
        period.lastOrdinal = boundaries[i + 1] - 1;
        bool receiverFound = false, antennaFound = false;
        for (const auto& item : equipment) {
            if (item.first > period.firstOrdinal || item.end <= period.firstOrdinal) continue;
            if (item.kind == 0 && !receiverFound) {
                period.receiver = item.model; period.receiverSerial = item.serial;
                period.firmware = item.firmware; receiverFound = true;
            } else if (item.kind == 1 && !antennaFound) {
                period.antenna = item.model; period.antennaSerial = item.serial;
                period.radome = item.radome; period.referencePoint = item.referencePoint;
                antennaFound = true;
            }
        }
        for (const auto& item : codes)
            if (item.first <= period.firstOrdinal && period.firstOrdinal < item.end)
                period.signals.push_back(item.code);
        std::sort(period.signals.begin(), period.signals.end());
        period.signals.erase(std::unique(period.signals.begin(), period.signals.end()), period.signals.end());
        // No equipment or codes are inferred before their first recorded interval.
        if (result.empty() && period.Empty()) continue;
        if (!result.empty() && result.back().lastOrdinal + 1 == period.firstOrdinal
            && result.back().Values() == period.Values()) {
            result.back().lastOrdinal = period.lastOrdinal;
        } else {
            result.push_back(std::move(period));
        }
    }
    return result;
}
