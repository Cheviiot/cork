#include "manifest/model.hpp"

#include <algorithm>

namespace cork::manifest {

std::string Payload::base_name() const {
    std::string name = file_name;
    // Разделители бывают и те, и другие: манифест писался под Windows, но
    // встречается и прямой слэш.
    const std::size_t back = name.find_last_of('\\');
    if (back != std::string::npos) {
        name = name.substr(back + 1);
    }
    const std::size_t fwd = name.find_last_of('/');
    if (fwd != std::string::npos) {
        name = name.substr(fwd + 1);
    }
    return name;
}

const char *Dependency::kind_name() const {
    switch (kind) {
        case Kind::Required:
            return "required";
        case Kind::Recommended:
            return "recommended";
        case Kind::Optional:
            return "optional";
    }
    return "required";
}

std::string Package::key() const {
    std::string k = id;
    if (!version_text.empty()) {
        k += "-" + version_text;
    }
    if (!chip.empty()) {
        k += "-chip." + chip;
    }
    if (!machine_arch.empty()) {
        k += "-machineArch." + machine_arch;
    }
    if (!product_arch.empty()) {
        k += "-productArch." + product_arch;
    }
    return k;
}

std::int64_t Package::download_size() const {
    std::int64_t sum = 0;
    for (const auto &p : payloads) {
        sum += p.size;
    }
    return sum;
}

const LocalizedResource *Package::localized_for(std::string_view want) const {
    if (localized.empty()) {
        return nullptr;
    }
    if (want.empty()) {
        want = "en";
    }

    std::string lower_want(want);
    std::transform(lower_want.begin(), lower_want.end(), lower_want.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const LocalizedResource *best = &localized.front();
    int best_score = -1;
    for (const auto &lr : localized) {
        std::string lang = lr.language;
        std::transform(lang.begin(), lang.end(), lang.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        int score = 0;
        if (lang == lower_want) {
            score = 3;
        } else if (lang.rfind(lower_want + "-", 0) == 0) {
            score = 2;
        } else if (lang.rfind("en", 0) == 0) {
            score = 1;
        }
        if (score > best_score) {
            best_score = score;
            best = &lr;
        }
    }
    return best;
}

} // namespace cork::manifest
