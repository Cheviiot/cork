#include "base/sha256.hpp"

#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

namespace cork {
namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr std::uint32_t rotr(std::uint32_t x, int n) { return std::rotr(x, n); }

std::uint32_t load_be32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void store_be32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

} // namespace

void Sha256::compress(const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = load_be32(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    std::uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];

    for (int i = 0; i < 64; ++i) {
        const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
        const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = S0 + maj;

        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
}

void Sha256::update(std::span<const std::byte> data) {
    const auto *p = reinterpret_cast<const std::uint8_t *>(data.data());
    std::size_t n = data.size();
    total_bits_ += static_cast<std::uint64_t>(n) * 8;

    if (buf_len_ > 0) {
        const std::size_t need = 64 - buf_len_;
        const std::size_t take = n < need ? n : need;
        std::memcpy(buf_.data() + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        n -= take;
        if (buf_len_ == 64) {
            compress(buf_.data());
            buf_len_ = 0;
        }
    }
    while (n >= 64) {
        compress(p);
        p += 64;
        n -= 64;
    }
    if (n > 0) {
        std::memcpy(buf_.data(), p, n);
        buf_len_ = n;
    }
}

void Sha256::update(std::string_view text) {
    update(std::span<const std::byte>(reinterpret_cast<const std::byte *>(text.data()), text.size()));
}

Sha256::Digest Sha256::finish() {
    const std::uint64_t bits = total_bits_;

    // Дополнение по стандарту: байт 0x80, нули, и длина в битах big-endian.
    std::uint8_t pad = 0x80;
    update(std::span<const std::byte>(reinterpret_cast<const std::byte *>(&pad), 1));
    // total_bits_ уже испорчен вызовом выше, поэтому длина взята заранее.
    const std::uint8_t zero = 0;
    while (buf_len_ != 56) {
        update(std::span<const std::byte>(reinterpret_cast<const std::byte *>(&zero), 1));
    }

    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) {
        len[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
    }
    std::memcpy(buf_.data() + 56, len, 8);
    compress(buf_.data());
    buf_len_ = 0;

    Digest out{};
    for (int i = 0; i < 8; ++i) {
        store_be32(out.data() + i * 4, h_[static_cast<std::size_t>(i)]);
    }
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
