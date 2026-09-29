#pragma once

// SHA-256 через EVP_* из OpenSSL.
//
// Здесь была своя реализация, и довод за неё звучал так: OpenSSL приезжает
// транзитивно вместе с curl, но завязывать на его версию проверку
// целостности всего, что мы скачиваем, не хочется — алгоритм зафиксирован
// стандартом навсегда, а API OpenSSL между ветками меняется. Довод
// разумный, и он не выдержал замера.
//
// Замерено на этой машине, 2 ГиБ одним потоком: своя — 381 МБ/с, OpenSSL —
// 2160 МБ/с, в 5,7 раза быстрее. Разница не в качестве кода, а в SHA-NI:
// у процессора есть инструкции для SHA-256, и OpenSSL их берёт. Догонять её
// своими силами значит написать и отлаживать ассемблер под несколько
// микроархитектур.
//
// Цена этого решения — ноль: libcrypto уже статически внутри бинарника,
// потому что её тянет curl[ssl]. Новой зависимости не появилось.
//
// Считается этим весь дайджест дерева поколения (12 ГБ на `doctor --deep`) и
// каждый скачанный пейлоад, так что множитель пять с лишним виден человеку.
//
// Векторы NIST в тесте остались: теперь они проверяют не алгоритм, а то, что
// мы правильно его позвали.

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

    Sha256();
    ~Sha256();
    // Контекст OpenSSL владеет ресурсом, поэтому копировать объект нельзя, а
    // перемещать можно: это разовый расчёт, и делить его между двумя
    // владельцами незачем.
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;
    Sha256(Sha256 &&) noexcept;
    Sha256 &operator=(Sha256 &&) noexcept;

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
    // Непрозрачный указатель, чтобы <openssl/evp.h> не попадал в каждый
    // файл, который считает хеш.
    void *ctx_ = nullptr;
};

// Сравнение шестнадцатеричных строк без учёта регистра и без ранних выходов по
// длине содержимого: манифесты Microsoft пишут хеши то строчными, то
// прописными.
[[nodiscard]] bool hex_equal(std::string_view a, std::string_view b);

} // namespace cork
