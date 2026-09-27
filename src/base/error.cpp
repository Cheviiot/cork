#include "base/error.hpp"

#include <cerrno>
#include <cstring>

namespace cork {

std::string Error::to_string() const {
    std::string out = message;
    // Контекст печатается от внешнего к внутреннему: человек читает сверху вниз
    // начиная с того, что он сам запускал, и только потом доходит до причины.
    for (auto it = context.rbegin(); it != context.rend(); ++it) {
        out += "\n  while ";
        out += *it;
    }
    return out;
}

std::unexpected<Error> err_errno(std::string_view what, int errnum) {
    // strerror_r в GNU-варианте может вернуть указатель на собственный буфер,
    // а может — на статическую строку, поэтому используется возвращаемое
    // значение, а не buf напрямую.
    char buf[256];
    const char *msg = strerror_r(errnum, buf, sizeof buf);

    std::string message(what);
    message += ": ";
    message += msg;

    // ENOENT и EACCES встречаются достаточно часто, чтобы их стоило отличать от
    // прочего ввода-вывода: по ним вызывающий решает, это «нет файла» или
    // настоящая поломка.
    Error::Code code = Error::Code::Io;
    if (errnum == ENOENT) {
        code = Error::Code::NotFound;
    }
    return std::unexpected(Error{code, std::move(message)});
}

} // namespace cork
