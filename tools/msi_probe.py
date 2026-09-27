#!/usr/bin/env python3
"""Разборщик MSI на Python: эксперимент и эталон для сверки.

Нужен дважды. Сейчас — чтобы узнать, какого объёма должен быть читатель на
C++: какие таблицы вообще встречаются в пакетах Windows SDK, бывают ли
встроенные CAB-потоки, бывают ли файлы, разрезанные между архивами. Потом —
как независимая вторая реализация: тест сверяет список «путь, размер, MD5»,
полученный нашим C++-читателем, с тем, что выдаёт этот скрипт.

Формат: CFBF (составной документ OLE) с таблицами внутри. Разбор описан в
[MS-CFB] и [MS-MSI]; здесь ровно то, что нужно для распаковки файлов.

Запуск:
    tools/msi_probe.py survey <файл.msi>...     сводка по таблицам
    tools/msi_probe.py dump <файл.msi>          то же, что CORK_MSI_DUMP, для сверки
    tools/msi_probe.py cabs <файл.cab>...       методы сжатия и spanning
    tools/msi_probe.py list <файл.msi>          путь, размер, MD5 по каждому файлу
"""

import collections
import struct
import sys

# --- CFBF ---------------------------------------------------------------------

CFBF_MAGIC = b"\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1"
FREESECT = 0xFFFFFFFF
ENDOFCHAIN = 0xFFFFFFFE


class Cfbf:
    def __init__(self, data: bytes):
        if data[:8] != CFBF_MAGIC:
            raise ValueError("не CFBF")
        self.data = data
        (self.sector_shift,) = struct.unpack_from("<H", data, 0x1E)
        (self.mini_sector_shift,) = struct.unpack_from("<H", data, 0x20)
        (self.fat_count,) = struct.unpack_from("<I", data, 0x2C)
        (self.dir_start,) = struct.unpack_from("<I", data, 0x30)
        (self.mini_cutoff,) = struct.unpack_from("<I", data, 0x38)
        (self.mini_fat_start,) = struct.unpack_from("<I", data, 0x3C)
        (self.mini_fat_count,) = struct.unpack_from("<I", data, 0x40)
        (self.difat_start,) = struct.unpack_from("<I", data, 0x44)
        (self.difat_count,) = struct.unpack_from("<I", data, 0x48)
        self.sector_size = 1 << self.sector_shift
        self.mini_sector_size = 1 << self.mini_sector_shift
        self._read_fat()
        self._read_directory()
        self._read_mini_stream()

    def _sector(self, index: int) -> bytes:
        # Заголовок занимает ровно один сектор, а не всегда 512 байт: в
        # версии 4 сектор равен 4096, и сектор N начинается с (N+1)*размер.
        # Формула «512 + N*размер» верна только для версии 3 и на версии 4
        # даёт мусор, который выглядит как зацикленная цепочка.
        start = (index + 1) * self.sector_size
        return self.data[start : start + self.sector_size]

    def _read_fat(self):
        # DIFAT: первые 109 записей в заголовке, остальные — цепочкой секторов.
        difat = list(struct.unpack_from("<109I", self.data, 0x4C))
        sector = self.difat_start
        for _ in range(self.difat_count):
            if sector in (ENDOFCHAIN, FREESECT):
                break
            raw = self._sector(sector)
            entries = struct.unpack(f"<{self.sector_size // 4}I", raw)
            difat.extend(entries[:-1])
            sector = entries[-1]

        self.fat = []
        for s in difat[: self.fat_count]:
            if s in (ENDOFCHAIN, FREESECT):
                continue
            raw = self._sector(s)
            self.fat.extend(struct.unpack(f"<{self.sector_size // 4}I", raw))

        self.mini_fat = []
        sector = self.mini_fat_start
        for _ in range(self.mini_fat_count):
            if sector in (ENDOFCHAIN, FREESECT):
                break
            raw = self._sector(sector)
            self.mini_fat.extend(struct.unpack(f"<{self.sector_size // 4}I", raw))
            sector = self.fat[sector]

    def _chain(self, start: int, fat) -> list:
        out, cur, seen = [], start, set()
        while cur not in (ENDOFCHAIN, FREESECT) and cur < len(fat):
            if cur in seen:
                raise ValueError("зацикленная цепочка секторов")
            seen.add(cur)
            out.append(cur)
            cur = fat[cur]
        return out

    def _read_directory(self):
        self.entries = []
        for sector in self._chain(self.dir_start, self.fat):
            raw = self._sector(sector)
            for off in range(0, len(raw), 128):
                e = raw[off : off + 128]
                if len(e) < 128:
                    break
                (name_len,) = struct.unpack_from("<H", e, 0x40)
                name = e[: max(0, name_len - 2)].decode("utf-16-le", "replace")
                kind = e[0x42]
                (start,) = struct.unpack_from("<I", e, 0x74)
                (size,) = struct.unpack_from("<Q", e, 0x78)
                self.entries.append({"name": name, "type": kind, "start": start, "size": size})

    def _read_mini_stream(self):
        root = next((e for e in self.entries if e["type"] == 5), None)
        self.mini_stream = b""
        if root is None or root["size"] == 0:
            return
        parts = [self._sector(s) for s in self._chain(root["start"], self.fat)]
        self.mini_stream = b"".join(parts)[: root["size"]]

    def stream(self, entry) -> bytes:
        if entry["size"] < self.mini_cutoff:
            parts = []
            for s in self._chain(entry["start"], self.mini_fat):
                start = s * self.mini_sector_size
                parts.append(self.mini_stream[start : start + self.mini_sector_size])
            return b"".join(parts)[: entry["size"]]
        parts = [self._sector(s) for s in self._chain(entry["start"], self.fat)]
        return b"".join(parts)[: entry["size"]]


# --- имена потоков MSI --------------------------------------------------------

# Имена закодированы по основанию 64 в диапазоне 0x3800..0x4840.
# Алфавит именно в этом порядке: цифры, прописные, строчные, точка,
# подчёркивание. Любая другая перестановка даёт правдоподобные имена из
# тех же букв — "Property" превращается в "z1YZO138" — и ошибка выглядит
# как повреждённый файл, а не как опечатка в таблице.
ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._"


def demangle(name: str) -> tuple[str, bool]:
    """Возвращает (имя, это_таблица)."""
    out = []
    is_table = False
    for ch in name:
        c = ord(ch)
        if c == 0x4840:
            # Префиксный маркер "дальше имя таблицы". Он не кодирует символ,
            # и если оставить его в строке, имя не совпадёт ни с чем.
            is_table = True
            continue
        if 0x3800 <= c < 0x4800:
            c -= 0x3800
            # Обе половины значащие всегда: нулевой индекс — это цифра "0",
            # законная в именах вроде "Win10". Пропуск второй половины при
            # нулевом старшем шестибитнике молча теряет её.
            out.append(ALPHABET[c & 0x3F])
            out.append(ALPHABET[(c >> 6) & 0x3F])
        elif 0x4800 <= c < 0x4840:
            out.append(ALPHABET[c - 0x4800])
        else:
            out.append(ch)
    return "".join(out), is_table


# --- таблицы MSI --------------------------------------------------------------

COL_STRING = 0x0800
COL_NULLABLE = 0x1000


class Msi:
    def __init__(self, data: bytes):
        self.cfbf = Cfbf(data)
        self.streams = {}
        self.table_streams = set()
        for e in self.cfbf.entries:
            if e["type"] != 2:
                continue
            name, is_table = demangle(e["name"])
            self.streams[name] = e
            if is_table:
                self.table_streams.add(name)
        self._read_string_pool()
        self._read_schema()

    def _raw(self, name: str) -> bytes:
        e = self.streams.get(name)
        return self.cfbf.stream(e) if e else b""

    def _read_string_pool(self):
        info = self._raw("_StringPool")
        data = self._raw("_StringData")
        # Ловушка, на которой легко ошибиться: в паре идёт СНАЧАЛА длина,
        # ПОТОМ счётчик ссылок. Перепутав их, получаешь правдоподобный, но
        # полностью мусорный индекс.
        (codepage_len, codepage_refs) = struct.unpack_from("<HH", info, 0)
        self.long_refs = bool(codepage_refs & 0x8000)
        self.strings = [""]
        offset = 0
        i = 1
        while i * 4 < len(info):
            length, refs = struct.unpack_from("<HH", info, i * 4)
            if length == 0 and refs != 0:
                # Строка длиннее 65535 байт занимает две ячейки. Первая —
                # только маркер, её поле ссылок хранит счётчик ссылок. Длина
                # целиком во второй: младшие 16 бит в поле длины, старшие — в
                # поле ссылок. Взяв старшие из первой ячейки, получаешь
                # правдоподобный, но полностью съехавший пул.
                i += 1
                if i * 4 + 4 > len(info):
                    break
                low, high = struct.unpack_from("<HH", info, i * 4)
                length = (high << 16) | low
            self.strings.append(data[offset : offset + length].decode("utf-8", "replace"))
            offset += length
            i += 1

    def _string_ref_size(self) -> int:
        return 3 if self.long_refs else 2

    def _read_schema(self):
        self.schema = {}
        columns = self._decode_table(
            "_Columns", [0x9D00, 0x9502, 0x9D00 | COL_STRING, 0x9502], raw_names=True
        )
        tables = {}
        for row in columns:
            table = self.strings[row[0]] if isinstance(row[0], int) else row[0]
            number = row[1]
            name = self.strings[row[2]] if isinstance(row[2], int) else row[2]
            ctype = row[3]
            tables.setdefault(table, []).append((number, name, ctype))
        for table, cols in tables.items():
            cols.sort()
            self.schema[table] = [(name, ctype) for _, name, ctype in cols]

    def _col_width(self, ctype: int) -> int:
        if ctype & COL_STRING:
            return self._string_ref_size()
        # Младшие биты несут размер для чисел: 2 или 4 байта.
        return 2 if (ctype & 0xFF) == 2 else 4

    def _decode_table(self, name: str, ctypes, raw_names=False):
        raw = self._raw(name)
        if not raw:
            return []
        widths = [self._col_width(c) for c in ctypes]
        row_size = sum(widths)
        if row_size == 0:
            return []
        rows = len(raw) // row_size
        # Данные хранятся ПО СТОЛБЦАМ, а не по строкам.
        out = [[] for _ in range(rows)]
        pos = 0
        for width, ctype in zip(widths, ctypes):
            for r in range(rows):
                chunk = raw[pos : pos + width]
                pos += width
                value = int.from_bytes(chunk, "little")
                if not (ctype & COL_STRING):
                    # Числа хранятся со сдвинутым знаковым битом.
                    if width == 2:
                        value = value - 0x8000 if value else 0
                    else:
                        value = value - 0x80000000 if value else 0
                out[r].append(value)
        return out

    def table(self, name: str):
        cols = self.schema.get(name)
        if cols is None:
            return None
        ctypes = [c for _, c in cols]
        rows = self._decode_table(name, ctypes)
        names = [n for n, _ in cols]
        result = []
        for row in rows:
            record = {}
            for (col_name, ctype), value in zip(cols, row):
                if ctype & COL_STRING:
                    record[col_name] = self.strings[value] if value < len(self.strings) else ""
                else:
                    record[col_name] = value
            result.append(record)
        return result

    def table_names(self):
        return sorted(self.schema.keys())


# --- CAB ----------------------------------------------------------------------

# Флаги CFHEADER. Нас интересуют первые два: они означают, что папка (folder)
# продолжается в соседнем архиве, то есть один файл физически разрезан между
# двумя CAB. Если такое встречается, читателю мало распаковать каждый архив
# по отдельности — придётся склеивать поток данных через границу.
CAB_PREV_CABINET = 0x0001
CAB_NEXT_CABINET = 0x0002
CAB_RESERVE_PRESENT = 0x0004

COMPRESSION = {0: "None", 1: "MSZIP", 2: "Quantum", 3: "LZX"}


def cab_header(data: bytes) -> dict:
    if data[:4] != b"MSCF":
        raise ValueError("не CAB")
    (
        _res1, size, _res2, files_off, _res3,
        ver_minor, ver_major, folders, files, flags, set_id, cab_index,
    ) = struct.unpack_from("<IIIIIBBHHHHH", data, 4)

    offset = 36
    folder_reserve = 0
    if flags & CAB_RESERVE_PRESENT:
        (header_reserve, folder_reserve, _data_reserve) = struct.unpack_from("<HBB", data, offset)
        offset += 4 + header_reserve
    if flags & CAB_PREV_CABINET:
        for _ in range(2):  # szCabinetPrev, szDiskPrev
            offset = data.index(b"\0", offset) + 1
    if flags & CAB_NEXT_CABINET:
        for _ in range(2):  # szCabinetNext, szDiskNext
            offset = data.index(b"\0", offset) + 1

    methods = collections.Counter()
    for i in range(folders):
        (_data_off, _blocks, comp) = struct.unpack_from("<IHH", data, offset + i * (8 + folder_reserve))
        methods[COMPRESSION.get(comp & 0x0F, f"0x{comp:x}")] += 1

    return {
        "size": size,
        "version": f"{ver_major}.{ver_minor}",
        "folders": folders,
        "files": files,
        "flags": flags,
        "prev": bool(flags & CAB_PREV_CABINET),
        "next": bool(flags & CAB_NEXT_CABINET),
        "set_id": set_id,
        "cab_index": cab_index,
        "methods": methods,
        "files_off": files_off,
    }


# --- команды ------------------------------------------------------------------


def survey(paths):
    tables = collections.Counter()
    attrs = collections.Counter()
    cab_kinds = collections.Counter()
    media_hist = collections.Counter()
    multi_media_with_spread = []
    noncompressed = []
    embedded_names = []
    failures = []

    for path in paths:
        try:
            msi = Msi(open(path, "rb").read())
        except Exception as exc:  # noqa: BLE001
            failures.append((path, repr(exc)))
            continue
        for t in msi.table_names():
            tables[t] += 1

        files = msi.table("File") or []
        nc = [f for f in files if (f.get("Attributes", 0) or 0) & 0x2000]
        for f in files:
            a = f.get("Attributes", 0) or 0
            if a & 0x2000:
                attrs["msidbFileAttributesNoncompressed"] += 1
            if a & 0x1:
                attrs["ReadOnly"] += 1
        if nc:
            noncompressed.append((path, len(nc), [f.get("FileName", "") for f in nc[:4]]))

        media = msi.table("Media") or []
        media_hist[len(media)] += 1
        for m in media:
            cab = m.get("Cabinet", "") or ""
            if cab.startswith("#"):
                cab_kinds["встроенный"] += 1
                embedded_names.append((path, cab, cab[1:] in msi.streams))
            elif cab:
                cab_kinds["внешний"] += 1
            else:
                cab_kinds["без архива"] += 1

        # Раскладываем Sequence файлов по дискам: непустых дисков больше
        # одного — значит файлы пакета действительно лежат в разных архивах,
        # а не просто объявлено лишнее Media.
        bounds = sorted((m.get("LastSequence", 0) or 0, m.get("Cabinet", "") or "") for m in media)
        used = set()
        for f in files:
            seq = f.get("Sequence", 0) or 0
            for last, cab in bounds:
                if seq <= last:
                    used.add(cab)
                    break
        if len(used) > 1:
            multi_media_with_spread.append((path, sorted(used)))

    print(f"разобрано: {len(paths) - len(failures)} из {len(paths)}")
    if failures:
        print("\nне разобрались:")
        for path, why in failures[:5]:
            print(f"  {path}: {why}")

    print("\nархивы по видам:")
    for k, v in cab_kinds.most_common():
        print(f"  {v:6d}  {k}")
    print("\nсколько Media в пакете:")
    for k, v in sorted(media_hist.items()):
        print(f"  {v:6d} пакетов с {k} Media")
    print(f"\nпакетов, где файлы разложены по нескольким архивам: {len(multi_media_with_spread)}")
    for path, used in multi_media_with_spread[:5]:
        print(f"  {path.rsplit('/', 1)[-1]}: {used}")

    print(f"\nвстроенных CAB-потоков: {len(embedded_names)}")
    missing = [e for e in embedded_names if not e[2]]
    print(f"  из них не нашлись в CFBF: {len(missing)}")
    for path, cab, _ in embedded_names[:3]:
        print(f"  {path.rsplit('/', 1)[-1]}: {cab}")

    print(f"\nпакетов с noncompressed-файлами: {len(noncompressed)}")
    for path, n, names in noncompressed[:8]:
        print(f"  {path.rsplit('/', 1)[-1]}: {n} шт, например {names}")

    print("\nатрибуты File:")
    for k, v in attrs.most_common():
        print(f"  {v:6d}  {k}")
    print("\nтаблицы (в скольких пакетах встречаются):")
    for k, v in tables.most_common():
        print(f"  {v:4d}  {k}")


def survey_cabs(paths):
    """Эксперимент 13: какими методами сжаты настоящие CAB и есть ли spanning."""
    methods = collections.Counter()
    versions = collections.Counter()
    spanning = []
    failures = []
    folders_hist = collections.Counter()

    for path in paths:
        try:
            h = cab_header(open(path, "rb").read())
        except Exception as exc:  # noqa: BLE001
            failures.append((path, repr(exc)))
            continue
        methods.update(h["methods"])
        versions[h["version"]] += 1
        folders_hist[h["folders"]] += 1
        if h["prev"] or h["next"]:
            spanning.append((path, h["prev"], h["next"]))

    print(f"разобрано: {len(paths) - len(failures)} из {len(paths)}")
    for path, why in failures[:5]:
        print(f"  {path}: {why}")
    print("\nметоды сжатия (по папкам):")
    for k, v in methods.most_common():
        print(f"  {v:6d}  {k}")
    print("\nверсии формата:")
    for k, v in versions.most_common():
        print(f"  {v:6d}  {k}")
    print("\nпапок в архиве:")
    for k, v in sorted(folders_hist.items()):
        print(f"  {v:6d} архивов с {k} папками")
    print(f"\nархивов с продолжением в соседнем (spanning): {len(spanning)}")
    for path, prev, nxt in spanning[:5]:
        print(f"  {path.rsplit('/', 1)[-1]}: prev={prev} next={nxt}")


def list_files(path):
    msi = Msi(open(path, "rb").read())
    dirs = {d["Directory"]: d for d in (msi.table("Directory") or [])}
    comps = {c["Component"]: c for c in (msi.table("Component") or [])}

    def resolve(directory, depth=0):
        if depth > 32 or directory not in dirs:
            return ""
        d = dirs[directory]
        name = d.get("DefaultDir", "") or ""
        # Форма «short|long»: берём длинное имя.
        if "|" in name:
            name = name.split("|", 1)[1]
        if name == ".":
            name = ""
        parent = d.get("Directory_Parent", "") or ""
        if not parent or parent == directory:
            return name
        head = resolve(parent, depth + 1)
        return f"{head}/{name}".strip("/") if name else head

    for f in msi.table("File") or []:
        comp = comps.get(f.get("Component_", ""), {})
        path_dir = resolve(comp.get("Directory_", ""))
        name = f.get("FileName", "")
        if "|" in name:
            name = name.split("|", 1)[1]
        print(f"{f.get('Sequence', 0):6d}  {f.get('FileSize', 0):10d}  {path_dir}/{name}")


def dump(path):
    """Тот же формат, что у tests/test_msi (CORK_MSI_DUMP): для сверки."""
    msi = Msi(open(path, "rb").read())
    dirs = {d["Directory"]: d for d in (msi.table("Directory") or [])}
    comps = {c["Component"]: c for c in (msi.table("Component") or [])}
    resolved = {}

    def target(name):
        name = name or ""
        if ":" in name:
            name = name.split(":", 1)[0]
        if "|" in name:
            name = name.split("|", 1)[1]
        return name

    def resolve(directory, depth=0):
        if directory in resolved:
            return resolved[directory]
        if depth > 63 or directory not in dirs:
            return ""
        d = dirs[directory]
        name = target(d.get("DefaultDir", ""))
        if name in (".", "SourceDir"):
            name = ""
        parent = d.get("Directory_Parent", "") or ""
        head = "" if not parent or parent == directory else resolve(parent, depth + 1)
        path = f"{head}/{name}".strip("/") if name else head
        resolved[directory] = path
        return path

    media = sorted(
        ((m.get("LastSequence", 0) or 0, m.get("Cabinet", "") or "")
         for m in (msi.table("Media") or []))
    )
    hashes = {}
    for h in (msi.table("MsiFileHash") or []):
        parts = b"".join(
            (h[f"HashPart{i}"] & 0xFFFFFFFF).to_bytes(4, "little") for i in range(1, 5)
        )
        hashes[h["File_"]] = parts.hex()

    for f in msi.table("File") or []:
        comp = comps.get(f.get("Component_", ""), {})
        directory = resolve(comp.get("Directory_", ""))
        name = target(f.get("FileName", ""))
        seq = f.get("Sequence", 0) or 0
        cab = ""
        for last, c in media:
            if seq <= last:
                cab = c
                break
        full = f"{directory}/{name}" if directory else name
        attrs = f.get("Attributes", 0) or 0
        print(f"{seq}\t{f.get('FileSize', 0) or 0}\t{attrs}\t{cab}\t{hashes.get(f['File'], '-')}\t{full}")


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    if sys.argv[1] == "survey":
        survey(sys.argv[2:])
    elif sys.argv[1] == "dump":
        dump(sys.argv[2])
    elif sys.argv[1] == "cabs":
        survey_cabs(sys.argv[2:])
    elif sys.argv[1] == "list":
        list_files(sys.argv[2])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
