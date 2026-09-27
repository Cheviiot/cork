#include "manifest/index.hpp"

#include <algorithm>
#include <cctype>

namespace cork::manifest {
namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// «neutral» и пустая строка означают «подходит для любой архитектуры» — это
// не отсутствие сведений, а явное значение в манифесте.
bool arch_neutral(const std::string &v) {
    if (v.empty()) {
        return true;
    }
    const std::string lower = to_lower(v);
    return lower == "neutral";
}

int arch_rank(const std::string &field, std::string_view want) {
    if (want.empty() || arch_neutral(field)) {
        return 0;
    }
    return to_lower(field) == want ? -1 : 1;
}

int language_rank(const std::string &field, std::string_view want) {
    if (field.empty()) {
        return 0;
    }
    const std::string lower = to_lower(field);
    const bool country_specific = want.find('-') != std::string_view::npos;
    if (country_specific ? (lower == want) : (lower.rfind(std::string(want) + "-", 0) == 0)) {
        return 2;
    }
    if (lower.rfind("en-", 0) == 0) {
        return 1;
    }
    return 0;
}

bool matches(const Package &p, const Constraints &c) {
    if (c.chip.has_value() && !arch_neutral(p.chip)) {
        if (to_lower(p.chip) != to_lower(*c.chip)) {
            return false;
        }
    }
    if (c.machine_arch.has_value() && !arch_neutral(p.machine_arch)) {
        if (to_lower(p.machine_arch) != to_lower(*c.machine_arch)) {
            return false;
        }
    }
    // Ограничение версии проверяется здесь и только здесь. Резолвер кладёт его
    // в constraints и считает, что дальше о нём позаботятся; если не
    // позаботиться, запрос «нужна 2.0» молча получит 1.0.
    if (c.version.has_value() && !c.version->contains(p.version)) {
        return false;
    }
    return true;
}

} // namespace

Index Index::build(const Document &doc, std::string_view host_arch, std::string_view language) {
    const std::string arch = to_lower(host_arch);
    const std::string lang = to_lower(language.empty() ? std::string_view("en") : language);

    Index idx;
    for (const auto &p : doc.packages) {
        idx.by_id_[to_lower(p.id)].push_back(&p);
    }

    for (auto &[id, list] : idx.by_id_) {
        // stable_sort, а не sort: при равном приоритете порядок должен
        // остаться таким же, как в манифесте, иначе один и тот же запрос на
        // разных запусках может выбрать разные варианты.
        std::stable_sort(list.begin(), list.end(), [&](const Package *a, const Package *b) {
            for (const auto field : {0, 1, 2}) {
                const auto pick = [field](const Package *p) -> const std::string & {
                    return field == 0 ? p->chip : (field == 1 ? p->machine_arch : p->product_arch);
                };
                const int diff = arch_rank(pick(a), arch) - arch_rank(pick(b), arch);
                if (diff != 0) {
                    return diff < 0;
                }
            }
            const int la = language_rank(a->language, lang);
            const int lb = language_rank(b->language, lang);
            return la > lb;
        });
    }
    return idx;
}

bool Index::contains(std::string_view id) const { return by_id_.count(to_lower(id)) != 0; }

Result<const Package *> Index::find(std::string_view id, const Constraints &c) const {
    const auto it = by_id_.find(to_lower(id));
    if (it == by_id_.end()) {
        return err_not_found("no package \"" + std::string(id) + "\" in the manifest");
    }

    for (const auto *p : it->second) {
        if (matches(*p, c)) {
            return p;
        }
    }

    // Отказ обязан говорить, что именно есть: иначе пользователю остаётся
    // только гадать, какую версию просить.
    std::string available;
    for (const auto *p : it->second) {
        if (!available.empty()) {
            available += ", ";
        }
        available += p->version_text.empty() ? "(no version)" : p->version_text;
        if (!p->chip.empty()) {
            available += " chip=" + p->chip;
        }
        if (!p->machine_arch.empty()) {
            available += " machineArch=" + p->machine_arch;
        }
    }

    std::string wanted;
    if (c.version.has_value() && !c.version->is_any()) {
        wanted += " version " + c.version->text();
    }
    if (c.chip.has_value()) {
        wanted += " chip=" + *c.chip;
    }
    if (c.machine_arch.has_value()) {
        wanted += " machineArch=" + *c.machine_arch;
    }

    return err_not_found("package \"" + std::string(id) + "\" exists but no variant matches" +
                         (wanted.empty() ? std::string(" the constraints") : wanted) +
                         "; available: " + available);
}

std::vector<const Package *> Index::by_type(std::string_view type) const {
    std::vector<const Package *> out;
    for (const auto &[id, list] : by_id_) {
        if (!list.empty() && list.front()->type == type) {
            out.push_back(list.front());
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Package *a, const Package *b) { return a->id < b->id; });
    return out;
}

std::vector<std::string> Index::ids_with_prefix(std::string_view lowercase_prefix) const {
    std::vector<std::string> out;
    for (const auto &[id, list] : by_id_) {
        if (id.rfind(std::string(lowercase_prefix), 0) == 0) {
            out.push_back(id);
        }
    }
    return out;
}

} // namespace cork::manifest
