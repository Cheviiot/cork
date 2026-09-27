#include "setup/relocate.hpp"

#include <cctype>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "base/fs.hpp"

namespace cork::setup {
namespace {

namespace stdfs = std::filesystem;

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

Result<void> link_if_needed(const std::string &target, const stdfs::path &link) {
    std::error_code ec;
    if (stdfs::is_symlink(link, ec)) {
        const stdfs::path current = stdfs::read_symlink(link, ec);
        if (!ec && current.string() == target) {
            return {};
        }
        stdfs::remove(link, ec);
    } else if (fs::exists_no_follow(link)) {
        // Настоящий каталог с таким именем уже есть — ссылка не нужна.
        return {};
    }
    // Цель относительная, чтобы всё дерево оставалось переносимым: с
    // абсолютной перенос каталога установки ломал бы ссылки.
    stdfs::create_symlink(target, link, ec);
    if (ec) {
        return err_io("creating " + link.string() + " -> " + target + ": " + ec.message());
    }
    return {};
}

} // namespace

Result<void> merge_trees(const stdfs::path &src, const stdfs::path &dest) {
    if (!fs::is_dir(src)) {
        return {};
    }
    if (!fs::is_dir(dest)) {
        if (auto r = fs::mkdir_p(dest.parent_path()); !r.has_value()) {
            return std::unexpected(std::move(r.error()));
        }
        std::error_code ec;
        stdfs::rename(src, dest, ec);
        if (!ec) {
            return {};
        }
        // Переименование через границу файловых систем не работает; тогда
        // создаём каталог и сливаем содержимое поэлементно.
        if (auto r = fs::mkdir_p(dest); !r.has_value()) {
            return std::unexpected(std::move(r.error()));
        }
    }

    // Существующие имена запоминаются в нижнем регистре: пакеты Microsoft не
    // выдерживают регистр единообразно, и «Include» из одного пакета должен
    // слиться с «include» из другого, а не лечь рядом.
    std::map<std::string, std::string> existing;
    std::error_code ec;
    for (const auto &entry : stdfs::directory_iterator(dest, ec)) {
        existing.emplace(to_lower(entry.path().filename().string()),
                         entry.path().filename().string());
    }

    std::vector<stdfs::path> children;
    for (const auto &entry : stdfs::directory_iterator(src, ec)) {
        children.push_back(entry.path());
    }

    for (const auto &child : children) {
        const std::string name = child.filename().string();
        const auto found = existing.find(to_lower(name));
        const stdfs::path target = dest / (found != existing.end() ? found->second : name);

        const bool src_is_dir = fs::is_dir(child);
        const bool dest_exists = fs::exists_no_follow(target);
        const bool dest_is_dir = fs::is_dir(target);

        if (src_is_dir && dest_exists && !dest_is_dir) {
            return err_conflict("cannot merge directory " + child.string() + " over file " +
                                target.string());
        }
        if (!src_is_dir && dest_is_dir) {
            return err_conflict("cannot merge file " + child.string() + " over directory " +
                                target.string());
        }

        if (src_is_dir) {
            if (auto r = merge_trees(child, target); !r.has_value()) {
                return r;
            }
            continue;
        }

        std::error_code move_ec;
        stdfs::rename(child, target, move_ec);
        if (move_ec) {
            // Тот же случай разных файловых систем.
            stdfs::copy_file(child, target, stdfs::copy_options::overwrite_existing, move_ec);
            if (move_ec) {
                return err_io("moving " + child.string() + " to " + target.string() + ": " +
                              move_ec.message());
            }
            stdfs::remove(child, move_ec);
        }
    }
    return {};
}

Result<void> relocate_build_tools(const stdfs::path &unpack, const stdfs::path &dest) {
    // Только то, что нужно инструментам командной строки. Остальное из
    // распаковки выбрасывается: это компоненты IDE.
    const std::vector<std::string> components = {
        "VC", "Windows Kits", "DIA SDK", "MSBuild", "Common7",
    };

    // Имена сопоставляются без учёта регистра, и это не вежливость к
    // пользователю, а необходимость. Разные пакеты Microsoft пишут один и тот
    // же каталог по-разному: «MSBuild», «Msbuild» и «MSBUILD» приезжают в
    // одну распаковку одновременно. Перенос по точному имени забирает один из
    // трёх, а два остаются в unpack и исчезают вместе с ним — вместе со всей
    // поддержкой C++ в MSBuild, потому что Microsoft.Cpp.props лежит именно в
    // «Msbuild».
    std::error_code ec;
    std::vector<stdfs::path> present;
    for (const auto &entry : stdfs::directory_iterator(unpack, ec)) {
        if (ec) {
            return err_io("listing " + unpack.string() + ": " + ec.message());
        }
        if (entry.is_directory(ec)) {
            present.push_back(entry.path());
        }
    }

    for (const auto &component : components) {
        for (const auto &candidate : present) {
            if (to_lower(candidate.filename().string()) != to_lower(component)) {
                continue;
            }
            if (auto r = merge_trees(candidate, dest / component); !r.has_value()) {
                return std::unexpected(std::move(r.error()).at("relocating " + component));
            }
        }
    }
    return {};
}

namespace {

// v<цифры> и ничего больше: v140_xp — это другой набор, а не другое написание
// того же, и подменять его нельзя.
bool numeric_toolset(std::string_view name) {
    if (name.size() < 2 || name[0] != 'v') {
        return false;
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(name[i])) == 0) {
            return false;
        }
    }
    return true;
}

// Имена PlatformToolset, под которыми можно выдать установленный набор.
//
// Список кончается на 140 не по забывчивости. v140, v141, v142, v143 и v145 —
// это всё поколение 14.x, и Microsoft с Visual Studio 2015 держит между ними
// бинарную совместимость: объектный файл от одного линкуется с библиотекой от
// другого. Подмена здесь честная — проект может не собраться из-за строгости
// нового компилятора, но если собрался, результат корректен.
//
// v90, v100, v110 и v120 — это 9.0, 10.0, 11.0 и 12.0, другие ABI. Выдать
// вместо них 14.x значит получить двоичный файл, несовместимый с чужими
// библиотеками той эпохи, причём молча. А код тех лет новый компилятор чаще
// всего не принимает вовсе, и вместо внятного «такого набора нет» человек
// получает гору ошибок компиляции в чужом коде. Пусть уж MSBuild скажет
// прямо, что набора нет.
constexpr const char *kAbiCompatibleToolsets[] = {"140", "141", "142", "143"};

} // namespace

Result<std::string> alias_platform_toolsets(const stdfs::path &dest) {
    std::error_code ec;
    std::string real_name;

    const stdfs::path vc = dest / "MSBuild" / "Microsoft" / "VC";
    for (const auto &schema : stdfs::directory_iterator(vc, ec)) {
        if (ec) {
            break;
        }
        const stdfs::path platforms = schema.path() / "Platforms";
        for (const auto &arch : stdfs::directory_iterator(platforms, ec)) {
            if (ec) {
                break;
            }
            const stdfs::path toolsets = arch.path() / "PlatformToolsets";
            if (!fs::is_dir(toolsets)) {
                continue;
            }

            std::string real;
            for (const auto &entry : stdfs::directory_iterator(toolsets, ec)) {
                if (ec) {
                    break;
                }
                if (entry.is_directory(ec) && numeric_toolset(entry.path().filename().string())) {
                    real = entry.path().filename().string();
                    break;
                }
            }
            if (real.empty()) {
                // Числового набора здесь нет — например, лежат только
                // драйверные наборы WDK. Алиасить нечего.
                continue;
            }
            if (real_name.empty()) {
                real_name = real.substr(1);
            }

            for (const char *n : kAbiCompatibleToolsets) {
                const std::string alias = std::string("v") + n;
                if (alias == real) {
                    continue;
                }
                const stdfs::path link = toolsets / alias;
                // Уже существующее не трогаем: если набор с этим именем
                // установлен по-настоящему, подменять его нечем и незачем.
                if (fs::exists_no_follow(link)) {
                    continue;
                }
                stdfs::create_directory_symlink(real, link, ec);
                if (ec) {
                    return err_io("creating toolset alias " + link.string() + ": " +
                                  ec.message());
                }
                ec.clear();
            }
        }
        ec.clear();
    }
    return real_name;
}

Result<void> create_layout_links(const stdfs::path &dest) {
    if (auto r = link_if_needed("VC", dest / "vc"); !r.has_value()) {
        return r;
    }
    if (auto r = link_if_needed("Windows Kits", dest / "kits"); !r.has_value()) {
        return r;
    }
    if (fs::is_dir(dest / "VC")) {
        if (auto r = link_if_needed("Tools", dest / "VC" / "tools"); !r.has_value()) {
            return r;
        }
        if (fs::is_dir(dest / "VC" / "Tools")) {
            if (auto r = link_if_needed("MSVC", dest / "VC" / "Tools" / "msvc"); !r.has_value()) {
                return r;
            }
        }
    }
    if (fs::is_dir(dest / "Windows Kits")) {
        const stdfs::path kits10 = dest / "Windows Kits" / "10";
        if (fs::is_dir(kits10)) {
            if (auto r = link_if_needed("Include", kits10 / "include"); !r.has_value()) {
                return r;
            }
            if (auto r = link_if_needed("Lib", kits10 / "lib"); !r.has_value()) {
                return r;
            }
            if (auto r = link_if_needed("bin", kits10 / "bin"); !r.has_value()) {
                return r;
            }
        }
    }
    return {};
}

} // namespace cork::setup
