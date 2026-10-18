#include "PackageReader.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hkdf.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace mark::setup {
namespace {
constexpr std::size_t HeaderBytes = 96;
constexpr std::size_t TagBytes = 16;
constexpr std::size_t TrailerBytes = 88;
constexpr std::size_t BufferBytes = 4 * 1024 * 1024;

class Sha256 {
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<unsigned char> object_;
    void Close() noexcept {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
        hash_ = nullptr;
        algorithm_ = nullptr;
    }
public:
    Sha256() {
        try {
            DWORD bytes = 0, size = 0;
            if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
                || BCryptGetProperty(algorithm_, BCRYPT_OBJECT_LENGTH,
                    reinterpret_cast<PUCHAR>(&size), sizeof(size), &bytes, 0) < 0)
                throw std::runtime_error("Unable to initialize file verification.");
            object_.resize(size);
            if (BCryptCreateHash(algorithm_, &hash_, object_.data(), size, nullptr, 0, 0) < 0)
                throw std::runtime_error("Unable to initialize file verification.");
        } catch (...) { Close(); throw; }
    }
    ~Sha256() { Close(); }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void Update(const unsigned char* data, std::size_t size) {
        if (size > 0xffffffffULL || BCryptHashData(hash_, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) < 0)
            throw std::runtime_error("File verification failed.");
    }
    std::array<unsigned char, 32> Finish() {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("File verification failed.");
        return digest;
    }
};

std::uint64_t ReadLe64(const unsigned char* bytes) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) value |= std::uint64_t(bytes[i]) << (i * 8);
    return value;
}

std::uint32_t ReadLe32(const unsigned char* bytes) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) value |= std::uint32_t(bytes[i]) << (i * 8);
    return value;
}

void Read(std::ifstream& input, unsigned char* data, std::size_t size) {
    if (!input.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size)))
        throw std::runtime_error("The installer is incomplete. Download the file again.");
}

std::string Hex(const unsigned char* data, std::size_t size) {
    constexpr char digits[] = "0123456789abcdef";
    std::string value(size * 2, '0');
    for (std::size_t i = 0; i < size; ++i) {
        value[i * 2] = digits[data[i] >> 4];
        value[i * 2 + 1] = digits[data[i] & 15];
    }
    return value;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error("This setup build has no valid knowledge-base key.");
}
}

std::string DatabaseKey() {
#ifdef GNSS_DOWNLOADER_COMPILED_DB_KEY
    const std::string compiled = GNSS_DOWNLOADER_COMPILED_DB_KEY;
    if (!compiled.empty()) return compiled;
#endif
    const DWORD required = GetEnvironmentVariableA("GNSS_DOWNLOADER_DB_KEY", nullptr, 0);
    if (required == 0) return {};
    std::string value(required, '\0');
    const DWORD length = GetEnvironmentVariableA("GNSS_DOWNLOADER_DB_KEY", value.data(), required);
    if (length == 0 || length >= required) return {};
    value.resize(length);
    return value;
}

Payload ReadPayload(const std::filesystem::path& file, bool raw_package) {
    Payload payload;
    payload.file = file;
    const auto size = std::filesystem::file_size(file);
    if (raw_package) {
        payload.length = size;
    } else {
        if (size < TrailerBytes) throw std::runtime_error("This installer contains no application package.");
        std::ifstream input(file, std::ios::binary);
        input.seekg(static_cast<std::streamoff>(size - TrailerBytes));
        std::array<unsigned char, TrailerBytes> trailer{};
        Read(input, trailer.data(), trailer.size());
        if (std::memcmp(trailer.data(), "MARKSFX2", 8) != 0)
            throw std::runtime_error("This installer contains no application package.");
        payload.offset = ReadLe64(trailer.data() + 8);
        payload.length = ReadLe64(trailer.data() + 16);
        if (payload.offset > size - TrailerBytes || payload.length != size - TrailerBytes - payload.offset)
            throw std::runtime_error("The installer is incomplete. Download the file again.");
        std::copy_n(trailer.data() + 24, 32, payload.digest.begin());
        payload.has_digest = true;
        payload.space.unpacked = ReadLe64(trailer.data() + 56);
        payload.space.installed = ReadLe64(trailer.data() + 64);
        payload.space.conversionExtra[0] = ReadLe64(trailer.data() + 72);
        payload.space.conversionExtra[1] = ReadLe64(trailer.data() + 80);
    }
    if (payload.length < HeaderBytes + TagBytes)
        throw std::runtime_error("The installation package is incomplete.");
    return payload;
}

std::string FileDigest(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to read a file for verification.");
    Sha256 context;
    std::vector<unsigned char> buffer(BufferBytes);
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) context.Update(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!input.eof()) throw std::runtime_error("Unable to finish reading a file for verification.");
    const auto digest = context.Finish();
    return Hex(digest.data(), digest.size());
}

std::uint32_t DecodePayload(const Payload& payload, const std::filesystem::path& output,
                            const std::atomic_bool& cancelled, const Progress& progress) {
    if (std::filesystem::exists(output)) throw std::runtime_error("The preparation file already exists.");
    std::ifstream input(payload.file, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to read the installer.");
    input.seekg(static_cast<std::streamoff>(payload.offset));
    std::array<unsigned char, HeaderBytes> header{};
    Read(input, header.data(), header.size());
    if (std::memcmp(header.data(), "MARKMKB1", 8) != 0)
        throw std::runtime_error("The installation package format is not supported.");
    const auto length = ReadLe64(header.data() + 8);
    const auto databases = ReadLe32(header.data() + 16);
    if (length != payload.length - HeaderBytes - TagBytes || databases == 0 || databases > 4096)
        throw std::runtime_error("The installation package header is invalid.");
    const std::string key = DatabaseKey();
    if (key.size() != 64) throw std::runtime_error("This setup build has no valid knowledge-base key.");
    std::array<unsigned char, 32> secret{}, derived{};
    for (std::size_t i = 0; i != secret.size(); ++i)
        secret[i] = static_cast<unsigned char>((HexDigit(key[i * 2]) << 4) | HexDigit(key[i * 2 + 1]));
    constexpr char info[] = "MARK compressed knowledge distribution v1";
    if (HKDF(derived.data(), derived.size(), EVP_sha256(), secret.data(), secret.size(),
             header.data() + 20, 32, reinterpret_cast<const unsigned char*>(info), sizeof(info) - 1) != 1)
        throw std::runtime_error("Unable to initialize package decryption.");
    OPENSSL_cleanse(secret.data(), secret.size());
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int written = 0;
    if (!cipher || EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1
        || EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1
        || EVP_DecryptInit_ex(cipher.get(), nullptr, nullptr, derived.data(), header.data() + 52) != 1
        || EVP_DecryptUpdate(cipher.get(), nullptr, &written, header.data(), static_cast<int>(header.size())) != 1)
        throw std::runtime_error("Unable to initialize package decryption.");
    OPENSSL_cleanse(derived.data(), derived.size());
    Sha256 plain_hash, package_hash;
    package_hash.Update(header.data(), header.size());
    try {
        std::ofstream decoded(output, std::ios::binary | std::ios::out);
        if (!decoded) throw std::runtime_error("Unable to create a preparation file in the selected folder.");
        std::vector<unsigned char> encoded(BufferBytes), plain(BufferBytes + EVP_MAX_BLOCK_LENGTH);
        std::uint64_t remaining = length;
        while (remaining != 0) {
            if (cancelled) throw std::runtime_error("Setup cancelled.");
            const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, encoded.size()));
            Read(input, encoded.data(), size);
            package_hash.Update(encoded.data(), size);
            if (EVP_DecryptUpdate(cipher.get(), plain.data(), &written, encoded.data(), static_cast<int>(size)) != 1)
                throw std::runtime_error("Unable to decrypt the installation package.");
            decoded.write(reinterpret_cast<const char*>(plain.data()), written);
            if (!decoded) throw std::runtime_error("Unable to write preparation files. Check the available disk space.");
            plain_hash.Update(plain.data(), static_cast<std::size_t>(written));
            remaining -= size;
            progress(static_cast<int>((length - remaining) * 100 / std::max<std::uint64_t>(length, 1)), L"Verifying installation package");
        }
        std::array<unsigned char, TagBytes> tag{};
        Read(input, tag.data(), tag.size());
        package_hash.Update(tag.data(), tag.size());
        if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()), tag.data()) != 1
            || EVP_DecryptFinal_ex(cipher.get(), plain.data(), &written) != 1)
            throw std::runtime_error("Package verification failed. Download the installer again.");
        decoded.write(reinterpret_cast<const char*>(plain.data()), written);
        plain_hash.Update(plain.data(), static_cast<std::size_t>(written));
        const auto actual_plain = plain_hash.Finish(), actual_package = package_hash.Finish();
        if (CRYPTO_memcmp(actual_plain.data(), header.data() + 64, actual_plain.size()) != 0
            || (payload.has_digest && CRYPTO_memcmp(actual_package.data(), payload.digest.data(), actual_package.size()) != 0))
            throw std::runtime_error("Package verification failed. Download the installer again.");
        decoded.close();
        if (!decoded || std::filesystem::file_size(output) != length)
            throw std::runtime_error("The preparation file could not be written completely.");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        throw;
    }
    return databases;
}
}
