#pragma once

// Своя реализация SHA-256, а не EVP_* из OpenSSL. OpenSSL и так приезжает
// транзитивно вместе с curl, но завязывать на его версию проверку целостности
// всего, что мы скачиваем, в статической сборке не хочется: алгоритм
// зафиксирован стандартом навсегда, а API OpenSSL между ветками меняется.
// Полтораста строк с векторами NIST в тесте — обмен в нашу пользу.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "base/error.hpp"

namespace cork {

class Sha256 {
public:
    static constexpr std::size_t kDigestSize = 32;
    using Digest = std::array<std::uint8_t, kDigestSize>;

    Sha256() = default;

    void update(std::span<const std::byte> data);
    void update(std::string_view text);

    // После finish() объект использовать нельзя — это разовый расчёт.
    [[nodiscard]] Digest finish();

    static std::string to_hex(const Digest &);

    static std::string hex_of(std::span<const std::byte> data);
    static std::string hex_of(std::string_view text);

    // Хеш файла потоком: пейлоады MSVC доходят до нескольких гигабайт, читать
    // их целиком в память нельзя.
    static Result<std::string> hex_of_file(const std::filesystem::path &);

private:
    void compress(const std::uint8_t block[64]);

    std::array<std::uint32_t, 8> h_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buf_len_ = 0;
    std::uint64_t total_bits_ = 0;
};

// Сравнение шестнадцатеричных строк без учёта регистра и без ранних выходов по
// длине содержимого: манифесты Microsoft пишут хеши то строчными, то
// прописными.
[[nodiscard]] bool hex_equal(std::string_view a, std::string_view b);

} // namespace cork
