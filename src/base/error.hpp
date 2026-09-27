#pragma once

// Единый способ вернуть отказ. Исключения как поток управления здесь не
// используются: во-первых, из-за статической линковки, во-вторых — и это
// важнее — потому что отказ обязательного этапа обязан быть незаметным для
// вызывающего. Этап, который «пробует», при провале оставляет строчку в
// выводе, а установка объявляется успешной; потом она не собирает, и понять
// почему уже нельзя. Result заставляет вызывающего либо обработать отказ,
// либо прокинуть его выше.

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cork {

struct Error {
    enum class Code {
        Io,            // не удалось прочитать, записать, создать
        Network,       // соединение, таймаут, код ответа
        Protocol,      // ответ сервера не соответствует ожидаемому (Content-Range и т.п.)
        Format,        // содержимое не разбирается: JSON, CFBF, zip, patch
        Verification,  // хеш, подпись, дайджест дерева не сошлись
        Conflict,      // занятая блокировка, столкновение файла и каталога
        NotFound,      // запрошенного пакета, версии, файла нет
        Unsupported,   // формат или возможность распознаны, но не поддерживаются
        Interrupted,   // сигнал или таймаут
        Config,        // конфигурация недействительна или другой версии схемы
        Internal,      // нарушен инвариант — дефект в нас самих
    };

    Code code = Code::Internal;

    // Текст технический и всегда английский: он попадает в отчёты об ошибках и
    // в поиск по сети. Локализуется только «хром» CLI, см. src/i18n.
    std::string message;

    // Цепочка «при чём это случилось», от внутреннего к внешнему. Позволяет
    // сказать «не удалось открыть файл» один раз, а контекст («распаковка
    // такого-то пакета», «подготовка такого-то поколения») добавить по пути.
    std::vector<std::string> context;

    Error(Code c, std::string msg) : code(c), message(std::move(msg)) {}

    Error &at(std::string_view what) & {
        context.emplace_back(what);
        return *this;
    }
    Error &&at(std::string_view what) && {
        context.emplace_back(what);
        return std::move(*this);
    }

    // Однострочное представление: сообщение, затем контекст от внешнего к
    // внутреннему — так его читает человек, начиная с того, что он запускал.
    [[nodiscard]] std::string to_string() const;
};

template <class T>
using Result = std::expected<T, Error>;

// Короткие конструкторы отказа. Пишутся как `return err_io("...")`, поэтому
// названы без префикса make_.
#define CORK_DEFINE_ERR(fn, enumerator)                                     \
    [[nodiscard]] inline std::unexpected<Error> fn(std::string msg) {          \
        return std::unexpected(Error{Error::Code::enumerator, std::move(msg)}); \
    }

CORK_DEFINE_ERR(err_io, Io)
CORK_DEFINE_ERR(err_network, Network)
CORK_DEFINE_ERR(err_protocol, Protocol)
CORK_DEFINE_ERR(err_format, Format)
CORK_DEFINE_ERR(err_verification, Verification)
CORK_DEFINE_ERR(err_conflict, Conflict)
CORK_DEFINE_ERR(err_not_found, NotFound)
CORK_DEFINE_ERR(err_unsupported, Unsupported)
CORK_DEFINE_ERR(err_interrupted, Interrupted)
CORK_DEFINE_ERR(err_config, Config)
CORK_DEFINE_ERR(err_internal, Internal)

#undef CORK_DEFINE_ERR

// Отказ системного вызова: к сообщению добавляется расшифровка errno. Отдельная
// функция, а не err_io(strerror(errno)) на месте вызова, потому что errno легко
// затирается любым промежуточным вызовом — здесь он снимается сразу.
[[nodiscard]] std::unexpected<Error> err_errno(std::string_view what, int errnum);

} // namespace cork
