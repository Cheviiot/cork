#include "base/md5.hpp"

#include <cstring>

namespace cork {
namespace {

// Константы раунда — целые части 2^32 * |sin(i+1)|, как в RFC 1321.
constexpr std::uint32_t kK[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
    0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
    0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
    0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
    0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
    0xeb86d391u,
};

constexpr unsigned kShift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

std::uint32_t rotl(std::uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }

} // namespace

void Md5::compress(const std::uint8_t block[64]) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<std::uint32_t>(block[i * 4]) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }

    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    for (unsigned i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        unsigned g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        std::uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl(a + f + kK[i] + m[g], kShift[i]);
        a = tmp;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
}

void Md5::update(std::span<const std::byte> data) {
    total_bits_ += static_cast<std::uint64_t>(data.size()) * 8;
    const auto *p = reinterpret_cast<const std::uint8_t *>(data.data());
    std::size_t n = data.size();
    while (n > 0) {
        std::size_t take = std::min(n, 64 - buf_len_);
        std::memcpy(buf_.data() + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        n -= take;
        if (buf_len_ == 64) {
            compress(buf_.data());
            buf_len_ = 0;
        }
    }
}

void Md5::update(std::string_view text) {
    update(std::span(reinterpret_cast<const std::byte *>(text.data()), text.size()));
}

Md5::Digest Md5::finish() {
    const std::uint64_t bits = total_bits_;
    const std::uint8_t pad = 0x80;
    update(std::span(reinterpret_cast<const std::byte *>(&pad), 1));
    const std::byte zero{};
    while (buf_len_ != 56) {
        update(std::span(&zero, 1));
    }
    // Длина дописывается little-endian — в отличие от SHA-2, где она
    // big-endian. Перепутанный порядок даёт верный хеш для всех входов, кратных
    // блоку, и неверный для остальных, то есть ловится не сразу.
    std::uint8_t len[8];
    for (int i = 0; i < 8; ++i) {
        len[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    update(std::span(reinterpret_cast<const std::byte *>(len), 8));

    Digest out{};
    for (int i = 0; i < 4; ++i) {
        for (int b = 0; b < 4; ++b) {
            out[static_cast<std::size_t>(i * 4 + b)] =
                static_cast<std::uint8_t>(h_[static_cast<std::size_t>(i)] >> (8 * b));
        }
    }
    return out;
}

std::string Md5::to_hex(const Digest &d) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(kDigestSize * 2, '\0');
    for (std::size_t i = 0; i < kDigestSize; ++i) {
        out[i * 2] = kHex[d[i] >> 4];
        out[i * 2 + 1] = kHex[d[i] & 0x0F];
    }
    return out;
}

Md5::Digest Md5::of(std::string_view text) {
    Md5 m;
    m.update(text);
    return m.finish();
}

} // namespace cork
