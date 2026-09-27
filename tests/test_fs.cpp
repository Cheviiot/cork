// Проверки выхода за корень. Их легко сделать наполовину: проверять имя
// записи, но не цель символьной ссылки, или поставить проверку в одну ветку
// распаковки из трёх. Любая половина бесполезна — архив просто воспользуется
// той, что осталась без присмотра.

#include <string>

#include "base/fs.hpp"
#include "check.h"

namespace fs = cork::fs;
using fs::stdfs::path;

int main() {
    const path base = "/tmp/extract";

    // Обычные имена проходят.
    {
        auto r = fs::safe_join(base, "a/b/c.h");
        CHECK(r.has_value());
        if (r) {
            CHECK_EQ(r->string(), std::string("/tmp/extract/a/b/c.h"));
        }
    }
    // Внутренний ".." допустим, пока не выводит за корень.
    {
        auto r = fs::safe_join(base, "a/../b.h");
        CHECK(r.has_value());
        if (r) {
            CHECK_EQ(r->string(), std::string("/tmp/extract/b.h"));
        }
    }

    // Выход за корень отвергается во всех формах.
    CHECK(!fs::safe_join(base, "../outside").has_value());
    CHECK(!fs::safe_join(base, "a/../../outside").has_value());
    CHECK(!fs::safe_join(base, "../../../../home/user/.bashrc").has_value());
    CHECK(!fs::safe_join(base, "/etc/passwd").has_value());
    CHECK(!fs::safe_join(base, "/").has_value());

    // Имя, которое лишь начинается так же, как корень, не должно считаться
    // «внутри»: /tmp/extract-evil не находится в /tmp/extract. Именно этот
    // случай пропускает наивная проверка по префиксу строки.
    CHECK(!fs::safe_join("/tmp/extract", "../extract-evil/x").has_value());

    // Цель символьной ссылки проверяется отдельно от её имени.
    CHECK(fs::symlink_target_inside(base, base / "bin/msiexec", "wine"));
    CHECK(fs::symlink_target_inside(base, base / "lib/mono/4.5/System.dll",
                                    "../gac/System/4.0.0.0/System.dll"));
    CHECK(!fs::symlink_target_inside(base, base / "bin/evil", "../../etc/passwd"));
    CHECK(!fs::symlink_target_inside(base, base / "bin/evil", "/etc/passwd"));
    // Ссылка в глубине не должна уводить наружу через достаточное число "..".
    CHECK(!fs::symlink_target_inside(base, base / "a/b/c/link", "../../../../etc/passwd"));

    // Атомарная запись и чтение.
    {
        const path dir = fs::stdfs::temp_directory_path() / "cork-test-fs";
        fs::stdfs::remove_all(dir);
        CHECK(fs::mkdir_p(dir).has_value());

        const path file = dir / "data.txt";
        CHECK(fs::write_atomic(file, std::string_view("hello")).has_value());
        auto got = fs::read_file(file);
        CHECK(got.has_value());
        if (got) {
            CHECK_EQ(*got, std::string("hello"));
        }

        // Перезапись не должна оставить временных файлов рядом.
        CHECK(fs::write_atomic(file, std::string_view("second")).has_value());
        got = fs::read_file(file);
        CHECK(got.has_value());
        if (got) {
            CHECK_EQ(*got, std::string("second"));
        }
        int entries = 0;
        for (const auto &e : fs::stdfs::directory_iterator(dir)) {
            (void)e;
            ++entries;
        }
        CHECK_EQ(std::to_string(entries), std::string("1"));

        fs::stdfs::remove_all(dir);
    }

    return cork::test::finish("test_fs");
}
