#include "DownloadPlan.h"

#include <atomic>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;

static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template<class Predicate>
static void Await(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        Check(std::chrono::steady_clock::now() < deadline, "worker timeout");
        std::this_thread::sleep_for(5ms);
    }
}

int main() {
    try {
        DownloadPlanWorkers plan;
        std::atomic_int started{0}, stopped{0};
        auto work = [&](std::stop_token stop) {
            ++started;
            while (!stop.stop_requested()) std::this_thread::sleep_for(5ms);
            ++stopped;
        };
        for (std::uint64_t id = 1; id <= DownloadPlanWorkers::limit; ++id)
            Check(plan.Start(id, work), "could not start plan item");
        Check(!plan.Start(99, work), "plan exceeded its worker limit");
        Await([&] { return started == DownloadPlanWorkers::limit; });
        plan.Stop(2);
        Await([&] { return stopped == 1; });
        Check(plan.Contains(1) && plan.Contains(3), "item cancel affected other jobs");
        plan.Finish(2);
        Check(plan.Start(99, work), "completed slot was not reusable");
        Await([&] { return started == DownloadPlanWorkers::limit + 1; });
        plan.JoinAll();
        Check(stopped == started && plan.Size() == 0, "whole plan cancel missed a job");

        const auto path = std::filesystem::temp_directory_path() / "MARK-plan-destination";
        std::atomic_bool acquired{false}, cancelled{false};
        {
            DownloadDestinationLock first(path, {});
            Check(bool(first), "first destination lock failed");
            std::jthread waiting([&](std::stop_token stop) {
                DownloadDestinationLock second(path, stop);
                acquired = bool(second);
                cancelled = stop.stop_requested();
            });
            std::this_thread::sleep_for(50ms);
            Check(!acquired, "overlapping destination was not protected");
            waiting.request_stop();
            waiting.join();
            Check(cancelled && !acquired, "destination wait ignored cancellation");
        }
        DownloadDestinationLock released(path, {});
        Check(bool(released), "destination lock was not released");
        std::cout << "Plan concurrency, worker limit, isolated cancellation, global stop and destination lock passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
