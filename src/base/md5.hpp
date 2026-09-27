#pragma once

// MD5 нужен ровно для одного: таблица MsiFileHash внутри .msi несёт MD5
// каждого файла, который пакет устанавливает. Это контрольная сумма, которую
// Microsoft уже посчитала за нас, и не сверять её было бы расточительно —
// распакованный файл проверяется бесплатно, без второй загрузки.
//
// Как криптографическая функция MD5 сломан, и здесь он не используется ни для
// чего, кроме сверки с этой таблицей: подлинность цепочки обеспечивают
// sha256 пейлоадов из манифеста и TLS при загрузке.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cork {

class Md5 {
public:
    static constexpr std::size_t kDigestSize = 16;
    using Digest = std::array<std::uint8_t, kDigestSize>;

    Md5() = default;

    void update(std::span<const std::byte> data);
    void update(std::string_view text);

    // После finish() объект использовать нельзя — это разовый расчёт.
    [[nodiscard]] Digest finish();

    static std::string to_hex(const Digest &);
    static Digest of(std::string_view text);

private:
    void compress(const std::uint8_t block[64]);

    std::array<std::uint32_t, 4> h_{0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buf_len_ = 0;
    std::uint64_t total_bits_ = 0;
};

} // namespace cork
