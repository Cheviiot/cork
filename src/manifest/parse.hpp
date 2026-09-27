#pragma once

#include <string>
#include <string_view>

#include "base/error.hpp"
#include "manifest/model.hpp"

namespace cork::manifest {

// Установочный манифест — один документ на десятки мегабайт, поэтому разбор
// идёт через simdjson On-Demand: он читает за один проход и не материализует
// дерево, которое нам всё равно не нужно — всё сразу перекладывается в модель.
Result<Document> parse_installer_manifest(std::string_view json);

// Манифест канала нужен ровно затем, чтобы достать из него ссылку на
// установочный манифест.
Result<std::string> installer_manifest_url(std::string_view channel_json);

} // namespace cork::manifest
