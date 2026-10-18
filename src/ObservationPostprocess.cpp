#include "ObservationPostprocess.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <cwctype>
#include <limits>
#include <atomic>
#include <mutex>

namespace ObservationPostprocess {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t MiB = 1024 * 1024;
constexpr std::uint64_t Reserve = 256 * MiB;
struct Cancelled {};
struct SpaceNeeded {};
std::wstring NativePath(const fs::path& path) {
    auto value = fs::absolute(path).lexically_normal().wstring();
    if (value.starts_with(L"\\\\?\\")) return value;
    return value.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + value.substr(2) : L"\\\\?\\" + value;
}
DWORD Attributes(const fs::path& path) { return GetFileAttributesW(NativePath(path).c_str()); }
fs::path Long(const fs::path& path) { return fs::path(NativePath(path)); }
fs::path DisplayPath(const fs::path& path) {
    auto value = path.wstring();
    if (value.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + value.substr(8);
    if (value.starts_with(L"\\\\?\\")) return value.substr(4);
    return path;
}
void StageInput(const fs::path& source, const fs::path& target) {
    // Staging and input are on the same volume. A read-only hard link avoids
    // rewriting the archive/Compact RINEX while retaining the original name.
    if (!CreateHardLinkW(NativePath(target).c_str(), NativePath(source).c_str(), nullptr))
        fs::copy_file(Long(source), Long(target));
}
std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
bool Compact(const fs::path& file) {
    const auto ext = Lower(file.extension().wstring());
    return ext == L".crx" || ext == L".d"
        || (ext.size() == 4 && iswdigit(ext[1]) && iswdigit(ext[2]) && ext[3] == L'd');
}
bool Archive(const fs::path& file) {
    const auto ext = Lower(file.extension().wstring());
    return ext == L".gz" || ext == L".z" || ext == L".zip" || ext == L".tar"
        || ext == L".tgz" || ext == L".bz2" || ext == L".xz";
}
std::wstring FromUtf8(const std::string& value) {
    const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (!n) {
        const auto size = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        std::wstring fallback(size, 0);
        MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), fallback.data(), size);
        return fallback;
    }
    std::wstring result(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), n);
    return result;
}
std::wstring Quote(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
std::uint64_t FreeSpace(const fs::path& folder) {
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(NativePath(folder).c_str(), &available, nullptr, nullptr))
        throw std::runtime_error("Cannot read available disk space");
    return available.QuadPart;
}
fs::path Volume(const fs::path& folder) {
    std::array<wchar_t, 32768> volume{};
    if (!GetVolumePathNameW(folder.c_str(), volume.data(), static_cast<DWORD>(volume.size())))
        throw std::runtime_error("Cannot locate download volume");
    return volume.data();
}
void CheckStop(const std::stop_token stop) { if (stop.stop_requested()) throw Cancelled{}; }
std::uint64_t Multiply(const std::uint64_t n, const std::uint64_t factor) {
    if (n > std::numeric_limits<std::uint64_t>::max() / factor)
        throw std::runtime_error("Archive size too large");
    return n * factor;
}
std::uint64_t UnpackedHint(const fs::path& file) {
    const auto size = fs::file_size(Long(file));
    const auto ext = Lower(file.extension().wstring());
    if ((ext == L".gz" || ext == L".tgz") && size >= 18) {
        std::ifstream stream(Long(file), std::ios::binary);
        std::array<unsigned char, 2> magic{};
        stream.read(reinterpret_cast<char*>(magic.data()), magic.size());
        if (magic[0] != 0x1f || magic[1] != 0x8b) return Multiply(size, 16);
        stream.seekg(-4, std::ios::end);
        std::array<unsigned char, 4> bytes{};
        stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        const std::uint64_t hint = bytes[0] | (std::uint64_t(bytes[1]) << 8)
            | (std::uint64_t(bytes[2]) << 16) | (std::uint64_t(bytes[3]) << 24);
        // GZIP ISIZE wraps at 4 GiB; retain a compressed-size bound as well.
        return std::max(hint, Multiply(size, size >= 512 * MiB || hint < size ? 16 : 1));
    }
    if (ext == L".zip" && size >= 22) {
        // Read the central directory only; do not spawn a process per daily file.
        std::ifstream stream(Long(file), std::ios::binary);
        const auto tailSize = static_cast<std::size_t>(std::min<std::uint64_t>(size, 65557));
        std::vector<unsigned char> tail(tailSize);
        stream.seekg(static_cast<std::streamoff>(size - tailSize));
        stream.read(reinterpret_cast<char*>(tail.data()), tailSize);
        const auto word = [](const unsigned char* p) -> std::uint64_t {
            return p[0] | (std::uint64_t(p[1]) << 8);
        };
        const auto dword = [&](const unsigned char* p) -> std::uint64_t {
            return word(p) | (word(p + 2) << 16);
        };
        for (std::size_t offset = tail.size() - 22; ; --offset) {
            const auto* end = tail.data() + offset;
            if (dword(end) == 0x06054b50 && offset + 22 + word(end + 20) == tail.size()
                && word(end + 4) == 0 && word(end + 6) == 0) {
                const auto entries = word(end + 10), start = dword(end + 16);
                if (entries == 0xffff || start == 0xffffffff) break; // ZIP64: conservative fallback.
                stream.seekg(static_cast<std::streamoff>(start));
                std::uint64_t unpacked = 0;
                bool valid = true;
                for (std::uint64_t i = 0; i < entries; ++i) {
                    std::array<unsigned char, 46> header{};
                    stream.read(reinterpret_cast<char*>(header.data()), header.size());
                    if (!stream || dword(header.data()) != 0x02014b50 || dword(header.data() + 24) == 0xffffffff) {
                        valid = false; break;
                    }
                    unpacked += dword(header.data() + 24);
                    stream.seekg(static_cast<std::streamoff>(word(header.data() + 28)
                        + word(header.data() + 30) + word(header.data() + 32)), std::ios::cur);
                }
                if (valid) return std::max(unpacked, size);
                break;
            }
            if (offset == 0) break;
        }
    }
    if (ext == L".tar") return size;
    return Multiply(size, Archive(file) ? 16 : 1);
}
std::uint64_t Required(const fs::path& file) {
    // A conservative working-space allowance, not a claimed exact RINEX size.
    // Includes retained source, staging and expanded observations.
    return Multiply(UnpackedHint(file), 6);
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
};
struct CommandResult { DWORD code = 1; std::string output; };
CommandResult Command(const fs::path& exe, const std::vector<std::wstring>& args,
    const fs::path& cwd, const std::stop_token stop, const fs::path& outputFile = {}) {
    CheckStop(stop);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle read, write, output, input;
    if (!CreatePipe(&read.value, &write.value, &security, 0)
        || !SetHandleInformation(read.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("Cannot create tool output pipe");
    if (!outputFile.empty()) {
        output.value = CreateFileW(NativePath(outputFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ,
            &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output.value == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot create output file");
    }
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = outputFile.empty() ? write.value : output.value;
    startup.StartupInfo.hStdError = write.value;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<unsigned char> attributes(attributeBytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes))
        throw std::runtime_error("Cannot initialize tool handles");
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(value); }
    } cleanup{startup.lpAttributeList};
    std::vector<HANDLE> inherited{input.value, write.value};
    if (!outputFile.empty()) inherited.push_back(output.value);
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
            inherited.size() * sizeof(HANDLE), nullptr, nullptr))
        throw std::runtime_error("Cannot restrict tool handles");
    auto command = Quote(exe.wstring());
    for (const auto& arg : args) command += L" " + Quote(arg);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, NativePath(cwd).c_str(),
            &startup.StartupInfo, &process))
        throw std::runtime_error("Cannot launch extraction/conversion tool");
    Handle child{process.hProcess}, thread{process.hThread};
    struct ChildCleanup {
        HANDLE process;
        ~ChildCleanup() {
            if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1); WaitForSingleObject(process, INFINITE);
            }
        }
    } childCleanup{child.value};
    CloseHandle(write.value); write.value = INVALID_HANDLE_VALUE;
    CommandResult result;
    bool cancelled = false, noSpace = false, excessiveOutput = false;
    const auto drain = [&] {
        DWORD available = 0;
        std::array<char, 8192> buffer{};
        while (PeekNamedPipe(read.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD received = 0;
            if (!ReadFile(read.value, buffer.data(), std::min<DWORD>(available,
                    static_cast<DWORD>(buffer.size())), &received, nullptr) || !received) break;
            if (result.output.size() + received <= 16 * MiB) result.output.append(buffer.data(), received);
            else excessiveOutput = true;
        }
    };
    ULONGLONG spaceCheck = 0;
    for (;;) {
        drain();
        const auto wait = WaitForSingleObject(child.value, 50);
        if (wait == WAIT_OBJECT_0) break;
        cancelled = stop.stop_requested();
        if (GetTickCount64() - spaceCheck > 300) {
            spaceCheck = GetTickCount64();
            noSpace = FreeSpace(cwd) < 64 * MiB;
        }
        if (cancelled || noSpace || excessiveOutput || wait == WAIT_FAILED) {
            TerminateProcess(child.value, 1);
            WaitForSingleObject(child.value, INFINITE);
            break;
        }
    }
    drain();
    GetExitCodeProcess(child.value, &result.code);
    if (cancelled) throw Cancelled{};
    if (noSpace) throw SpaceNeeded{};
    if (excessiveOutput) throw std::runtime_error("Archive listing exceeds safety limit");
    return result;
}
bool RinexObservation(const fs::path& file) {
    std::ifstream stream(Long(file), std::ios::binary);
    std::string line;
    if (!std::getline(stream, line) || line.size() < 60
        || line.find("RINEX VERSION / TYPE") == std::string::npos) return false;
    return line[20] == 'O' || line.substr(20, 20).find("OBSERVATION") != std::string::npos;
}
bool Identical(const fs::path& a, const fs::path& b) {
    if (fs::file_size(Long(a)) != fs::file_size(Long(b))) return false;
    std::ifstream left(Long(a), std::ios::binary), right(Long(b), std::ios::binary);
    std::array<char, 65536> x{}, y{};
    while (left && right) {
        left.read(x.data(), x.size()); right.read(y.data(), y.size());
        if (left.gcount() != right.gcount()
            || !std::equal(x.begin(), x.begin() + left.gcount(), y.begin())) return false;
    }
    return !left.bad() && !right.bad();
}
void SafeMember(const std::wstring& name) {
    const fs::path path(name);
    if (name.empty() || path.is_absolute() || path.has_root_name()
        || name.find(L':') != std::wstring::npos || name.front() == L'/' || name.front() == L'\\')
        throw std::runtime_error("Unsafe archive member path");
    for (const auto& part : path) {
        const auto text = part.wstring();
        if (text == L".") continue;
        if (text == L".." || (!text.empty() && (text.back() == L'.' || text.back() == L' ')))
            throw std::runtime_error("Unsafe archive member path");
        auto base = Lower(part.stem().wstring());
        if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul"
            || (base.size() == 4 && (base.starts_with(L"com") || base.starts_with(L"lpt"))
                && iswdigit(base[3]))) throw std::runtime_error("Reserved archive member path");
    }
}
std::vector<fs::path> StageFiles(const fs::path& stage) {
    std::vector<fs::path> files;
    for (const auto& item : fs::recursive_directory_iterator(Long(stage))) {
        const auto attributes = Attributes(item.path());
        if (attributes == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("Cannot inspect extracted file attributes");
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Archive contains a link");
        if (item.is_regular_file()) files.push_back(DisplayPath(item.path()));
        else if (!item.is_directory()) throw std::runtime_error("Unsupported archive member");
    }
    return files;
}
struct Stage {
    fs::path folder, parent;
    ~Stage() {
        // Only remove the unique task-owned directory directly under its verified parent.
        if (folder.parent_path() == parent && folder.filename().wstring().starts_with(L".mark-unpack-")) {
            std::error_code error; fs::remove_all(Long(folder), error);
        }
    }
};
void Extract(const fs::path& file, const fs::path& tools, const std::stop_token stop) {
    const auto ext = Lower(file.extension().wstring());
    if (ext == L".zip" || ext == L".tar") {
        const auto listing = Command(tools / L"7z.exe",
            {L"l", L"-slt", L"-ba", L"-sccUTF-8", L"--", file.wstring()}, file.parent_path(), stop);
        if (listing.code != 0) throw std::runtime_error("Archive listing failed");
        std::istringstream lines(listing.output);
        std::string line;
        std::uint64_t bytes = 0;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("Path = ")) SafeMember(FromUtf8(line.substr(7)));
            if ((line.starts_with("Symbolic Link = ") && line.size() > 16)
                || (line.starts_with("Hard Link = ") && line.size() > 12)
                || line.starts_with("Encrypted = +") || line.starts_with("Alternate Stream = +")
                || line.starts_with("Mode = l") || line.find("SymbolicLink") != std::string::npos)
                throw std::runtime_error("Archive contains links, encrypted files or alternate streams");
            if (line.starts_with("Size = ")) {
                const auto value = std::stoull(line.substr(7));
                if (value > std::numeric_limits<std::uint64_t>::max() - bytes)
                    throw std::runtime_error("Archive size overflow");
                bytes += value;
            }
        }
        if (bytes > FreeSpace(file.parent_path()) || FreeSpace(file.parent_path()) - bytes < Reserve)
            throw SpaceNeeded{};
        const auto isolated = file.parent_path() / L".archive-contents";
        if (!fs::create_directory(Long(isolated))) throw std::runtime_error("Conflicting archive staging directory");
        const auto extracted = Command(tools / L"7z.exe",
            {L"x", L"-y", L"-aos", L"-sccUTF-8", L"-bso0", L"-bsp0",
             L"-o" + isolated.wstring(), L"--", file.wstring()}, file.parent_path(), stop);
        if (extracted.code != 0) throw std::runtime_error("Archive extraction failed");
        for (const auto& member : StageFiles(isolated)) {
            CheckStop(stop);
            const auto target = file.parent_path() / member.lexically_relative(isolated);
            if (fs::exists(Long(target))) throw std::runtime_error("Archive member conflicts with another file");
            fs::create_directories(Long(target.parent_path())); fs::rename(Long(member), Long(target));
        }
        fs::remove_all(Long(isolated));
    } else {
        auto output = file; output.replace_extension(ext == L".tgz" ? L".tar" : L"");
        const auto extracted = Command(tools / L"7z.exe",
            {L"x", L"-so", L"-bso0", L"-bsp0", L"--", file.wstring()}, file.parent_path(), stop, output);
        if (extracted.code != 0 || fs::file_size(Long(output)) == 0)
            throw std::runtime_error("Compressed observation extraction failed");
    }
    CheckStop(stop);
    fs::remove(Long(file)); // This is a staging copy, never the user's archive.
}
void Convert(const fs::path& file, const fs::path& stage, const fs::path& tools,
             const std::stop_token stop) {
    std::ifstream stream(Long(file), std::ios::binary);
    std::string line; std::getline(stream, line);
    stream.close();
    if (line.size() < 60 || line.substr(60).find("CRINEX VERS") == std::string::npos)
        throw std::runtime_error("Compact RINEX header missing");
    const auto scratch = stage / L".converter";
    fs::create_directory(Long(scratch));
    const auto input = scratch / (L"input" + (Lower(file.extension().wstring()) == L".d"
        ? std::wstring(L".24d") : file.extension().wstring()));
    StageInput(file, input);
    const auto converted = scratch / L"output.rnx";
    const auto result = Command(tools / L"CRX2RNX.exe",
        {input.filename().wstring(), L"-"}, scratch, stop, converted);
    // Do not salvage partial files: a warning exit may omit corrupt epochs.
    if (result.code != 0 || !RinexObservation(converted))
        throw std::runtime_error("CRX2RNX failed or output is not a complete RINEX observation file");
    CheckStop(stop);
    const auto target = StandardRinexPath(file);
    if (fs::exists(Long(target))) {
        if (!Identical(converted, target)) throw std::runtime_error("Conflicting RINEX file inside archive");
    } else fs::rename(Long(converted), Long(target));
    fs::remove(Long(file));
    fs::remove_all(Long(scratch));
}
void Process(const fs::path& original, const fs::path& tools, const std::stop_token stop,
             std::mutex& commitMutex) {
    CheckStop(stop);
    const auto parent = fs::absolute(original.parent_path()).lexically_normal();
    Stage stage{{}, parent};
    for (unsigned n = 0; n < 100; ++n) {
        stage.folder = parent / (L".mark-unpack-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(n));
        if (fs::create_directory(Long(stage.folder))) break;
        stage.folder.clear();
    }
    if (stage.folder.empty()) throw std::runtime_error("Cannot create staging directory");
    const auto sourceSize = fs::file_size(Long(original));
    const auto sourceTime = fs::last_write_time(Long(original));
    StageInput(original, stage.folder / original.filename());
    for (unsigned depth = 0; ; ++depth) {
        const auto files = StageFiles(stage.folder);
        bool found = false;
        for (const auto& file : files) {
            if (!Archive(file)) continue;
            if (depth >= 8) throw std::runtime_error("Too many nested archive layers");
            CheckStop(stop); Extract(file, tools, stop); found = true;
        }
        if (!found) break;
    }
    for (const auto& file : StageFiles(stage.folder)) {
        if (Compact(file)) Convert(file, stage.folder, tools, stop);
    }
    const auto outputs = StageFiles(stage.folder);
    if (outputs.empty()) throw std::runtime_error("Archive has no files");
    // Independent inputs can yield the same output name. Serialize preflight
    // and commit so no job can overwrite another job's completed result.
    std::scoped_lock commitLock(commitMutex);
    std::set<std::wstring> destinations;
    for (const auto& file : outputs) {
        const auto relative = file.lexically_relative(stage.folder);
        SafeMember(relative.wstring());
        const auto target = parent / relative;
        if (!destinations.insert(Lower(target.wstring())).second)
            throw std::runtime_error("Duplicate archive output path");
        // Every existing parent must be a directory without a junction/symlink.
        for (auto folder = target.parent_path(); folder != parent; folder = folder.parent_path()) {
            if (folder.empty()) throw std::runtime_error("Output escaped destination");
            if (fs::exists(Long(folder)) && (!fs::is_directory(Long(folder))
                || (Attributes(folder) & FILE_ATTRIBUTE_REPARSE_POINT)))
                throw std::runtime_error("Unsafe destination directory");
        }
        if (fs::exists(Long(target)) && (!fs::is_regular_file(Long(target))
            || (Attributes(target) & FILE_ATTRIBUTE_REPARSE_POINT)
            || !Identical(file, target))) throw std::runtime_error("Existing output differs; no files overwritten");
    }
    std::vector<fs::path> created;
    try {
        for (const auto& file : outputs) {
            CheckStop(stop);
            const auto target = parent / file.lexically_relative(stage.folder);
            if (fs::exists(Long(target))) continue; // Byte-identical output already checked.
            fs::create_directories(Long(target.parent_path()));
            // MoveFileW fails if another process created the destination in the meantime.
            if (!MoveFileW(NativePath(file).c_str(), NativePath(target).c_str())) throw std::runtime_error("Cannot commit output without overwrite");
            created.push_back(target);
        }
        CheckStop(stop);
        if (fs::file_size(Long(original)) != sourceSize || fs::last_write_time(Long(original)) != sourceTime)
            throw std::runtime_error("Source changed during postprocessing");
        if (!fs::remove(Long(original))) throw std::runtime_error("Cannot remove successfully processed source");
    } catch (...) {
        for (const auto& file : created) { std::error_code error; fs::remove(Long(file), error); }
        throw;
    }
}
}

std::filesystem::path ToolsDirectory() {
    std::array<wchar_t, 32768> exe{};
    GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    return fs::path(exe.data()).parent_path() / L"resources" / L"download-tools";
}
bool IsProcessable(const fs::path& file) { return Archive(file) || Compact(file); }
fs::path StandardRinexPath(fs::path file) {
    while (Archive(file)) {
        const auto ext = Lower(file.extension().wstring());
        if (ext == L".zip" || ext == L".tar" || ext == L".tgz") return {};
        file.replace_extension();
    }
    if (Compact(file)) {
        auto ext = file.extension().wstring();
        if (Lower(ext) == L".crx") file.replace_extension(L".rnx");
        else { ext.back() = iswupper(ext.back()) ? L'O' : L'o'; file.replace_extension(ext); }
    }
    return file;
}
Estimate Inspect(const std::vector<fs::path>& files, const fs::path& tools) {
    Estimate estimate;
    try {
        std::set<std::wstring> seen;
        std::map<std::wstring, VolumeSpace> volumes;
        std::map<fs::path, fs::path> folderVolumes;
        bool needConverter = false;
        for (const auto& file : files) {
            if (!IsProcessable(file) || !fs::exists(Long(file))) continue;
            const auto path = fs::absolute(file).lexically_normal();
            if (!seen.insert(Lower(path.wstring())).second) continue;
            if (!fs::is_regular_file(Long(path)) || (Attributes(path) & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("Input is not a regular observation file");
            estimate.files.push_back(path);
            // An archive may contain Compact RINEX regardless of its outer name.
            needConverter = needConverter || Compact(path) || Archive(path);
            auto [folder, inserted] = folderVolumes.try_emplace(path.parent_path());
            if (inserted) folder->second = Volume(path.parent_path());
            const auto& volume = folder->second;
            auto& item = volumes[Lower(volume.wstring())];
            if (item.volume.empty()) item.available = FreeSpace(path.parent_path());
            item.volume = volume;
            const auto required = Required(path);
            if (required > std::numeric_limits<std::uint64_t>::max() - item.required)
                throw std::runtime_error("Total required space overflow");
            item.required += required;
        }
        if (!estimate.files.empty() && (!fs::exists(tools / L"7z.exe") || !fs::exists(tools / L"7z.dll")))
            throw std::runtime_error("Bundled 7z.exe missing");
        if (needConverter && !fs::exists(tools / L"CRX2RNX.exe"))
            throw std::runtime_error("Bundled CRX2RNX.exe missing");
        for (auto& [key, volume] : volumes) {
            if (volume.required > std::numeric_limits<std::uint64_t>::max() - Reserve)
                throw std::runtime_error("Required space overflow");
            volume.required += Reserve;
            estimate.sufficient = estimate.sufficient && volume.available >= volume.required;
            estimate.volumes.push_back(volume);
        }
    } catch (const std::exception& error) {
        estimate.error = L"无法准备解压与 RINEX 转换：" + FromUtf8(error.what());
        estimate.sufficient = false;
    }
    return estimate;
}
Result Run(const Estimate& estimate, const fs::path& tools,
           const std::stop_token stop, const Progress& progress, unsigned workerLimit) {
    Result result;
    if (!estimate.error.empty()) { result.errors.push_back(estimate.error); result.failed = estimate.files.size(); return result; }
    const auto fresh = Inspect(estimate.files, tools);
    if (!fresh.error.empty()) { result.errors.push_back(fresh.error); result.failed = estimate.files.size(); return result; }
    if (!fresh.sufficient) { result.needsSpace = true; result.remaining = fresh.files; return result; }
    if (fresh.files.empty()) return result;
    if (!workerLimit) workerLimit = std::max(1u, std::thread::hardware_concurrency() / 2);
    workerLimit = static_cast<unsigned>(std::min<std::size_t>(std::clamp(workerLimit, 1u, 4u), fresh.files.size()));
    std::stop_source batchStop;
    std::stop_callback userCancellation(stop, [&] { batchStop.request_stop(); });
    std::atomic<std::size_t> next{0};
    std::atomic<bool> needsSpace{false};
    std::vector<unsigned char> outcomes(fresh.files.size(), 0);
    std::mutex resultMutex, commitMutex;
    std::size_t completed = 0;
    const auto noSpace = [&] { needsSpace = true; batchStop.request_stop(); };
    const auto worker = [&] {
        while (!batchStop.stop_requested()) {
            const auto index = next.fetch_add(1);
            if (index >= fresh.files.size()) break;
            const auto& file = fresh.files[index];
            try {
                CheckStop(batchStop.get_token());
                {
                    std::lock_guard lock(resultMutex);
                    if (progress) progress(completed, fresh.files.size(), file.filename().wstring());
                }
                CheckStop(batchStop.get_token());
                const auto free = FreeSpace(file.parent_path());
                const auto required = Required(file);
                if (required > free || free - required < Reserve) throw SpaceNeeded{};
                Process(file, tools, batchStop.get_token(), commitMutex);
                outcomes[index] = 1;
                std::lock_guard lock(resultMutex);
                ++result.succeeded; ++completed;
                if (progress) progress(completed, fresh.files.size(), file.filename().wstring());
            } catch (const Cancelled&) {
                // Uncommitted files are collected below for cancellation/retry.
            } catch (const SpaceNeeded&) {
                noSpace();
            } catch (const std::exception& error) {
                bool insufficient = false;
                try { insufficient = FreeSpace(file.parent_path()) < Reserve; } catch (...) {}
                if (insufficient) noSpace();
                else {
                    outcomes[index] = 2;
                    std::lock_guard lock(resultMutex);
                    ++result.failed; ++completed;
                    result.errors.push_back(file.filename().wstring() + L"：" + FromUtf8(error.what()));
                    if (progress) progress(completed, fresh.files.size(), file.filename().wstring());
                }
            }
        }
    };
    std::vector<std::jthread> workers;
    workers.reserve(workerLimit);
    for (unsigned n = 0; n < workerLimit; ++n) workers.emplace_back(worker);
    for (auto& thread : workers) thread.join();
    result.cancelled = stop.stop_requested();
    result.needsSpace = needsSpace.load();
    for (std::size_t index = 0; index < outcomes.size(); ++index)
        if (!outcomes[index]) result.remaining.push_back(fresh.files[index]);
    return result;
}
}
