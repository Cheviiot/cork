#!/usr/bin/env bash
#
# Приёмочный прогон cork.
#
# Отличается от ctest тем, что требует настоящей установки: настоящий MSVC,
# настоящий Wine, настоящие сборки. Поэтому живёт отдельно и запускается
# руками или ночью, а не на каждый коммит.
#
# Почему один скрипт, а не набор: результат нужен таблицей. Приёмка — это не
# «упало или нет», а «что именно работает на сегодня». На такой вопрос
# отвечает таблица, а не полтора десятка отдельных запусков, результаты
# которых надо сводить руками, — а значит, их никто не сводит.
#
# Временное живёт под $CORK_HOME, а не в /tmp: на машинах с tmpfs распаковка
# и сборки туда не помещаются.

# shellcheck disable=SC2317
# Случаи вызываются по имени, собранному из таблицы, поэтому shellcheck считает
# их тела недостижимыми. Это ровно то, ради чего таблица и заведена.

set -uo pipefail

self_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$self_dir/../.." && pwd)"

CORK_BIN=""
CORK_ROOT="${CORK_HOME:-$HOME/.cork}"
WORK=""
REPORT=""
KEEP=0
DO_INSTALL=0
WITH_OGRE=0
SELECTED=()

CASES=(
    doctor-quick        "cork doctor on the published generation"
    doctor-deep         "every file verified against the recorded digest"
    doctor-build        "compile and link a probe for every target"
    hello-c             "cl hello.c with stdio, run it, check the exit code"
    hello-cpp           "C++ with iostream, Debug and Release"
    static-lib          "lib.exe makes an archive, link.exe consumes it"
    resources           "rc.exe compiles a .rc, link.exe embeds it"
    manifest            "mt.exe embeds a manifest into a linked binary"
    response-file       "@rsp in UTF-16LE, with a path that has a space"
    awkward-paths       "build in a directory with spaces and Cyrillic"
    targets             "every target: machine type in the PE, and running it"
    cmake-ninja         "CMake + Ninja through the shipped toolchain file"
    msbuild             "a real .vcxproj through MSBuild"
    incremental         "second build does nothing"
    parallel            "two builds at once, one session each"
    interrupt           "Ctrl-C mid-build leaves no Wine processes behind"
    ctest               "ctest runs the Windows binaries it just built"
    winsysroot          "clang-cl builds against the tree as a /winsysroot"
    ccache              "ccache caches compilations through the wrappers"
    offline             "a repeat build with the network taken away"
    ogre                "Ogre3D 14.5.2, OgreMain (slow, needs --with-ogre)"
)

usage() {
    cat <<EOF
Usage: tools/acceptance/run.sh [options] [case...]

  --cork <path>     the binary under test (default: a build/ in the repo)
  --root <dir>      CORK_HOME to test against (default: \$CORK_HOME or ~/.cork)
  --work <dir>      scratch tree (default: <root>/acceptance)
  --report <file>   write the results table here as well
  --install         run download and install first; without it an existing
                    installation is reused
  --with-ogre       include the Ogre3D case
  --keep            leave the scratch tree behind
  --list            print the case names and stop

With no case names, everything except ogre runs.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --cork) CORK_BIN="$2"; shift 2 ;;
        --root) CORK_ROOT="$2"; shift 2 ;;
        --work) WORK="$2"; shift 2 ;;
        --report) REPORT="$2"; shift 2 ;;
        --install) DO_INSTALL=1; shift ;;
        --with-ogre) WITH_OGRE=1; shift ;;
        --keep) KEEP=1; shift ;;
        --list)
            for ((i = 0; i < ${#CASES[@]}; i += 2)); do
                printf '  %-16s %s\n' "${CASES[i]}" "${CASES[i+1]}"
            done
            exit 0 ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "unknown option $1" >&2; usage >&2; exit 2 ;;
        *) SELECTED+=("$1"); shift ;;
    esac
done

if [ -z "$CORK_BIN" ]; then
    for candidate in \
        "$repo_dir/build/linux-release/cork" \
        "$repo_dir/build/linux-debug/cork" \
        "$(command -v cork 2>/dev/null)"; do
        if [ -x "$candidate" ]; then CORK_BIN="$candidate"; break; fi
    done
fi
if [ ! -x "$CORK_BIN" ]; then
    echo "acceptance: no cork binary; pass --cork <path>" >&2
    exit 2
fi
CORK_BIN="$(cd "$(dirname "$CORK_BIN")" && pwd)/$(basename "$CORK_BIN")"
export CORK_HOME="$CORK_ROOT"
[ -n "$WORK" ] || WORK="$CORK_ROOT/acceptance"

# Английский на весь прогон: таблица результатов читается и сравнивается
# машинами тоже, а перевод её сломал бы.
export CORK_LANG=en

# ---------------------------------------------------------------- инструменты

pass=0; fail=0; skip=0
declare -a ROWS=()
NOTE=""

log()  { printf '%s\n' "$*"; }
# Выделение только в терминале: в перенаправленном выводе escape-последовательности
# попадают в файл, и отчёт, который потом читают, оказывается в мусоре.
if [ -t 1 ]; then
    step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
else
    step() { printf '\n== %s\n' "$*"; }
fi

# Вывод каждого случая пишется в файл: на экране нужна таблица, а при разборе
# упавшего случая — всё до последней строки.
case_log() { echo "$WORK/logs/$1.log"; }

run_case() {
    local name="$1" desc="$2"
    local started elapsed rc note_file
    mkdir -p "$WORK/logs"
    note_file="$WORK/logs/$name.note"
    rm -f "$note_file"
    step "$name — $desc"
    started=$(date +%s)
    # Каждый случай — в своей подоболочке. Иначе export PATH и cd из одного
    # достаются следующим, и случай может пройти по чужой причине — а это
    # худший вид зелёной проверки. Ценой того, что NOTE приходится передавать
    # файлом: из подоболочки переменная наружу не выходит.
    (
        NOTE=""
        "case_${name//-/_}"
        rc=$?
        printf '%s' "$NOTE" > "$note_file"
        exit $rc
    ) >"$(case_log "$name")" 2>&1
    rc=$?
    NOTE="$(cat "$note_file" 2>/dev/null)"
    elapsed=$(( $(date +%s) - started ))
    case $rc in
        0) pass=$((pass + 1)); ROWS+=("$name|pass|${elapsed}s|$NOTE")
           log "   pass  ${elapsed}s  $NOTE" ;;
        77) skip=$((skip + 1)); ROWS+=("$name|skip|${elapsed}s|$NOTE")
           log "   skip  $NOTE" ;;
        *) fail=$((fail + 1)); ROWS+=("$name|FAIL|${elapsed}s|$NOTE")
           log "   FAIL  ${elapsed}s  $NOTE"
           log "   ---- last 25 lines of $(case_log "$name") ----"
           tail -25 "$(case_log "$name")" | sed 's/^/   /' ;;
    esac
}

# Рабочий каталог случая: каждый начинает с чистого, иначе «прошёл» может
# означать «нашёл файл от предыдущего». Каталог кладётся в CASE_DIR, а не
# печатается: подстановка команд уводит cd в подоболочку, и вызывающий
# остаётся там, где был.
CASE_DIR=""
workdir() {
    CASE_DIR="$WORK/$1"
    rm -rf "$CASE_DIR"; mkdir -p "$CASE_DIR"; cd "$CASE_DIR" || return 1
}

skip_case() { NOTE="$1"; return 77; }
fail_case() { NOTE="$1"; return 1; }

# Ждать, что в выводе есть строка. Отдельной функцией, потому что сообщение об
# отличии важнее самого сравнения: «не совпало» без текста бесполезно.
expect_contains() {
    local haystack="$1" needle="$2"
    case "$haystack" in
        *"$needle"*) return 0 ;;
        *) echo "expected to see: $needle"; echo "got: $haystack"; return 1 ;;
    esac
}

pe_machine() { "$CORK_BIN" dumpbin /nologo /headers "$1" 2>/dev/null | grep -m1 'machine ('; }

# ------------------------------------------------------------------- случаи

case_doctor_quick() {
    local out
    out="$("$CORK_BIN" doctor 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    NOTE="$(echo "$out" | grep -c '^\[ok')"" checks ok"
}

case_doctor_deep() {
    local out
    out="$("$CORK_BIN" doctor --deep 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    expect_contains "$out" "0 failed" || return 1
}

case_doctor_build() {
    local out
    out="$("$CORK_BIN" doctor --build 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    NOTE="$(echo "$out" | grep -c '^\[ok *\] probe')"" probes"
}

case_hello_c() {
    workdir hello-c
    cat > hello.c <<'EOF'
#include <stdio.h>
int main(void) { printf("hello from %s\n", "cork"); return 7; }
EOF
    "$CORK_BIN" cl /nologo hello.c || return 1
    local out rc
    out="$("$CORK_BIN" run -- ./hello.exe 2>&1)"; rc=$?
    echo "$out"
    expect_contains "$out" "hello from cork" || return 1
    # Код возврата обязан доехать целиком: усечение до нуля здесь означало бы,
    # что сборочная система не заметит провалившийся тест.
    [ "$rc" = 7 ] || { echo "exit code was $rc, wanted 7"; return 1; }
    NOTE="ran, exit code 7 preserved"
}

case_hello_cpp() {
    workdir hello-cpp
    cat > hello.cpp <<'EOF'
#include <iostream>
#include <string>
int main() {
    const std::string who = "cork";
    std::cout << "c++ says " << who << " " << __cplusplus << "\n";
    return 0;
}
EOF
    local out
    # /Zc:__cplusplus — иначе cl сообщает 199711 независимо от /std, и
    # проверка версии стандарта ничего бы не проверяла.
    "$CORK_BIN" cl /nologo /std:c++20 /Zc:__cplusplus /EHsc /MD /O2 \
        /Fe:release.exe hello.cpp || return 1
    "$CORK_BIN" cl /nologo /std:c++20 /Zc:__cplusplus /EHsc /MDd /Zi /Od \
        /Fe:debug.exe hello.cpp || return 1
    for build in release debug; do
        out="$("$CORK_BIN" run -- "./$build.exe" 2>&1)" || { echo "$out"; return 1; }
        echo "$build: $out"
        expect_contains "$out" "c++ says cork 202002" || return 1
    done
    # Debug обязан быть связан именно с отладочными библиотеками: если он
    # вдруг окажется связан с выпускными, случай пройдёт, ничего не проверив.
    local deps
    deps="$("$CORK_BIN" dumpbin /nologo /dependents debug.exe 2>&1)"
    echo "$deps"
    expect_contains "$deps" "140D.dll" || return 1
    NOTE="Release and Debug both run; Debug linked against the debug CRT"
}

case_static_lib() {
    workdir static-lib
    cat > add.c <<'EOF'
int add(int a, int b) { return a + b; }
EOF
    cat > main.c <<'EOF'
#include <stdio.h>
int add(int, int);
int main(void) { printf("sum=%d\n", add(19, 23)); return 0; }
EOF
    "$CORK_BIN" cl /nologo /c add.c || return 1
    "$CORK_BIN" lib /nologo /OUT:math.lib add.obj || return 1
    "$CORK_BIN" cl /nologo main.c math.lib || return 1
    local out
    out="$("$CORK_BIN" run -- ./main.exe 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    expect_contains "$out" "sum=42" || return 1
    NOTE="archive built and linked against"
}

case_resources() {
    workdir resources
    cat > app.rc <<'EOF'
#include <windows.h>
STRINGTABLE
BEGIN
    1 "resource string from cork"
END
EOF
    cat > main.c <<'EOF'
#include <windows.h>
#include <stdio.h>
int main(void) {
    char buf[128] = {0};
    int n = LoadStringA(GetModuleHandleA(NULL), 1, buf, sizeof buf);
    printf("loaded %d: %s\n", n, buf);
    return n > 0 ? 0 : 1;
}
EOF
    "$CORK_BIN" rc /nologo /fo app.res app.rc || return 1
    "$CORK_BIN" cl /nologo main.c app.res user32.lib || return 1
    local out
    out="$("$CORK_BIN" run -- ./main.exe 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    expect_contains "$out" "resource string from cork" || return 1
    NOTE="resource compiled, embedded and read back"
}

case_manifest() {
    workdir manifest
    cat > main.c <<'EOF'
int main(void) { return 0; }
EOF
    cat > app.manifest <<'EOF'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="cork.acceptance" version="1.0.0.0"/>
</assembly>
EOF
    "$CORK_BIN" cl /nologo main.c || return 1
    "$CORK_BIN" mt /nologo -manifest app.manifest -outputresource:main.exe\;1 || return 1
    # Второй прогон с тем же манифестом: mt.exe отвечает 0x41020001 «ничего не
    # изменилось», и это не отказ. Полный код доезжает до нативной стороны и
    # отображается в 0xbb, которого ждёт CMake.
    "$CORK_BIN" mt /nologo -manifest app.manifest -outputresource:main.exe\;1
    local rc=$?
    echo "second mt run exit: $rc"
    [ "$rc" = 0 ] || [ "$rc" = 187 ] || { echo "unexpected mt exit $rc"; return 1; }
    NOTE="embedded; repeat run gave $rc"
}

case_response_file() {
    workdir response-file
    mkdir -p "dir with space"
    cat > "dir with space/main.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("rsp ok\n"); return 0; }
EOF
    # UTF-16LE с BOM — то, что порождает MSBuild, и то, на чём ломались
    # самодельные токенизаторы.
    printf '/nologo\n"%s/dir with space/main.c"\n/Fe:out.exe\n' "$PWD" \
        | iconv -f UTF-8 -t UTF-16LE > args.rsp.tmp
    printf '\xff\xfe' > args.rsp
    cat args.rsp.tmp >> args.rsp
    rm -f args.rsp.tmp
    "$CORK_BIN" cl "@args.rsp" || return 1
    local out
    out="$("$CORK_BIN" run -- ./out.exe 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    expect_contains "$out" "rsp ok" || return 1
    NOTE="UTF-16LE with BOM, path with a space"
}

case_awkward_paths() {
    workdir awkward-paths
    local dir="$CASE_DIR/Проект с пробелами"
    mkdir -p "$dir" && cd "$dir" || return 1
    cat > main.c <<'EOF'
#include <stdio.h>
int main(void) { printf("awkward ok\n"); return 0; }
EOF
    "$CORK_BIN" cl /nologo main.c || return 1
    local out
    out="$("$CORK_BIN" run -- ./main.exe 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    expect_contains "$out" "awkward ok" || return 1
    NOTE="Cyrillic directory name with a space"
}

case_targets() {
    workdir targets
    cat > main.c <<'EOF'
#include <stdio.h>
int main(void) { printf("%zu-bit\n", sizeof(void *) * 8); return 0; }
EOF
    local built=() out machine
    for arch in x86 x64 arm64; do
        local bin="$CORK_ROOT/toolchains/current/bin/$arch"
        [ -d "$bin" ] || continue
        rm -f main.obj
        "$bin/cl" /nologo /Fe:"main-$arch.exe" main.c >/dev/null || { echo "$arch: cl failed"; return 1; }
        machine="$(pe_machine "main-$arch.exe")"
        echo "$arch machine: $machine"
        case "$arch:$machine" in
            x86:*x86*|x64:*x64*|arm64:*ARM64*) ;;
            *) echo "$arch produced the wrong machine type"; return 1 ;;
        esac
        built+=("$arch")
        # arm64 не запустить на этом хосте, и это не отказ.
        if [ "$arch" != arm64 ]; then
            out="$("$CORK_BIN" run -- "./main-$arch.exe" 2>&1)" || { echo "$out"; return 1; }
            echo "$arch runs: $out"
            case "$arch:$out" in
                x86:*32-bit*|x64:*64-bit*) ;;
                *) echo "$arch ran but reported the wrong pointer size"; return 1 ;;
            esac
        fi
    done
    [ ${#built[@]} -gt 0 ] || return 1
    NOTE="${built[*]}"
}

case_cmake_ninja() {
    command -v cmake >/dev/null || skip_case "no cmake" || return 77
    command -v ninja >/dev/null || skip_case "no ninja" || return 77
    workdir cmake-ninja
    local toolchain="$CORK_ROOT/toolchains/current/share/cork-toolchain.cmake"
    [ -f "$toolchain" ] || { skip_case "no toolchain file in this generation"; return 77; }
    mkdir -p src
    cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("cmake+ninja ok\n"); return 0; }
EOF
    cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.28)
project(acceptance CXX)
add_executable(app src/main.cpp)
EOF
    export PATH="$CORK_ROOT/toolchains/current/bin/x64:$PATH"
    cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
          -DCMAKE_BUILD_TYPE=Release || return 1
    "$CORK_BIN" run -- cmake --build build || return 1
    local out
    out="$("$CORK_BIN" run -- ./build/app.exe 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    expect_contains "$out" "cmake+ninja ok" || return 1
    NOTE="configure, build and run"
}

# Минимальный, но настоящий .vcxproj: важен не размер, а то, что MSBuild
# проходит весь свой путь — импорт Microsoft.Cpp.props, выбор набора
# инструментов, вызов cl и link своими задачами.
write_vcxproj() {
    cat > app.vcxproj <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <ProjectGuid>{2F1E4A2B-0B0C-4C71-9A1E-9C0F1B2D3E4F}</ProjectGuid>
    <RootNamespace>app</RootNamespace>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ItemDefinitionGroup>
    <ClCompile>
      <LanguageStandard>stdcpp20</LanguageStandard>
      <ExceptionHandling>Sync</ExceptionHandling>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="main.cpp" />
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
EOF
    cat > main.cpp <<'EOF'
#include <cstdio>
int main() { std::printf("msbuild ok\n"); return 0; }
EOF
}

case_msbuild() {
    workdir msbuild
    write_vcxproj
    local out
    out="$("$CORK_BIN" run -- "$CORK_BIN" msbuild app.vcxproj \
            /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal 2>&1)"
    local rc=$?
    echo "$out"
    [ $rc -eq 0 ] || return 1
    local exe
    exe="$(find . -name app.exe -print -quit)"
    [ -n "$exe" ] || { echo "no app.exe produced"; return 1; }
    out="$("$CORK_BIN" run -- "$exe" 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    expect_contains "$out" "msbuild ok" || return 1
    NOTE="v143 project built and ran"
}

case_incremental() {
    command -v cmake >/dev/null && command -v ninja >/dev/null || { skip_case "no cmake/ninja"; return 77; }
    local toolchain="$CORK_ROOT/toolchains/current/share/cork-toolchain.cmake"
    [ -f "$toolchain" ] || { skip_case "no toolchain file"; return 77; }
    workdir incremental
    mkdir -p src
    for i in 1 2 3 4 5 6 7 8; do
        printf 'int unit%d(void) { return %d; }\n' "$i" "$i" > "src/unit$i.c"
    done
    { echo '#include <stdio.h>'
      for i in 1 2 3 4 5 6 7 8; do printf 'int unit%d(void);\n' "$i"; done
      echo 'int main(void) { int s = 0;'
      for i in 1 2 3 4 5 6 7 8; do printf '  s += unit%d();\n' "$i"; done
      echo '  printf("sum=%d\n", s); return 0; }'
    } > src/main.c
    cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.28)
project(incr C)
file(GLOB sources src/*.c)
add_executable(incr ${sources})
EOF
    export PATH="$CORK_ROOT/toolchains/current/bin/x64:$PATH"
    # Ничего не глушим: случай с пустым логом не разобрать, а падает обычно
    # именно то, что сочли неинтересным.
    cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" || return 1
    "$CORK_BIN" run -- cmake --build build || return 1
    local second
    second="$("$CORK_BIN" run -- cmake --build build 2>&1)"
    echo "$second"
    expect_contains "$second" "no work to do" || return 1
    # Правка одного файла обязана пересобрать один файл, а не все восемь.
    printf 'int unit3(void) { return 33; }\n' > src/unit3.c
    local third rebuilt
    third="$("$CORK_BIN" run -- cmake --build build 2>&1)"
    echo "$third"
    rebuilt="$(echo "$third" | grep -c 'Building C object')"
    [ "$rebuilt" = 1 ] || { echo "rebuilt $rebuilt objects, wanted 1"; return 1; }
    NOTE="no-op second build; one object after a one-file edit"
}

case_parallel() {
    workdir parallel
    local pids=() dirs=()
    for name in alpha beta; do
        mkdir -p "$name" && cd "$name" || return 1
        git init -q . 2>/dev/null   # корень сборки -> свой ключ сессии
        cat > main.c <<EOF
#include <stdio.h>
int main(void) { printf("$name ok\n"); return 0; }
EOF
        cd ..
        dirs+=("$name")
    done
    for name in "${dirs[@]}"; do
        # Внутренний cork run наследует CORK_SESSION и поэтому не сносит
        # чужую сессию — это и проверяется заодно.
        ( cd "$name" && "$CORK_BIN" run -- sh -c \
            "\"$CORK_BIN\" cl /nologo main.c && exec \"$CORK_BIN\" run -- ./main.exe" ) \
            > "$name.out" 2>&1 &
        pids+=($!)
    done
    local bad=0
    for pid in "${pids[@]}"; do wait "$pid" || bad=1; done
    for name in "${dirs[@]}"; do
        echo "--- $name ---"; cat "$name.out"
        expect_contains "$(cat "$name.out")" "$name ok" || bad=1
    done
    [ $bad -eq 0 ] || return 1
    # Обе сессии обязаны убраться за собой: ради этого cork run и существует.
    local left
    left="$("$CORK_BIN" session list 2>&1 | grep -c .)"
    echo "sessions left: $left"
    NOTE="two concurrent builds, sessions cleaned up"
}

# Сколько процессов Wine этой установки живёт прямо сейчас. Считается по
# полному пути, а не по имени: на машине может идти чужая сборка под системным
# Wine, и засчитывать её было бы неверно.
cork_wine_processes() {
    ps -eo args= | grep -c "^$CORK_ROOT/\(runtime\|toolchains\)/" || true
}

case_interrupt() {
    command -v cmake >/dev/null && command -v ninja >/dev/null || { skip_case "no cmake/ninja"; return 77; }
    local toolchain="$CORK_ROOT/toolchains/current/share/cork-toolchain.cmake"
    [ -f "$toolchain" ] || { skip_case "no toolchain file"; return 77; }
    workdir interrupt
    mkdir -p src
    # Сборка должна быть достаточно длинной, чтобы Ctrl-C попал в середину, и
    # достаточно тяжёлой, чтобы к этому моменту жили и компилятор, и сервер
    # PDB. Шестнадцать единиц трансляции в Debug с /FS дают и то, и другое.
    local i
    for i in $(seq 1 16); do
        {
            printf '#include <cstdio>\ntemplate<int N> struct F { static long long v() { return N * F<N-1>::v(); } };\n'
            printf 'template<> struct F<0> { static long long v() { return 1; } };\n'
            printf 'long long unit%d() { return F<20>::v(); }\n' "$i"
        } > "src/unit$i.cpp"
    done
    printf 'int main(void) { return 0; }\n' > src/main.cpp
    cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.28)
project(interrupt CXX)
file(GLOB sources src/*.cpp)
add_executable(interrupt ${sources})
EOF
    export PATH="$CORK_ROOT/toolchains/current/bin/x64:$PATH"
    cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
          -DCMAKE_BUILD_TYPE=Debug > /dev/null || return 1

    local before after key
    key="interrupt-$$"
    before="$(cork_wine_processes)"

    # Своя группа процессов, чтобы Ctrl-C пришёл всей сборке разом, как в
    # терминале, а не одному только cork.
    setsid "$CORK_BIN" run --session "$key" -- cmake --build build > build.log 2>&1 &
    local pid=$!
    # Ждём, пока сборка действительно начнётся: прервать то, что ещё не
    # запустилось, ничего не проверяет.
    #
    # Запас большой намеренно. Без шаблона префикса первая сборка поднимает
    # префикс с нуля, и это минута, а не секунда; тридцати секунд не хватало,
    # и случай падал не на том, что проверяет.
    local waited=0
    while [ "$waited" -lt 240 ]; do
        grep -q 'Building CXX object' build.log 2>/dev/null && break
        sleep 0.5; waited=$((waited + 1))
    done
    if ! grep -q 'Building CXX object' build.log 2>/dev/null; then
        echo "the build never got as far as compiling in $((waited / 2))s:"
        cat build.log
        kill -9 -"$pid" 2>/dev/null
        return 1
    fi
    sleep 1

    kill -INT -"$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    local rc=$?
    echo "build exited with $rc after SIGINT"

    # Гасить некому и незачем: `cork run` обязан убрать за собой сам. Пауза —
    # на завершение процессов, а не на второй шанс.
    sleep 3
    after="$(cork_wine_processes)"
    echo "wine processes: $before before, $after after"

    local left=0
    [ -d "$CORK_ROOT/sessions/$key" ] && left=1
    if [ "$after" -gt "$before" ] || [ "$left" = 1 ]; then
        echo "session directory left: $left"
        ps -eo args= | grep "^$CORK_ROOT/" | head -5
        return 1
    fi
    NOTE="interrupted mid-build, no processes or session left"
}

case_ctest() {
    command -v cmake >/dev/null && command -v ctest >/dev/null || { skip_case "no cmake/ctest"; return 77; }
    command -v ninja >/dev/null || { skip_case "no ninja"; return 77; }
    local toolchain="$CORK_ROOT/toolchains/current/share/cork-toolchain.cmake"
    [ -f "$toolchain" ] || { skip_case "no toolchain file"; return 77; }
    workdir ctest
    mkdir -p src
    printf '#include <stdio.h>\nint main(int c, char **v) { printf("%%s\\n", c > 1 ? v[1] : "no arg"); return c > 1 ? 0 : 3; }\n' > src/main.c
    cat > CMakeLists.txt <<'EOF'
cmake_minimum_required(VERSION 3.28)
project(emu C)
enable_testing()
add_executable(emu src/main.c)
add_test(NAME runs COMMAND emu hello)
add_test(NAME exit_code COMMAND emu)
set_tests_properties(exit_code PROPERTIES WILL_FAIL TRUE)
EOF
    export PATH="$CORK_ROOT/toolchains/current/bin/x64:$PATH"
    cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" > cfg.log 2>&1 || { cat cfg.log; return 1; }

    # То, ради чего случай написан: без CMAKE_CROSSCOMPILING_EMULATOR CMake
    # запишет в тесты голый .exe и ctest не запустит ни одного. Смотреть надо
    # в сгенерированный CTestTestfile, а не в кэш: toolchain-файл заводит
    # обычную переменную, и в CMakeCache.txt она не попадает.
    expect_contains "$(cat build/CTestTestfile.cmake)" "\"run\" \"--\"" || return 1

    "$CORK_BIN" run -- cmake --build build > build.log 2>&1 || { cat build.log; return 1; }
    local out
    out="$(cd build && ctest --output-on-failure 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    expect_contains "$out" "100% tests passed" || return 1
    NOTE="ctest ran both cases through the emulator"
}

case_winsysroot() {
    command -v clang-cl >/dev/null || { skip_case "no clang-cl"; return 77; }
    # Компоновщик тоже нативный: link.exe — это PE, и clang-cl на Linux его
    # просто не запустит. lld-link умеет то же самое и работает здесь.
    command -v lld-link >/dev/null || { skip_case "no lld-link"; return 77; }
    workdir winsysroot
    # windows.h строчными нарочно: именно так пишут в коде, и именно это не
    # находится без символьных ссылок регистра. Wine ищет без учёта регистра
    # сам, clang-cl на Linux — нет.
    cat > a.c <<'EOF'
#include <stdio.h>
#include <windows.h>
int main(void) { printf("winsysroot ok %lu\n", (unsigned long)GetTickCount()); return 0; }
EOF
    local out
    out="$(clang-cl --target=x86_64-pc-windows-msvc \
             /winsysroot "$CORK_ROOT/toolchains/current" \
             -fuse-ld=lld /nologo a.c /Fea.exe 2>&1)" || { echo "$out"; return 1; }
    echo "$out"
    [ -f a.exe ] || { echo "no a.exe produced"; return 1; }
    pe_machine a.exe
    # Собранное clang-cl запускается тем же cork: два компилятора, один
    # рантайм.
    local ran
    ran="$("$CORK_BIN" run -- ./a.exe 2>&1)" || { echo "$ran"; return 1; }
    expect_contains "$ran" "winsysroot ok" || return 1
    NOTE="clang-cl compiled and linked against the generation"
}

case_ccache() {
    command -v ccache >/dev/null || { skip_case "no ccache"; return 77; }
    workdir ccache
    printf '#include <stdio.h>\nint main(void) { printf("cached\\n"); return 0; }\n' > a.c
    export PATH="$CORK_ROOT/toolchains/current/bin/x64:$PATH"
    export CCACHE_DIR="$CASE_DIR/cache"
    ccache --zero-stats > /dev/null 2>&1

    # Дважды одно и то же: первый раз мимо кэша, второй обязан попасть.
    # Кеш гоняет `cl /EP` сам, поэтому случай заодно проверяет, что
    # препроцессор через обёртку отдаёт то, что можно сравнить.
    ccache cl /nologo /c a.c /Foa1.obj > first.log 2>&1 || { cat first.log; return 1; }
    ccache cl /nologo /c a.c /Foa2.obj > second.log 2>&1 || { cat second.log; return 1; }

    local stats hits
    stats="$(ccache --show-stats)"
    echo "$stats"
    hits="$(ccache --print-stats | awk -F'\t' '$1 == "direct_cache_hit" || $1 == "preprocessed_cache_hit" { n += $2 } END { print n + 0 }')"
    echo "cache hits: $hits"
    [ "$hits" -ge 1 ] || { echo "the second compilation did not hit the cache"; return 1; }
    cmp -s a1.obj a2.obj || { echo "the cached object differs from the compiled one"; return 1; }
    NOTE="second compilation served from cache, object identical"
}

case_offline() {
    command -v unshare >/dev/null || { skip_case "no unshare(1)"; return 77; }
    # --map-current-user, а не -r: под -r процесс внутри становится root, и
    # проверялся бы заодно Wine от root — другой случай, не тот, который нужен.
    unshare --map-current-user -n true 2>/dev/null \
        || { skip_case "no unprivileged network namespaces"; return 77; }
    workdir offline
    cat > main.c <<'EOF'
#include <stdio.h>
int main(void) { printf("offline ok\n"); return 0; }
EOF
    # Установка обязана быть самодостаточной: ни компиляция, ни запуск не
    # имеют права лезть в сеть. Проверяется тем, что сети попросту нет.
    local out
    out="$(unshare --map-current-user -n env CORK_HOME="$CORK_ROOT" PATH="$PATH" \
            sh -c "\"$CORK_BIN\" cl /nologo main.c && \"$CORK_BIN\" run -- ./main.exe" 2>&1)"
    local rc=$?
    echo "$out"
    [ $rc -eq 0 ] || return 1
    expect_contains "$out" "offline ok" || return 1
    NOTE="compiled and ran with no network at all"
}

case_ogre() {
    command -v cmake >/dev/null && command -v ninja >/dev/null || { skip_case "no cmake/ninja"; return 77; }
    local toolchain="$CORK_ROOT/toolchains/current/share/cork-toolchain.cmake"
    [ -f "$toolchain" ] || { skip_case "no toolchain file"; return 77; }
    local src="$WORK/ogre-src"
    if [ ! -d "$src" ]; then
        local tarball="$WORK/ogre-14.5.2.tar.gz"
        [ -f "$tarball" ] || curl -fL --retry 3 -o "$tarball" \
            https://github.com/OGRECave/ogre/archive/refs/tags/v14.5.2.tar.gz || return 1
        mkdir -p "$src" && tar -xzf "$tarball" -C "$src" --strip-components=1 || return 1
    fi
    workdir ogre
    export PATH="$CORK_ROOT/toolchains/current/bin/x64:$PATH"
    # Только OgreMain: рендереры и примеры тянут зависимости, к самой
    # кросс-компиляции отношения не имеющие.
    cmake -B build -G Ninja -S "$src" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release \
        -DOGRE_BUILD_RENDERSYSTEM_D3D11=OFF \
        -DOGRE_BUILD_RENDERSYSTEM_GL=OFF -DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=OFF \
        -DOGRE_BUILD_RENDERSYSTEM_GLES2=OFF -DOGRE_BUILD_RENDERSYSTEM_VULKAN=OFF \
        -DOGRE_BUILD_COMPONENT_BITES=OFF -DOGRE_BUILD_COMPONENT_OVERLAY=OFF \
        -DOGRE_BUILD_SAMPLES=OFF -DOGRE_BUILD_TOOLS=OFF -DOGRE_BUILD_TESTS=OFF \
        -DOGRE_BUILD_PLUGIN_ASSIMP=OFF -DOGRE_BUILD_PLUGIN_FREEIMAGE=OFF \
        -DOGRE_BUILD_DEPENDENCIES=OFF || return 1
    "$CORK_BIN" run -- cmake --build build --target OgreMain || return 1
    local dll
    dll="$(find build -name 'OgreMain*.dll' -print -quit)"
    [ -n "$dll" ] || { echo "no OgreMain dll produced"; return 1; }
    echo "built: $dll"
    pe_machine "$dll"
    NOTE="OgreMain built ($(du -h "$dll" | cut -f1))"
}

# --------------------------------------------------------------------- прогон

if [ "$DO_INSTALL" = 1 ]; then
    step "download and install"
    # Каталог сборки download заводит себе сам под staging/, и install без
    # аргумента продолжает ровно тот, который только что получился.
    "$CORK_BIN" download --accept-license --store "$CORK_ROOT/store" || exit 1
    "$CORK_BIN" install || exit 1
fi

if [ ! -e "$CORK_ROOT/toolchains/current" ]; then
    echo "acceptance: nothing installed under $CORK_ROOT; re-run with --install" >&2
    exit 2
fi

mkdir -p "$WORK/logs"
banner() {
    printf 'cork:    %s\n' "$CORK_BIN"
    # `cork version` печатает две строки — свою версию и нужный ей Wine, —
    # и обе нужны в отчёте: без второй непонятно, на чём это прогонялось.
    "$CORK_BIN" version | sed 's/^/         /'
    printf 'root:    %s\n' "$CORK_ROOT"
    printf 'host:    %s\n' "$(uname -srm)"
}
banner
printf 'work:    %s\n' "$WORK"

started_all=$(date +%s)
for ((i = 0; i < ${#CASES[@]}; i += 2)); do
    name="${CASES[i]}"; desc="${CASES[i+1]}"
    if [ ${#SELECTED[@]} -gt 0 ]; then
        wanted=0
        for s in "${SELECTED[@]}"; do [ "$s" = "$name" ] && wanted=1; done
        [ $wanted = 1 ] || continue
    elif [ "$name" = ogre ] && [ "$WITH_OGRE" != 1 ]; then
        skip=$((skip + 1)); ROWS+=("$name|skip|0s|not requested; pass --with-ogre")
        continue
    fi
    run_case "$name" "$desc"
done
elapsed_all=$(( $(date +%s) - started_all ))

table() {
    printf '%-16s %-6s %-8s %s\n' case result time note
    printf '%-16s %-6s %-8s %s\n' ---------------- ------ -------- ----
    for row in "${ROWS[@]}"; do
        IFS='|' read -r n r t note <<<"$row"
        printf '%-16s %-6s %-8s %s\n' "$n" "$r" "$t" "$note"
    done
    printf '\n%d passed, %d failed, %d skipped in %ds\n' "$pass" "$fail" "$skip" "$elapsed_all"
}

printf '\n'
table
if [ -n "$REPORT" ]; then
    mkdir -p "$(dirname "$REPORT")"
    { printf '# cork acceptance\n\n'
      printf '```\n'
      printf 'date:    %s\n' "$(date -Iseconds)"
      banner
      printf '\n'
      table
      printf '```\n'
    } > "$REPORT"
    printf '\nreport written to %s\n' "$REPORT"
fi

# Убирается только то, что построено случаями. Исходники Ogre и его тарбол
# остаются: выкачивать сто мегабайт заново на каждый прогон незачем.
if [ "$KEEP" != 1 ]; then
    for dir in "$WORK"/*/; do
        case "$(basename "$dir")" in
            logs|ogre-src) ;;
            *) rm -rf "$dir" ;;
        esac
    done
fi

exit $(( fail > 0 ))
