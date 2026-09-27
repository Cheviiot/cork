// Векторы взяты из FIPS 180-4 и из общеизвестных контрольных значений.
// Отдельно проверяются границы блока: 55, 56, 63, 64 и 65 байт — именно там
// живут ошибки дополнения, потому что длина в битах занимает последние 8 байт
// блока и при 56 байтах перестаёт помещаться.

#include <string>

#include "base/sha256.hpp"
#include "check.h"

using cork::Sha256;

namespace {

std::string of(const std::string &s) { return Sha256::hex_of(std::string_view(s)); }

} // namespace

int main() {
    CHECK_EQ(of(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(of("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    // Границы дополнения.
    CHECK_EQ(of(std::string(55, 'a')),
             std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
    CHECK_EQ(of(std::string(56, 'a')),
             std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
    CHECK_EQ(of(std::string(63, 'a')),
             std::string("7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"));
    CHECK_EQ(of(std::string(64, 'a')),
             std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
    CHECK_EQ(of(std::string(65, 'a')),
             std::string("635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"));

    // Миллион 'a' — классический вектор, заодно проверяет накопление по кускам.
    {
        Sha256 s;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) {
            s.update(std::string_view(chunk));
        }
        CHECK_EQ(Sha256::to_hex(s.finish()),
                 std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
    }

    // hex_equal не должен зависеть от регистра: манифесты Microsoft пишут хеши
    // и так, и так.
    CHECK(cork::hex_equal("ABCdef", "abcDEF"));
    CHECK(!cork::hex_equal("abc", "abcd"));
    CHECK(!cork::hex_equal("abc", "abd"));

    return cork::test::finish("test_sha256");
}
