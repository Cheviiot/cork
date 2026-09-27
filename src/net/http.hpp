#pragma once

// HTTP: получение манифестов и скачивание пейлоадов с докачкой.
//
// Пейлоады MSVC и WinSDK — это сотни мегабайт на файл и гигабайты в сумме.
// Обрыв на середине не должен означать «начать сначала», поэтому загрузка
// идёт во временный файл рядом с целью, а повтор продолжает с того байта, где
// остановился.
//
// Отдельная тонкость, на которой легко обжечься: ответ 206 сам по себе не
// доказывает, что сервер начал тело там, где его
// попросили. Без сверки Content-Range дописывание в .part молча портит файл,
// и обнаруживается это только на проверке sha256 — то есть после того, как
// скачаны все гигабайты.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "base/error.hpp"

namespace cork::net {

// Вызывается по мере передачи. Возврат false просит прервать загрузку —
// так работает отмена по Ctrl-C.
using ProgressFn = std::function<bool(std::int64_t transferred, std::int64_t total)>;

struct Options {
    // Общий предел на передачу. Ноль — без предела: многогигабайтный пейлоад
    // на медленном канале может идти очень долго, и обрывать его по часам
    // бессмысленно. Застой ловится не этим, а low_speed_* ниже.
    int timeout_seconds = 0;
    int connect_timeout_seconds = 30;

    // Если скорость держится ниже low_speed_bytes дольше low_speed_seconds,
    // соединение считается зависшим. Это и есть настоящая защита от застоя.
    long low_speed_bytes = 1024;
    long low_speed_seconds = 60;

    ProgressFn progress;
};

// Ответ целиком в память — для манифестов. Установочный манифест доходит до
// десятков мегабайт, что для памяти несущественно.
Result<std::string> get(std::string_view url, const Options & = {});

struct DownloadResult {
    std::int64_t bytes_transferred = 0;  // сколько прошло по сети в этот раз
    bool resumed = false;
};

// Скачивает url в dest через dest + ".part". Если .part уже есть, загрузка
// продолжается с его размера.
Result<DownloadResult> download_file(std::string_view url, const std::filesystem::path &dest,
                                     const Options & = {});

// Разбор заголовка Content-Range вида "bytes 123-456/789". Вынесен в
// интерфейс, потому что именно он проверяется отдельным тестом: ошибка здесь
// означает молча испорченный файл.
bool parse_content_range_start(std::string_view header, std::int64_t &start);

// Инициализация curl один раз на процесс. Вызывать из main до первого
// обращения к сети; повторные вызовы безвредны.
void global_init();

} // namespace cork::net
