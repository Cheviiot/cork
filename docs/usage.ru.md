# Использование cork

[README](../README.ru.md) · [English](usage.md)

## Состояние проекта

**Проект активно развивается и готовится к первому релизу.** Реализованы
обёртки компилятора, установка SDK, MSBuild, изолированные сессии и интеграция
с системами сборки. [Приёмочные сценарии](../tools/acceptance/run.sh) проверяют
настоящую компиляцию, компоновку и запуск, включая CMake/CTest, clang-cl,
ccache и clangd; Ogre3D служит дополнительной проверкой на большом проекте.

Опубликованного релиза cork пока нет. Автоматическая загрузка Wine
реализована, но [список закреплённых рантаймов](../wine/runtime-pins.txt)
остаётся пустым до первого выпуска рантайма. **Сейчас cork и его Wine нужно
собрать из исходников.** Инструкция ниже включает PE-хелпер, необходимый
для запуска инструментов.

## Установка

Нужны **Linux на x86-64**, доступ к сети для первоначальной установки и
примерно **12 ГБ** для установленного тулчейна и кеша загрузок. Для сборки
из исходников, сохранённых установок и ваших проектов понадобится
дополнительное место. Точный объём зависит от целей и версий инструментов.

Установленный cork использует собственный Wine и самостоятельно читает
установщики Microsoft. Системный Wine, `msitools` и `cabextract` для работы
не требуются. CMake, Ninja, Meson и другие системы сборки устанавливаются
отдельно по потребности.

### Сборка из исходников

Среда разработки — Distrobox `cork-dev` на Fedora 44. Используйте существующий
контейнер; если его ещё нет, создайте и войдите в него:

```sh
distrobox create --name cork-dev --image registry.fedoraproject.org/fedora:44 --yes
distrobox enter cork-dev
```

Все следующие команды выполняются внутри контейнера. Установите в нём
зависимости разработки:

```sh
sudo dnf install -y --setopt=install_weak_deps=False \
  gcc-c++ cmake ninja-build git make curl perl-core perl-IPC-Cmd python3 \
  zip unzip tar xz zstd pkgconf-pkg-config \
  autoconf automake libtool autoconf-archive bison flex patch file which \
  clang lld llvm

git clone --recurse-submodules https://github.com/Cheviiot/cork.git
cd cork

export CORK_HOME="${CORK_HOME:-$HOME/.cork}"
mkdir -p "$CORK_HOME/tmp" "$CORK_HOME/dev"
export TMPDIR="$CORK_HOME/tmp"
```

Используйте vcpkg на ревизии baseline из репозитория. Если подходящий клон
уже есть, задайте `VCPKG_ROOT` и пропустите клонирование и bootstrap:

```sh
export VCPKG_ROOT="$CORK_HOME/dev/vcpkg"
git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
cork_vcpkg_baseline="$(python3 -c 'import json; print(json.load(open("vcpkg.json"))["builtin-baseline"])')"
git -C "$VCPKG_ROOT" checkout --detach "$cork_vcpkg_baseline"
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
. tools/toolchain_env.sh
```

Соберите Wine, затем PE-хелпер с помощью SDK, выделенного этой сборкой.
Сборка Wine может занять десятки минут:

```sh
tools/wine/build.sh
tools/wine/build-helper.sh --wine "$PWD/build/wine-staging-sdk/opt/cork-wine"

cmake --preset linux-release -DCORK_HELPER="$PWD/build/helper/cork-helper.exe"
cmake --build --preset linux-release
ctest --preset linux-release
export PATH="$PWD/build/linux-release:$PATH"
```

Подключите рантайм под идентификатором, который ожидает собранный cork:

```sh
cork_runtime_id="$(cork version | awk '/^wine runtime / {print $3}')"
mkdir -p "$CORK_HOME/runtime"
ln -sT "$PWD/build/wine-staging/opt/cork-wine" "$CORK_HOME/runtime/$cork_runtime_id"
cork version
```

Не перемещайте клон: ссылка на рантайм ведёт внутрь него. Если такой путь
рантайма уже существует, перед заменой проверьте, какая сборка за ним стоит.
Переходите к [быстрому старту](#быстрый-старт) в том же контейнере.

**Запускайте собранный бинарник внутри `cork-dev`.** Версия glibc в контейнере
может быть новее хостовой, и такой бинарник не запустится на старом
дистрибутиве. Подробности сборки и пресеты санитайзеров описаны в
[руководстве разработчика](development.md).

Готовые выпуски появятся на
[странице релизов](https://github.com/Cheviiot/cork/releases). Воркфлоу выпуска
уже созданы, но полный путь установки из релиза ещё не проверен.

## Быстрый старт

После подготовки сборки установите инструменты Microsoft. Передавайте
`--accept-license` только после ознакомления и согласия с применимыми
лицензионными условиями Microsoft:

```sh
cork setup --accept-license
eval "$(cork env --shell bash)"
```

`setup` скачивает и распаковывает выбранные пакеты, готовит установку и
префикс Wine, затем переключает текущий тулчейн. `env` добавляет обёртки
в `PATH` и экспортирует `CORK_TOOLCHAIN_FILE` для CMake.

Создайте и запустите небольшую программу в каталоге своего проекта:

```sh
cat > hello.c <<'EOF'
#include <stdio.h>
int main(void) {
    puts("Hello from MSVC on Linux!");
    return 0;
}
EOF

cl /nologo hello.c /Fehello.exe
cork run -- ./hello.exe
```

Программа выведет `Hello from MSVC on Linux!`. Файл `hello.exe` — исполняемый
файл Windows; `cork run` запускает его через собственный Wine.

Для постоянного использования добавьте строку `cork env` в настройки
оболочки. Она указывает на `toolchains/current` и продолжает работать после
новой установки. Для zsh используйте `--shell zsh`; для fish —
`cork env --shell fish | source`. Автодополнение доступно для всех трёх
оболочек:

```sh
eval "$(cork completion bash)"
```

## Системы сборки

Примеры ниже выполняются в каталоге исходников вашего проекта с установленным
x64-тулчейном. Для разных целей используйте отдельные каталоги сборки.

### CMake и Ninja

```sh
eval "$(cork env --shell bash --arch x64)"
cmake -S . -B build-win64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$CORK_TOOLCHAIN_FILE" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cork run -- cmake --build build-win64
ctest --test-dir build-win64 --output-on-failure
```

Поставляемый файл тулчейна задаёт `CMAKE_CROSSCOMPILING_EMULATOR` как
`cork run --`: CMake может запускать собранные исполняемые цели, а CTest —
их тесты. Запуск сборки через `cork run` объединяет её в одну сессию Wine
с очисткой при завершении.

### MSBuild

```sh
eval "$(cork env --shell bash --arch x64)"
cork run -- msbuild MyProject.vcxproj /p:Configuration=Release /p:Platform=x64
```

Подставьте имя файла, конфигурацию и платформу своего проекта. MSBuild
работает под собственным Wine Mono; совместимость конкретного проекта
также зависит от требуемых задач и компонентов.

### Meson

```sh
eval "$(cork env --shell bash --arch x64)"
meson setup build-meson-win64 \
  --cross-file "$(dirname "$CORK_TOOLCHAIN_FILE")/cork-x64-cross.ini"
cork run -- meson compile -C build-meson-win64
meson test -C build-meson-win64
```

При установке создаётся cross-файл для каждой цели, включая обёртку запуска
тестов. Запуск через Wine доступен для целей x86 и x64.

### vcpkg

```sh
eval "$(cork env --shell bash --arch x64)"
vcpkg install zlib --triplet x64-windows \
  --overlay-triplets="$(dirname "$CORK_TOOLCHAIN_FILE")"
```

Созданные overlay-триплеты направляют vcpkg к CMake-тулчейну cork, поэтому
зависимости собираются тем же компилятором, что и проект. Эти триплеты
выбирают динамические библиотеки и динамический CRT. Отдельным портам могут
понадобиться дополнительные инструменты на машине сборки.

### Редакторы, clang-cl и ccache

Для clangd создайте конфигурацию в корне своего проекта:

```sh
cork env --clangd > .clangd
```

Если `.clangd` уже существует, добавьте в него сгенерированные настройки
`CompileFlags`. Они указывают путь к установленным заголовкам через
`/winsysroot` и задают версию совместимости с MSVC. При смене набора
инструментов сгенерируйте настройки заново. Укажите clangd путь к
`compile_commands.json` из каталога сборки обычным для вашего редактора способом.

Отдельно установленные нативные `clang-cl` и `lld-link` могут использовать
те же заголовки и библиотеки MSVC:

```sh
clang-cl --target=x86_64-pc-windows-msvc \
  /winsysroot "${CORK_HOME:-$HOME/.cork}/toolchains/current" \
  -fuse-ld=lld /nologo hello.c /Fehello-clang.exe
cork run -- ./hello-clang.exe
```

Приёмочные сценарии также проверяют ccache через обёртки компилятора:
повторная компиляция берётся из кеша, объектный файл совпадает побайтово.

## Цели

| Цель Windows | `--arch` / `--architecture` | Равнозначный target triple | Запуск через cork на Linux x86-64 |
| --- | --- | --- | --- |
| x86-64 | `x64` | `x86_64-pc-windows-msvc` | Да |
| x86, 32 бита | `x86` | `i686-pc-windows-msvc` | Да |
| ARM64 | `arm64` | `aarch64-pc-windows-msvc` | Только сборка |

Выбрать пакеты при установке можно командой
`cork setup --architecture x64 --accept-license`; ключ `--architecture`
разрешено повторять для нескольких целей. Установленная цель выбирается для
оболочки через `cork env --arch`. Для 32-битной сборки CMake выберите
и обёртки, и цель CMake:

```sh
eval "$(cork env --shell bash --arch x86)"
cmake -S . -B build-win32 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$CORK_TOOLCHAIN_FILE" -DCORK_TARGET=x86
cork run -- cmake --build build-win32
```

`cork setup` также принимает `--msvc-version` и `--sdk-version` для выбора
версий, присутствующих в манифесте. Актуальные параметры перечислены в
`cork help setup`.

Текущий рантайм рассчитан на Linux-хосты x86-64. Это сборка Wine для
консольных инструментов без поддержки графики, звука и Vulkan. Поддержка
Windows Driver Kit и DirectX SDK пока не реализована. Успешная сборка
и запуск под Wine — часть проверки; выпускаемые программы проверяйте
и на Windows.

## Диагностика

```sh
cork doctor           # Receipt, конфигурация, ключевые файлы и состав Wine
cork doctor --deep    # Также сверка полного дайджеста установки
cork doctor --build   # Также компиляция и компоновка пробника для каждой цели
```

Установки собираются и проверяются в staging, после чего
`toolchains/current` переключается атомарно. В каждой установке есть
`receipt.json` со списком пакетов, хешами и составом рантайма. Прерванная
загрузка или распаковка не заменяет текущий тулчейн.

Каждая сборочная сессия получает собственный префикс Wine. Для клонирования
используется reflink с переходом на копирование, если он недоступен.
На файловых системах без reflink создание сессий требует больше времени
и места; `CORK_PREFIX_MODE=shared` позволяет использовать общий префикс
ценой изоляции сборок.

| Команда | Назначение |
| --- | --- |
| `cork help <command>` | Актуальные параметры команды |
| `cork version` | Ревизия cork и ожидаемый рантайм Wine |
| `cork download --accept-license` | Отдельная загрузка и подготовка пакетов |
| `cork install` | Завершение подготовленной установки |
| `cork session list` | Список сборочных сессий |
| `cork session stop <key>` | Очистка сессии, которая больше не используется |
| `cork gc` | Очистка устаревших staging-каталогов и сессий |

Если setup сообщает, что рантайм Wine ещё не опубликован, завершите
подготовку из исходников выше и повторите установку. Если отсутствует
хелпер, соберите его и заново выполните конфигурацию CMake перед пересборкой
cork. В [сообщении об ошибке на GitHub](https://github.com/Cheviiot/cork/issues)
укажите вывод `cork version`, дистрибутив, команду с ошибкой, её полный
вывод и относящиеся к проблеме результаты `cork doctor`.

## Хранение данных и язык

`CORK_HOME` по умолчанию указывает на `~/.cork`. Здесь лежат кеш загрузок,
тулчейны, рантайм Wine, префиксы и временные файлы cork. Чтобы выбрать
другое место, задайте переменную до установки. Каталоги `staging/` и
`toolchains/` должны находиться на одной файловой системе для атомарного
переименования при публикации.

Справка, прогресс и приглашения доступны на английском и русском и следуют
локали. `CORK_LANG=en` или `CORK_LANG=ru` задаёт язык отдельно для cork.
Сообщения об ошибках остаются английскими.
