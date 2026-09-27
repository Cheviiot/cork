#include "base/json.hpp"

#include <fmt/format.h>

namespace cork {
namespace {

constexpr int kIndentStep = 4;

} // namespace

void json_escape(std::string &out, std::string_view value) {
    out += '"';
    for (const char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            // Управляющие символы обязаны быть экранированы, иначе документ
            // недействителен. Всё остальное проходит как есть, включая байты
            // выше 0x7f: пути в Linux — произвольные байты, и портить их
            // заменой на U+FFFD нельзя.
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

void JsonWriter::fail(std::string_view what) {
    if (error_.empty()) {
        error_ = std::string(what);
    }
}

void JsonWriter::indent() {
    out_.append(static_cast<std::size_t>(kIndentStep) * stack_.size(), ' ');
}

void JsonWriter::separator() {
    // Значение сразу после ключа продолжает ту же строку: key() уже написал
    // и отступ, и двоеточие. Без этого раннего выхода между ключом и
    // значением вставлялись запятая и перевод строки.
    if (have_key_) {
        need_comma_ = true;
        return;
    }
    if (need_comma_) {
        out_ += ",\n";
    } else if (!out_.empty()) {
        out_ += '\n';
    }
    indent();
    need_comma_ = true;
}

JsonWriter &JsonWriter::key(std::string_view k) {
    if (stack_.empty() || stack_.back() != Scope::Object) {
        fail("key outside of an object");
        return *this;
    }
    if (have_key_) {
        fail("two keys in a row");
        return *this;
    }
    separator();
    have_key_ = true;
    json_escape(out_, k);
    out_ += ": ";
    return *this;
}

JsonWriter &JsonWriter::begin_object() {
    separator();
    have_key_ = false;
    out_ += '{';
    stack_.push_back(Scope::Object);
    need_comma_ = false;
    return *this;
}

JsonWriter &JsonWriter::end_object() {
    if (stack_.empty() || stack_.back() != Scope::Object) {
        fail("closing an object that was not open");
        return *this;
    }
    if (have_key_) {
        fail("key without a value");
        return *this;
    }
    const bool empty = !need_comma_;
    stack_.pop_back();
    if (!empty) {
        out_ += '\n';
        indent();
    }
    out_ += '}';
    need_comma_ = true;
    return *this;
}

JsonWriter &JsonWriter::begin_array() {
    separator();
    have_key_ = false;
    out_ += '[';
    stack_.push_back(Scope::Array);
    need_comma_ = false;
    return *this;
}

JsonWriter &JsonWriter::end_array() {
    if (stack_.empty() || stack_.back() != Scope::Array) {
        fail("closing an array that was not open");
        return *this;
    }
    const bool empty = !need_comma_;
    stack_.pop_back();
    if (!empty) {
        out_ += '\n';
        indent();
    }
    out_ += ']';
    need_comma_ = true;
    return *this;
}

JsonWriter &JsonWriter::value(std::string_view v) {
    if (!stack_.empty() && stack_.back() == Scope::Object && !have_key_) {
        fail("value without a key inside an object");
        return *this;
    }
    separator();
    have_key_ = false;
    json_escape(out_, v);
    return *this;
}

JsonWriter &JsonWriter::value(std::int64_t v) {
    if (!stack_.empty() && stack_.back() == Scope::Object && !have_key_) {
        fail("value without a key inside an object");
        return *this;
    }
    separator();
    have_key_ = false;
    out_ += fmt::format("{}", v);
    return *this;
}

JsonWriter &JsonWriter::value(std::uint64_t v) {
    if (!stack_.empty() && stack_.back() == Scope::Object && !have_key_) {
        fail("value without a key inside an object");
        return *this;
    }
    separator();
    have_key_ = false;
    out_ += fmt::format("{}", v);
    return *this;
}

JsonWriter &JsonWriter::value(bool v) {
    if (!stack_.empty() && stack_.back() == Scope::Object && !have_key_) {
        fail("value without a key inside an object");
        return *this;
    }
    separator();
    have_key_ = false;
    out_ += v ? "true" : "false";
    return *this;
}

JsonWriter &JsonWriter::null() {
    if (!stack_.empty() && stack_.back() == Scope::Object && !have_key_) {
        fail("value without a key inside an object");
        return *this;
    }
    separator();
    have_key_ = false;
    out_ += "null";
    return *this;
}

std::string JsonWriter::take() {
    if (!stack_.empty()) {
        fail("document ends with an unclosed object or array");
    }
    if (!error_.empty()) {
        return {};
    }
    std::string out = std::move(out_);
    out_.clear();
    out += '\n';
    return out;
}

} // namespace cork
