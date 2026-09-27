#include "archive/cab.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include <fmt/format.h>
#include <mspack.h>

#include "base/fs.hpp"

namespace cork::archive {
namespace {

// Файл с точки зрения libmspack: либо кусок памяти (встроенный в .msi архив),
// либо обычный файл. Один тип на оба случая, потому что библиотека не делает
// между ними различия и открывает то и другое одним методом.
struct SysFile {
    const char *mem = nullptr;
    std::size_t mem_size = 0;
    std::size_t mem_pos = 0;
    std::FILE *fp = nullptr;
};

struct System {
    // Должен быть первым полем: libmspack передаёт обратно указатель на него,
    // и мы приводим его к System*. Это работает, только пока смещение нулевое.
    mspack_system base{};
    std::unordered_map<std::string, std::string_view> memory;
    std::string last_message;
};

System *self_of(mspack_system *s) { return reinterpret_cast<System *>(s); }

mspack_file *sys_open(mspack_system *s, const char *filename, int mode) {
    auto *sys = self_of(s);
    if (mode == MSPACK_SYS_OPEN_READ) {
        if (auto it = sys->memory.find(filename ? filename : ""); it != sys->memory.end()) {
            auto *f = new SysFile;
            f->mem = it->second.data();
            f->mem_size = it->second.size();
            return reinterpret_cast<mspack_file *>(f);
        }
    }
    const char *m = nullptr;
    switch (mode) {
    case MSPACK_SYS_OPEN_READ: m = "rb"; break;
    case MSPACK_SYS_OPEN_WRITE: m = "wb"; break;
    case MSPACK_SYS_OPEN_UPDATE: m = "r+b"; break;
    case MSPACK_SYS_OPEN_APPEND: m = "ab"; break;
    default: return nullptr;
    }
    std::FILE *fp = std::fopen(filename, m);
    if (!fp) {
        return nullptr;
    }
    auto *f = new SysFile;
    f->fp = fp;
    return reinterpret_cast<mspack_file *>(f);
}

void sys_close(mspack_file *file) {
    auto *f = reinterpret_cast<SysFile *>(file);
    if (!f) {
        return;
    }
    if (f->fp) {
        std::fclose(f->fp);
    }
    delete f;
}

int sys_read(mspack_file *file, void *buffer, int bytes) {
    auto *f = reinterpret_cast<SysFile *>(file);
    if (!f || bytes < 0) {
        return -1;
    }
    if (f->mem) {
        std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(bytes),
                                              f->mem_size - std::min(f->mem_pos, f->mem_size));
        std::memcpy(buffer, f->mem + f->mem_pos, n);
        f->mem_pos += n;
        return static_cast<int>(n);
    }
    return static_cast<int>(std::fread(buffer, 1, static_cast<std::size_t>(bytes), f->fp));
}

int sys_write(mspack_file *file, void *buffer, int bytes) {
    auto *f = reinterpret_cast<SysFile *>(file);
    if (!f || !f->fp || bytes < 0) {
        return -1;
    }
    return static_cast<int>(std::fwrite(buffer, 1, static_cast<std::size_t>(bytes), f->fp));
}

int sys_seek(mspack_file *file, off_t offset, int mode) {
    auto *f = reinterpret_cast<SysFile *>(file);
    if (!f) {
        return -1;
    }
    if (f->mem) {
        // Смещение считается в знаковых величинах и может уйти в минус: это
        // испорченный архив, а не повод завернуть индекс через ноль в конец
        // буфера.
        off_t pos = 0;
        switch (mode) {
        case MSPACK_SYS_SEEK_START: pos = offset; break;
        case MSPACK_SYS_SEEK_CUR: pos = static_cast<off_t>(f->mem_pos) + offset; break;
        case MSPACK_SYS_SEEK_END: pos = static_cast<off_t>(f->mem_size) + offset; break;
        default: return -1;
        }
        if (pos < 0 || static_cast<std::size_t>(pos) > f->mem_size) {
            return -1;
        }
        f->mem_pos = static_cast<std::size_t>(pos);
        return 0;
    }
    int whence = mode == MSPACK_SYS_SEEK_START  ? SEEK_SET
                 : mode == MSPACK_SYS_SEEK_CUR  ? SEEK_CUR
                 : mode == MSPACK_SYS_SEEK_END  ? SEEK_END
                                                : -1;
    if (whence < 0) {
        return -1;
    }
    return ::fseeko(f->fp, offset, whence) == 0 ? 0 : -1;
}

off_t sys_tell(mspack_file *file) {
    auto *f = reinterpret_cast<SysFile *>(file);
    if (!f) {
        return -1;
    }
    return f->mem ? static_cast<off_t>(f->mem_pos) : ::ftello(f->fp);
}

void sys_message(mspack_file *, const char *, ...) {
    // Библиотека печатает диагностику в свободной форме и без привязки к
    // конкретному отказу. В отчёт она не годится, а в stderr посреди сборки
    // не нужна — коды ошибок libmspack достаточно конкретны сами по себе.
}

void *sys_alloc(mspack_system *, std::size_t bytes) { return std::malloc(bytes); }
void sys_free(void *p) { std::free(p); }
void sys_copy(void *src, void *dest, std::size_t bytes) { std::memcpy(dest, src, bytes); }

const char *cab_error(int code) {
    switch (code) {
    case MSPACK_ERR_OK: return "ok";
    case MSPACK_ERR_ARGS: return "bad arguments";
    case MSPACK_ERR_OPEN: return "cannot open";
    case MSPACK_ERR_READ: return "read failed";
    case MSPACK_ERR_WRITE: return "write failed";
    case MSPACK_ERR_SEEK: return "seek failed";
    case MSPACK_ERR_NOMEMORY: return "out of memory";
    case MSPACK_ERR_SIGNATURE: return "not a cabinet";
    case MSPACK_ERR_DATAFORMAT: return "corrupt cabinet";
    case MSPACK_ERR_CHECKSUM: return "checksum mismatch";
    case MSPACK_ERR_DECRUNCH: return "decompression failed";
    default: return "unknown error";
    }
}

} // namespace

struct Cab::Impl {
    System sys;
    mscab_decompressor *decomp = nullptr;
    mscabd_cabinet *cab = nullptr;
    std::string label;
    std::unordered_map<std::string, mscabd_file *> by_name;

    ~Impl() {
        if (decomp) {
            if (cab) {
                decomp->close(decomp, cab);
            }
            mspack_destroy_cab_decompressor(decomp);
        }
    }
};

Cab::Cab() : impl_(std::make_unique<Impl>()) {}
Cab::Cab(Cab &&) noexcept = default;
Cab &Cab::operator=(Cab &&) noexcept = default;
Cab::~Cab() = default;

Result<Cab> Cab::open(const std::filesystem::path &path) {
    return open_memory({}, path.string());
}

Result<Cab> Cab::open_memory(std::string_view bytes, std::string label) {
    Cab c;
    Impl &impl = *c.impl_;
    impl.label = std::move(label);
    impl.sys.base = {sys_open, sys_close, sys_read,  sys_write, sys_seek,
                     sys_tell, sys_message, sys_alloc, sys_free,  sys_copy, nullptr};
    // Пустой bytes означает «архив на диске»: тогда имя не попадает в карту и
    // sys_open откроет настоящий файл. Различие только здесь, дальше пути
    // сходятся.
    if (bytes.data() != nullptr) {
        impl.sys.memory.emplace(impl.label, bytes);
    }

    impl.decomp = mspack_create_cab_decompressor(&impl.sys.base);
    if (!impl.decomp) {
        return err_internal(fmt::format("cannot create a cabinet decompressor for '{}'", impl.label));
    }
    impl.cab = impl.decomp->open(impl.decomp, impl.label.c_str());
    if (!impl.cab) {
        int code = impl.decomp->last_error(impl.decomp);
        return err_format(fmt::format("'{}': {}", impl.label, cab_error(code)));
    }
    for (mscabd_file *f = impl.cab->files; f; f = f->next) {
        c.entries_.push_back(CabEntry{f->filename ? f->filename : "", f->length});
        impl.by_name.emplace(c.entries_.back().name, f);
    }
    return c;
}

Result<void> Cab::extract(std::string_view name, const std::filesystem::path &dest) const {
    auto it = impl_->by_name.find(std::string(name));
    if (it == impl_->by_name.end()) {
        return err_not_found(fmt::format("'{}' is not in cabinet '{}'", name, impl_->label));
    }
    if (auto r = fs::mkdir_p(dest.parent_path()); !r) {
        return r;
    }
    int code = impl_->decomp->extract(impl_->decomp, it->second, dest.c_str());
    if (code != MSPACK_ERR_OK) {
        return err_format(
            fmt::format("extracting '{}' from '{}': {}", name, impl_->label, cab_error(code)));
    }
    return {};
}

} // namespace cork::archive
