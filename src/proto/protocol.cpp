#include "proto/protocol.hpp"

#include <cstring>

namespace cork::proto {
namespace {

// Порядок байтов задан явно, хотя обе стороны сегодня little-endian x86-64.
// Это формат файла, а не структура в памяти: стоит он ничего, а читателю
// сообщает, что здесь не полагаются на разметку конкретной платформы.
void put_u32(std::vector<unsigned char> &out, std::uint32_t v) {
    out.push_back(static_cast<unsigned char>(v));
    out.push_back(static_cast<unsigned char>(v >> 8));
    out.push_back(static_cast<unsigned char>(v >> 16));
    out.push_back(static_cast<unsigned char>(v >> 24));
}

void put_str(std::vector<unsigned char> &out, const std::string &s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

class Reader {
public:
    Reader(const unsigned char *data, std::size_t size) : p_(data), end_(data + size) {}

    bool u32(std::uint32_t &v) {
        if (static_cast<std::size_t>(end_ - p_) < 4) {
            return false;
        }
        v = static_cast<std::uint32_t>(p_[0]) | (static_cast<std::uint32_t>(p_[1]) << 8) |
            (static_cast<std::uint32_t>(p_[2]) << 16) | (static_cast<std::uint32_t>(p_[3]) << 24);
        p_ += 4;
        return true;
    }

    bool str(std::string &s) {
        std::uint32_t len = 0;
        if (!u32(len)) {
            return false;
        }
        // Длина сверяется с реальным остатком до выделения памяти: иначе
        // испорченный заголовок с длиной 0xffffffff заставил бы нас выделить
        // четыре гигабайта прежде, чем мы поймём, что файл обрезан.
        if (static_cast<std::size_t>(end_ - p_) < len) {
            return false;
        }
        s.assign(reinterpret_cast<const char *>(p_), len);
        p_ += len;
        return true;
    }

private:
    const unsigned char *p_;
    const unsigned char *end_;
};

} // namespace

std::vector<unsigned char> encode(const Request &r) {
    std::vector<unsigned char> out;
    put_u32(out, kRequestMagic);
    put_u32(out, kVersion);
    put_u32(out, r.flags);
    put_str(out, r.exe);
    put_str(out, r.cwd);
    put_str(out, r.status_path);

    put_u32(out, static_cast<std::uint32_t>(r.args.size()));
    for (const auto &a : r.args) {
        put_str(out, a);
    }

    put_u32(out, static_cast<std::uint32_t>(r.path_refs.size()));
    for (const auto &ref : r.path_refs) {
        put_u32(out, ref.index);
        put_u32(out, ref.offset);
    }

    put_u32(out, static_cast<std::uint32_t>(r.child_env.size()));
    for (const auto &kv : r.child_env) {
        put_str(out, kv);
    }

    put_u32(out, static_cast<std::uint32_t>(r.response_files.size()));
    for (const auto &rf : r.response_files) {
        put_u32(out, rf.arg_index);
        put_u32(out, static_cast<std::uint32_t>(rf.args.size()));
        for (const auto &a : rf.args) {
            put_str(out, a);
        }
        put_u32(out, static_cast<std::uint32_t>(rf.path_refs.size()));
        for (const auto &ref : rf.path_refs) {
            put_u32(out, ref.index);
            put_u32(out, ref.offset);
        }
    }
    return out;
}

std::vector<unsigned char> encode(const Status &s) {
    std::vector<unsigned char> out;
    put_u32(out, kStatusMagic);
    put_u32(out, kVersion);
    put_u32(out, static_cast<std::uint32_t>(s.kind));
    put_u32(out, s.code);
    put_u32(out, s.child_pid);
    put_u32(out, s.flags);
    return out;
}

std::uint32_t decode(const unsigned char *data, std::size_t size, Request &out) {
    Reader r(data, size);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;

    if (!r.u32(magic)) {
        return kReasonTruncated;
    }
    if (magic != kRequestMagic) {
        return kReasonBadMagic;
    }
    if (!r.u32(version)) {
        return kReasonTruncated;
    }
    if (version != kVersion) {
        return kReasonBadVersion;
    }

    if (!r.u32(out.flags) || !r.str(out.exe) || !r.str(out.cwd) || !r.str(out.status_path)) {
        return kReasonTruncated;
    }

    std::uint32_t count = 0;
    if (!r.u32(count)) {
        return kReasonTruncated;
    }
    out.args.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string s;
        if (!r.str(s)) {
            return kReasonTruncated;
        }
        out.args.push_back(std::move(s));
    }

    if (!r.u32(count)) {
        return kReasonTruncated;
    }
    out.path_refs.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        PathRef ref;
        if (!r.u32(ref.index) || !r.u32(ref.offset)) {
            return kReasonTruncated;
        }
        // Индекс за пределами args — это не «пропустим», а испорченный запрос:
        // хелпер иначе тихо не переведёт путь, и сборка упадёт где-то дальше с
        // невнятной ошибкой компилятора.
        if (ref.index >= out.args.size()) {
            return kReasonBadIndex;
        }
        if (ref.offset > out.args[ref.index].size()) {
            return kReasonBadIndex;
        }
        out.path_refs.push_back(ref);
    }

    if (!r.u32(count)) {
        return kReasonTruncated;
    }
    out.child_env.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string kv;
        if (!r.str(kv)) {
            return kReasonTruncated;
        }
        out.child_env.push_back(std::move(kv));
    }

    if (!r.u32(count)) {
        return kReasonTruncated;
    }
    out.response_files.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        ResponseFile rf;
        if (!r.u32(rf.arg_index)) {
            return kReasonTruncated;
        }
        if (rf.arg_index >= out.args.size()) {
            return kReasonBadIndex;
        }
        std::uint32_t n = 0;
        if (!r.u32(n)) {
            return kReasonTruncated;
        }
        for (std::uint32_t k = 0; k < n; ++k) {
            std::string a;
            if (!r.str(a)) {
                return kReasonTruncated;
            }
            rf.args.push_back(std::move(a));
        }
        if (!r.u32(n)) {
            return kReasonTruncated;
        }
        for (std::uint32_t k = 0; k < n; ++k) {
            PathRef ref;
            if (!r.u32(ref.index) || !r.u32(ref.offset)) {
                return kReasonTruncated;
            }
            // Ссылка за пределы своего же списка аргументов — испорченный
            // запрос. Пропустить её молча значит оставить путь непереведённым
            // и получить отказ компилятора вместо отказа здесь.
            if (ref.index >= rf.args.size() ||
                ref.offset > rf.args[ref.index].size()) {
                return kReasonBadIndex;
            }
            rf.path_refs.push_back(ref);
        }
        out.response_files.push_back(std::move(rf));
    }
    return 0;
}

std::uint32_t decode(const unsigned char *data, std::size_t size, Status &out) {
    Reader r(data, size);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t kind = 0;

    if (!r.u32(magic)) {
        return kReasonTruncated;
    }
    if (magic != kStatusMagic) {
        return kReasonBadMagic;
    }
    if (!r.u32(version)) {
        return kReasonTruncated;
    }
    if (version != kVersion) {
        return kReasonBadVersion;
    }
    if (!r.u32(kind) || !r.u32(out.code) || !r.u32(out.child_pid) || !r.u32(out.flags)) {
        return kReasonTruncated;
    }
    if (kind < static_cast<std::uint32_t>(StatusKind::ChildExited) ||
        kind > static_cast<std::uint32_t>(StatusKind::BadRequest)) {
        return kReasonBadVersion;
    }
    out.kind = static_cast<StatusKind>(kind);
    return 0;
}

} // namespace cork::proto
