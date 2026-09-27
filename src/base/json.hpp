#pragma once

// Запись JSON. Чтение делает simdjson, а писателя у него нет, и тянуть вторую
// библиотеку ради килобайтовых файлов незачем.
//
// Главное свойство — детерминированность: порядок ключей задаётся порядком
// вызовов, форматирование фиксировано, и одинаковое содержимое всегда даёт
// одинаковые байты. Это не эстетика: receipt входит в дайджест поколения, и
// перестановка ключей превратилась бы в «дерево изменилось».
//
// Структура проверяется на месте: закрыть массив как объект или записать
// значение без ключа внутри объекта — это дефект в вызывающем коде, и узнать
// о нём надо там, а не при разборе результата.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cork {

class JsonWriter {
public:
    JsonWriter() = default;

    JsonWriter &begin_object();
    JsonWriter &end_object();
    JsonWriter &begin_array();
    JsonWriter &end_array();

    // Ключ для следующего значения. Только внутри объекта.
    JsonWriter &key(std::string_view);

    JsonWriter &value(std::string_view);
    JsonWriter &value(const char *v) { return value(std::string_view(v)); }
    JsonWriter &value(const std::string &v) { return value(std::string_view(v)); }
    JsonWriter &value(std::int64_t);
    JsonWriter &value(std::uint64_t);
    JsonWriter &value(int v) { return value(static_cast<std::int64_t>(v)); }
    JsonWriter &value(bool);
    JsonWriter &null();

    // key + value одним вызовом — так пишется почти всё.
    template <class T>
    JsonWriter &field(std::string_view k, const T &v) {
        return key(k).value(v);
    }
    JsonWriter &field(std::string_view k, const char *v) { return key(k).value(v); }

    // Ошибка структуры, если она была. Пустая строка — всё в порядке.
    [[nodiscard]] const std::string &error() const { return error_; }

    // Готовый документ с завершающим переводом строки. Если структура
    // нарушена или не закрыта, возвращается пустая строка: лучше ничего, чем
    // недописанный JSON, который кто-то попробует разобрать.
    [[nodiscard]] std::string take();

private:
    enum class Scope { Object, Array };

    void separator();
    void indent();
    void fail(std::string_view what);

    std::string out_;
    std::string error_;
    std::vector<Scope> stack_;
    bool need_comma_ = false;
    bool have_key_ = false;
};

// Экранирование одной строки — нужно и вне писателя, например чтобы собрать
// сообщение об ошибке с чужим текстом внутри.
void json_escape(std::string &out, std::string_view value);

} // namespace cork
