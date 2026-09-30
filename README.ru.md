# cork

[![build](https://img.shields.io/github/actions/workflow/status/Cheviiot/cork/ci.yml?branch=main&label=build&style=flat-square)](https://github.com/Cheviiot/cork/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE.txt)

[English](README.md) · **Русский**

**Собирайте Windows-программы на Linux настоящим MSVC.**

cork запускает C++-компилятор Microsoft, компоновщик, инструменты Windows SDK
и MSBuild через собственный Wine. Он берёт на себя загрузку, установку
и сессии Wine, оставляя привычные команды `cl`, `link` и `msbuild`.

Сборка под **x64, x86 и ARM64** с Linux-хоста x86-64. Консольные программы
x64 и x86 можно запускать через cork; для ARM64 пока доступна только сборка.

> **Проект активно развивается.** Готового релиза пока нет. Начните с
> [инструкции по сборке из исходников](docs/usage.ru.md#сборка-из-исходников),
> включая Wine и необходимый хелпер. Для установленного тулчейна и кеша
> нужно около 12 ГБ, для сборки из исходников — дополнительное место.

## Быстрый старт

После сборки cork и его рантайма установите инструменты и скомпилируйте свой
`hello.c`. Ключ `--accept-license` подтверждает согласие с условиями Microsoft.

```sh
cork setup --accept-license
eval "$(cork env --shell bash)"
cl /nologo hello.c /Fehello.exe
cork run -- ./hello.exe
```

## CMake

В каталоге исходников своего проекта, с подключённым окружением из примера выше:

```sh
cmake -S . -B build-win64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$CORK_TOOLCHAIN_FILE"
cork run -- cmake --build build-win64
ctest --test-dir build-win64 --output-on-failure
```

Файл тулчейна позволяет CTest запускать Windows-программы через cork.

## Документация

- [Использование](docs/usage.ru.md) — установка, MSBuild, Meson, vcpkg, редакторы и диагностика.
- [Разработка](docs/development.md) — окружение и подробности сборки.
- [Внутреннее устройство](docs/generations.md) · [Приёмочные тесты](docs/acceptance.md).

Код cork — под [лицензией MIT](LICENSE.txt). Инструменты Microsoft
и сторонние компоненты сохраняют собственные лицензии.
