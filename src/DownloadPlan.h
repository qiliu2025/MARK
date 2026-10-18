#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

// Owned by the UI thread. Each plan item keeps its own cancellation token;
// transfers inside these jobs still share the global server concurrency gate.
class DownloadPlanWorkers {
public:
    static constexpr std::size_t limit = 12;
    ~DownloadPlanWorkers() { StopAll(); }

    template<class Work>
    bool Start(std::uint64_t id, Work&& work) {
        if (id == 0 || Contains(id) || Size() >= limit) return false;
        jobs_.try_emplace(id, std::forward<Work>(work));
        return true;
    }
    void Finish(std::uint64_t id) { jobs_.erase(id); }
    void Stop(std::uint64_t id) {
        if (const auto found = jobs_.find(id); found != jobs_.end())
            found->second.request_stop();
    }
    void StopAll() {
        for (auto& [id, job] : jobs_) job.request_stop();
    }
    void JoinAll() { StopAll(); jobs_.clear(); }
    bool Contains(std::uint64_t id) const { return jobs_.contains(id); }
    std::size_t Size() const { return jobs_.size(); }
    std::uint64_t First() const { return jobs_.empty() ? 0 : jobs_.begin()->first; }

private:
    std::map<std::uint64_t, std::jthread> jobs_;
};

// Serialize overlapping destinations across plan items and both download views.
// Never hold a server lease while waiting for this lock.
class DownloadDestinationLock {
public:
    DownloadDestinationLock(const std::filesystem::path& path,
                            std::stop_token stop) {
        auto key = std::filesystem::absolute(path).lexically_normal().wstring();
        for (auto& ch : key) ch = static_cast<wchar_t>(std::towlower(ch));
        {
            std::lock_guard guard(registryMutex_);
            std::erase_if(registry_, [](const auto& entry) { return entry.second.expired(); });
            auto& entry = registry_[key];
            mutex_ = entry.lock();
            if (!mutex_) { mutex_ = std::make_shared<std::mutex>(); entry = mutex_; }
        }
        lock_ = std::unique_lock<std::mutex>(*mutex_, std::defer_lock);
        while (!stop.stop_requested()) {
            if (lock_.try_lock()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    explicit operator bool() const { return lock_.owns_lock(); }

private:
    inline static std::mutex registryMutex_;
    inline static std::map<std::wstring, std::weak_ptr<std::mutex>> registry_;
    std::shared_ptr<std::mutex> mutex_;
    std::unique_lock<std::mutex> lock_;
};
