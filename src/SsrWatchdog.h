#pragma once

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

struct SsrWatchdogOptions {
    std::filesystem::path directory;
    int maximumCandidateNodes = 5;
    bool googleOnly = false;
    std::vector<int> preferredCandidateNodes;
};

class SsrWatchdog {
public:
    using LogCallback = std::function<void(const std::wstring&)>;

    explicit SsrWatchdog(SsrWatchdogOptions options);

    [[nodiscard]] bool available() const;
    [[nodiscard]] const std::filesystem::path& executablePath() const;
    [[nodiscard]] int localPort() const;

    // Makes curl-based archive readers use the local SSR SOCKS proxy.  This
    // intentionally affects only this process and its curl children.
    bool ConfigureProcessProxy(std::wstring& error);

    // Probe the currently selected node without restarting or changing it.
    // The probe uses the same anonymous IGS/MGEX endpoints as the high-rate
    // scanner, so a missing source directory is not mistaken for a dead node.
    bool ProbeCurrentNode(
        std::stop_token stopToken,
        const LogCallback& log = {}) const;

    // Restarts the current node, then tries a bounded set of other nodes.  If
    // no node passes two end-to-end archive probe rounds, scanning remains
    // paused and the recovery cycle repeats with bounded backoff until the
    // user stops the builder or connectivity returns.
    bool RecoverUntilHealthy(
        std::stop_token stopToken,
        const LogCallback& log,
        bool skipCurrentNode = false);

    static std::filesystem::path DiscoverDirectory();

public:
    struct ConfigInfo {
        int index = -1;
        int localPort = 1080;
        int nodeCount = 0;
        std::string text;
        std::vector<std::string> nodeNames;
    };

private:

    bool ReadConfig(ConfigInfo& config, std::wstring& error) const;
    bool WriteIndex(
        const ConfigInfo& config,
        int index,
        std::wstring& error) const;
    bool StopExactProcess(std::wstring& error) const;
    bool StartProcess(std::wstring& error) const;
    bool WaitForLocalPort(std::stop_token stopToken) const;
    bool ProbeRound(std::stop_token stopToken) const;
    bool RestartAndProbe(
        int index,
        bool changeIndex,
        std::stop_token stopToken,
        const LogCallback& log);
    void AppendRecoveryLog(const std::wstring& message) const;

    SsrWatchdogOptions options_;
    std::filesystem::path executablePath_;
    std::filesystem::path configPath_;
    mutable int localPort_ = 1080;
};
