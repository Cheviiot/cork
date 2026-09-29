#include "base/sha256.hpp"

#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include <openssl/evp.h>

namespace cork {

Sha256::Sha256() {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    // Нехватка памяти здесь означала бы, что считать нечем. Молчать нельзя:
    // хеш, посчитанный «никак», не отличить от настоящего, а по нему потом
    // принимается решение о целостности.
    if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA-256 is unavailable in this OpenSSL build");
    }
    ctx_ = ctx;
}

Sha256::~Sha256() { EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(ctx_)); }

Sha256::Sha256(Sha256 &&other) noexcept : ctx_(other.ctx_) { other.ctx_ = nullptr; }

Sha256 &Sha256::operator=(Sha256 &&other) noexcept {
    if (this != &other) {
        EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(ctx_));
        ctx_ = other.ctx_;
        other.ctx_ = nullptr;
    }
    return *this;
}

void Sha256::update(std::span<const std::byte> data) {
    if (ctx_ == nullptr || data.empty()) {
        return;
    }
    EVP_DigestUpdate(static_cast<EVP_MD_CTX *>(ctx_), data.data(), data.size());
}

void Sha256::update(std::string_view text) {
    update(std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()),
                                      text.size()));
}

Sha256::Digest Sha256::finish() {
    Digest out{};
    if (ctx_ == nullptr) {
        return out;
    }
    unsigned length = 0;
    EVP_DigestFinal_ex(static_cast<EVP_MD_CTX *>(ctx_), out.data(), &length);
    EVP_MD_CTX_free(static_cast<EVP_MD_CTX *>(ctx_));
    ctx_ = nullptr;
    return out;
}

std::string Sha256::to_hex(const Digest &d) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(kDigestSize * 2, '\0');
    for (std::size_t i = 0; i < kDigestSize; ++i) {
        out[i * 2] = kHex[d[i] >> 4];
        out[i * 2 + 1] = kHex[d[i] & 0x0f];
    }
    return out;
}

std::string Sha256::hex_of(std::span<const std::byte> data) {
    Sha256 s;
    s.update(data);
    return to_hex(s.finish());
}

std::string Sha256::hex_of(std::string_view text) {
    Sha256 s;
    s.update(text);
    return to_hex(s.finish());
}

Result<std::string> Sha256::hex_of_file(const std::filesystem::path &path) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return err_errno("opening " + path.string(), errno);
    }

    Sha256 s;
    std::vector<std::byte> buf(1u << 20); // мегабайт: пейлоады бывают многогигабайтные
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n > 0) {
            s.update(std::span<const std::byte>(buf.data(), n));
        }
        if (n < buf.size()) {
            if (std::ferror(f) != 0) {
                const int e = errno;
                std::fclose(f);
                return err_errno("reading " + path.string(), e);
            }
            break;
        }
    }
    std::fclose(f);
    return to_hex(s.finish());
}

bool hex_equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    auto lower = [](char c) -> char {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

} // namespace cork
