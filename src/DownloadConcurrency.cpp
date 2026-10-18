#include "DownloadConcurrency.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <utility>

namespace {

std::wstring LowercaseWide(std::wstring_view text) {
    std::wstring result(text);
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](const wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return result;
}

bool ContainsAny(
    const std::wstring_view text,
    const std::initializer_list<std::wstring_view> values) {
    return std::any_of(
        values.begin(), values.end(),
        [&](const std::wstring_view value) {
            return text.find(value) != std::wstring_view::npos;
        });
}

struct ParsedEndpoint {
    std::wstring scheme;
    std::wstring host;
};

ParsedEndpoint ParseEndpoint(const std::wstring_view url) {
    ParsedEndpoint result;
    const std::wstring lower = LowercaseWide(url);
    const auto separator = lower.find(L"://");
    if (separator == std::wstring::npos) {
        result.scheme = lower;
        return result;
    }
    result.scheme = lower.substr(0, separator);
    const std::size_t authorityStart = separator + 3U;
    const std::size_t authorityEnd = lower.find_first_of(
        L"/?#", authorityStart);
    std::wstring authority = lower.substr(
        authorityStart,
        authorityEnd == std::wstring::npos
            ? std::wstring::npos
            : authorityEnd - authorityStart);
    if (const auto at = authority.rfind(L'@'); at != std::wstring::npos) {
        authority.erase(0, at + 1U);
    }
    if (!authority.empty() && authority.front() == L'[') {
        const auto closing = authority.find(L']');
        result.host = closing == std::wstring::npos
            ? authority
            : authority.substr(0, closing + 1U);
    } else {
        const auto port = authority.find(L':');
        result.host = authority.substr(0, port);
    }
    return result;
}

DownloadEndpointProfile MakeProfile(
    std::wstring key,
    const int initial,
    const int minimum,
    const int maximum,
    const int growAfter = 12) {
    DownloadEndpointProfile result;
    result.key = std::move(key);
    result.minimum = std::max(1, minimum);
    result.maximum = std::max(result.minimum, maximum);
    result.initial = std::clamp(initial, result.minimum, result.maximum);
    result.growAfter = std::max(4, growAfter);
    return result;
}

} // namespace

DownloadEndpointProfile ClassifyDownloadEndpoint(
    const std::wstring_view url) {
    const ParsedEndpoint endpoint = ParseEndpoint(url);
    const std::wstring lower = LowercaseWide(url);

    if (endpoint.scheme == L"geonet-jp"
        || endpoint.host == L"terras.gsi.go.jp") {
        return MakeProfile(L"terras.gsi.go.jp", 3, 1, 3, 8);
    }
    if (endpoint.scheme == L"rgna-bundle") {
        return MakeProfile(L"rgna-bundle", 1, 1, 1);
    }

    std::wstring key = endpoint.host.empty()
        ? (endpoint.scheme.empty() ? L"unknown" : endpoint.scheme)
        : endpoint.host;

    if (ContainsAny(key, {
            L"cddis.nasa.gov", L"cddis.gsfc.nasa.gov",
            L"cddis.eosdis.nasa.gov"})
        || ContainsAny(key, {L"kasi", L"nfs.kasi.re.kr"})) {
        return MakeProfile(std::move(key), 1, 1, 1);
    }

    if (key == L"gps.csn.uchile.cl") {
        return MakeProfile(std::move(key), 1, 1, 2, 10);
    }
    if (key == L"srvstorage.igac.gov.co"
        || key == L"geodesia.igac.gov.co") {
        return MakeProfile(std::move(key), 1, 1, 2, 10);
    }

    if (endpoint.scheme.find(L"sftp") != std::wstring::npos
        || ContainsAny(lower, {
            L"sftp://", L"rgna-mx-sftp", L"regna-rou-sftp",
            L"panama-sftp"})) {
        return MakeProfile(std::move(key), 1, 1, 1);
    }

    const bool isS3 = ContainsAny(key, {
        L".s3.amazonaws.com", L".s3.",
        L"noaa-cors-pds.s3.amazonaws.com",
        L"ga-gnss-data-rinex-v1.s3.amazonaws.com",
        L"geonet-open-data.s3.ap-southeast-2.amazonaws.com"});
    if (isS3) {
        return MakeProfile(std::move(key), 6, 1, 12, 10);
    }

    if (key == L"igs.bkg.bund.de") {
        return MakeProfile(std::move(key), 6, 1, 8, 10);
    }

    const bool isFtp = endpoint.scheme == L"ftp"
        || endpoint.scheme == L"ftps";
    if (isFtp) {
        if (ContainsAny(key, {L"esa.int", L"gssc.esa.int"})) {
            return MakeProfile(std::move(key), 3, 1, 6, 10);
        }
        if (ContainsAny(key, {
                L"sonel", L"trignet", L"csn", L"magna"})) {
            return MakeProfile(std::move(key), 1, 1, 2, 10);
        }
        return MakeProfile(std::move(key), 2, 1, 3, 10);
    }

    if (endpoint.scheme == L"https") {
        return MakeProfile(std::move(key), 4, 1, 8, 10);
    }
    if (endpoint.scheme == L"http") {
        return MakeProfile(std::move(key), 3, 1, 6, 10);
    }
    return MakeProfile(std::move(key), 1, 1, 2, 10);
}

DownloadConcurrencyFeedback ClassifyDownloadConcurrencyFeedback(
    const HttpDownloadResult& result) {
    if (result.success) return DownloadConcurrencyFeedback::Success;
    if (result.cancelled) return DownloadConcurrencyFeedback::Cancelled;

    const std::wstring error = LowercaseWide(result.error);
    if (ContainsAny(error, {
            L"http 429", L"http 503", L"status 429", L"status 503",
            L"rate limit", L"too many requests", L"service unavailable",
            L"服务器繁忙", L"正繁忙", L"限制并发", L"限制了并发",
            L"返回了服务器页面", L"请求过多"})) {
        return DownloadConcurrencyFeedback::RateLimited;
    }
    if (ContainsAny(error, {
            L"http 408", L"http 500", L"http 502", L"http 504",
            L"status 408", L"status 500", L"status 502", L"status 504",
            L"timed out", L"timeout", L"超时", L"超过 300 秒",
            L"超过300秒", L"错误代码 28", L"错误代码 52",
            L"错误代码 56", L"错误代码 7", L"错误代码 35",
            L"connection reset", L"connection refused",
            L"connection closed", L"could not connect",
            L"failed to connect", L"failure when receiving data",
            L"server returned nothing", L"ssl connect", L"连接被重置",
            L"无法连接", L"读取网络数据失败", L"网络传输错误"})) {
        return DownloadConcurrencyFeedback::TransientFailure;
    }
    return DownloadConcurrencyFeedback::NeutralFailure;
}

std::wstring PrimaryDownloadEndpointUrl(const DatedStationFile& item) {
    if (!item.station.remoteParts.empty()) {
        return L"rgna-bundle://official";
    }
    if (!item.station.downloadRoutes.empty()) {
        return item.station.downloadRoutes.front().url;
    }
    return item.station.downloadUrl;
}

std::vector<std::size_t> BuildAutomaticDownloadDispatchOrder(
    const std::vector<DatedStationFile>& files) {
    struct Bucket {
        std::vector<std::size_t> indices;
        std::size_t next = 0;
        int weight = 1;
    };

    std::vector<Bucket> buckets;
    std::unordered_map<std::wstring, std::size_t> bucketByKey;
    bucketByKey.reserve(32);
    for (std::size_t index = 0; index < files.size(); ++index) {
        const auto profile = ClassifyDownloadEndpoint(
            PrimaryDownloadEndpointUrl(files[index]));
        auto [iterator, inserted] = bucketByKey.emplace(
            profile.key, buckets.size());
        if (inserted) {
            Bucket bucket;
            bucket.weight = profile.initial;
            buckets.push_back(std::move(bucket));
        }
        buckets[iterator->second].indices.push_back(index);
    }

    std::vector<std::size_t> result;
    result.reserve(files.size());
    while (result.size() < files.size()) {
        bool added = false;
        // First give every host one position so the initial global worker
        // wave starts all selected archives instead of only the first one or
        // two high-weight buckets.
        for (auto& bucket : buckets) {
            if (bucket.next < bucket.indices.size()) {
                result.push_back(bucket.indices[bucket.next++]);
                added = true;
            }
        }
        for (auto& bucket : buckets) {
            for (int slot = 1;
                 slot < bucket.weight && bucket.next < bucket.indices.size();
                 ++slot) {
                result.push_back(bucket.indices[bucket.next++]);
                added = true;
            }
        }
        if (!added) break;
    }
    return result;
}

AutomaticDownloadConcurrency::Lease::Lease(
    AutomaticDownloadConcurrency* owner,
    std::wstring key,
    const std::chrono::steady_clock::time_point started) noexcept
    : owner_(owner), key_(std::move(key)), started_(started) {}

AutomaticDownloadConcurrency::Lease::Lease(Lease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      key_(std::move(other.key_)),
      started_(other.started_) {}

AutomaticDownloadConcurrency::Lease&
AutomaticDownloadConcurrency::Lease::operator=(Lease&& other) noexcept {
    if (this == &other) return *this;
    abandon();
    owner_ = std::exchange(other.owner_, nullptr);
    key_ = std::move(other.key_);
    started_ = other.started_;
    return *this;
}

AutomaticDownloadConcurrency::Lease::~Lease() {
    abandon();
}

void AutomaticDownloadConcurrency::Lease::finish(
    const HttpDownloadResult& result) noexcept {
    if (owner_ == nullptr) return;
    auto* owner = std::exchange(owner_, nullptr);
    owner->release(key_, &result, started_);
}

void AutomaticDownloadConcurrency::Lease::abandon() noexcept {
    if (owner_ == nullptr) return;
    auto* owner = std::exchange(owner_, nullptr);
    owner->release(key_, nullptr, started_);
}

AutomaticDownloadConcurrency::HostState&
AutomaticDownloadConcurrency::ensureStateLocked(
    const DownloadEndpointProfile& profile) {
    auto [iterator, inserted] = states_.try_emplace(profile.key);
    if (inserted) {
        iterator->second.profile = profile;
        iterator->second.limit = profile.initial;
        iterator->second.adaptiveCeiling = profile.maximum;
    }
    return iterator->second;
}

std::optional<AutomaticDownloadConcurrency::Lease>
AutomaticDownloadConcurrency::tryAcquire(const std::wstring_view url) {
    const auto profile = ClassifyDownloadEndpoint(url);
    std::scoped_lock lock(mutex_);
    auto& state = ensureStateLocked(profile);
    const auto now = std::chrono::steady_clock::now();
    if (now < state.cooldownUntil || state.active >= state.limit
        || activeTotal_ >= kAutomaticDownloadGlobalWorkerLimit) {
        return std::nullopt;
    }
    ++state.active;
    ++activeTotal_;
    state.peakActive = std::max(state.peakActive, state.active);
    return Lease(this, profile.key, now);
}

std::optional<AutomaticDownloadConcurrency::Lease>
AutomaticDownloadConcurrency::acquire(
    const std::wstring_view url,
    const std::stop_token stopToken,
    const DownloadControlPtr& control) {
    const auto profile = ClassifyDownloadEndpoint(url);
    while (!stopToken.stop_requested()) {
        if (control != nullptr && !control->waitIfPaused(stopToken)) {
            return std::nullopt;
        }
        std::unique_lock lock(mutex_);
        auto& state = ensureStateLocked(profile);
        const auto now = std::chrono::steady_clock::now();
        if (now >= state.cooldownUntil && state.active < state.limit
            && activeTotal_ < kAutomaticDownloadGlobalWorkerLimit) {
            ++state.active;
            ++activeTotal_;
            state.peakActive = std::max(state.peakActive, state.active);
            return Lease(this, profile.key, now);
        }
        condition_.wait_for(lock, std::chrono::milliseconds(100));
    }
    return std::nullopt;
}

std::optional<AutomaticDownloadConcurrency::AcquiredLease>
AutomaticDownloadConcurrency::acquireAny(
    const std::span<const std::wstring_view> urls,
    const std::stop_token stopToken,
    const DownloadControlPtr& control) {
    if (urls.empty()) return std::nullopt;
    while (!stopToken.stop_requested()) {
        if (control != nullptr && !control->waitIfPaused(stopToken)) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < urls.size(); ++index) {
            auto lease = tryAcquire(urls[index]);
            if (lease.has_value()) {
                AcquiredLease result;
                result.candidateIndex = index;
                result.lease = std::move(*lease);
                return result;
            }
        }
        std::unique_lock lock(mutex_);
        condition_.wait_for(lock, std::chrono::milliseconds(100));
    }
    return std::nullopt;
}

void AutomaticDownloadConcurrency::release(
    const std::wstring& key,
    const HttpDownloadResult* result,
    const std::chrono::steady_clock::time_point started) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        const auto iterator = states_.find(key);
        if (iterator == states_.end()) return;
        auto& state = iterator->second;
        state.active = std::max(0, state.active - 1);
        if (activeTotal_ > 0) --activeTotal_;
        if (result == nullptr) {
            condition_.notify_all();
            return;
        }

        const auto feedback = ClassifyDownloadConcurrencyFeedback(*result);
        if (feedback == DownloadConcurrencyFeedback::Success) {
            state.pressureFailures = 0;
            const double seconds = std::max(
                0.001,
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count());
            if (result->bytesWritten > 0) {
                ++state.sampleSuccesses;
                state.sampleBytes += result->bytesWritten;
                state.sampleSeconds += seconds;
            }
            if (state.sampleSuccesses >= state.profile.growAfter
                && state.sampleSeconds > 0.0) {
                const int observedConcurrency = std::max(
                    1, std::min(state.limit, state.peakActive));
                const double aggregateBytesPerSecond =
                    static_cast<double>(state.sampleBytes)
                    / state.sampleSeconds
                    * static_cast<double>(observedConcurrency);
                if (state.evaluatingHigherLimit
                    && state.acceptedAggregateBytesPerSecond > 0.0
                    && aggregateBytesPerSecond
                        < state.acceptedAggregateBytesPerSecond * 1.05) {
                    state.limit = std::max(
                        state.profile.minimum, state.limit - 1);
                    state.adaptiveCeiling = state.limit;
                    state.evaluatingHigherLimit = false;
                } else {
                    state.acceptedAggregateBytesPerSecond =
                        aggregateBytesPerSecond;
                    if (state.limit < state.adaptiveCeiling) {
                        ++state.limit;
                        state.evaluatingHigherLimit = true;
                    } else {
                        state.evaluatingHigherLimit = false;
                    }
                }
                state.sampleSuccesses = 0;
                state.sampleBytes = 0;
                state.sampleSeconds = 0.0;
                state.peakActive = state.active;
            }
        } else if (feedback == DownloadConcurrencyFeedback::RateLimited) {
            state.limit = std::max(
                state.profile.minimum, (state.limit + 1) / 2);
            state.cooldownUntil = std::chrono::steady_clock::now()
                + std::chrono::seconds(30);
            state.pressureFailures = 0;
            state.adaptiveCeiling = state.profile.maximum;
            state.evaluatingHigherLimit = false;
            state.acceptedAggregateBytesPerSecond = 0.0;
            state.sampleSuccesses = 0;
            state.sampleBytes = 0;
            state.sampleSeconds = 0.0;
        } else if (feedback
                   == DownloadConcurrencyFeedback::TransientFailure) {
            ++state.pressureFailures;
            if (state.pressureFailures >= 2) {
                state.limit = std::max(
                    state.profile.minimum, (state.limit + 1) / 2);
                state.cooldownUntil = std::chrono::steady_clock::now()
                    + std::chrono::seconds(10);
                state.pressureFailures = 0;
                state.adaptiveCeiling = state.profile.maximum;
                state.evaluatingHigherLimit = false;
                state.acceptedAggregateBytesPerSecond = 0.0;
                state.sampleSuccesses = 0;
                state.sampleBytes = 0;
                state.sampleSeconds = 0.0;
            }
        } else if (feedback
                   == DownloadConcurrencyFeedback::NeutralFailure) {
            state.pressureFailures = 0;
        }
        condition_.notify_all();
    } catch (...) {
        // Concurrency feedback must never turn a completed transfer into an
        // application failure.  The next batch will simply relearn the host.
    }
}

DownloadConcurrencySnapshot AutomaticDownloadConcurrency::snapshot(
    const std::wstring_view url) const {
    const auto profile = ClassifyDownloadEndpoint(url);
    std::scoped_lock lock(mutex_);
    DownloadConcurrencySnapshot result;
    result.minimum = profile.minimum;
    result.maximum = profile.maximum;
    const auto iterator = states_.find(profile.key);
    if (iterator == states_.end()) {
        result.limit = profile.initial;
        return result;
    }
    result.active = iterator->second.active;
    result.limit = iterator->second.limit;
    result.coolingDown = std::chrono::steady_clock::now()
        < iterator->second.cooldownUntil;
    return result;
}

AutomaticDownloadConcurrency& SharedAutomaticDownloadConcurrency() noexcept {
    static AutomaticDownloadConcurrency concurrency;
    return concurrency;
}
