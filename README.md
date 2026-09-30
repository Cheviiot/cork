# cork

[![build](https://img.shields.io/github/actions/workflow/status/Cheviiot/cork/ci.yml?branch=main&label=build&style=flat-square)](https://github.com/Cheviiot/cork/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE.txt)

**English** · [Русский](README.ru.md)

**Build Windows software on Linux with the real MSVC toolchain.**

cork runs Microsoft's C++ compiler, linker, Windows SDK tools and MSBuild
through its own Wine. It handles downloads, installation and Wine sessions,
so you can use familiar commands such as `cl`, `link` and `msbuild`.

Build for **x64, x86 and ARM64** from x86-64 Linux. Run x64 and x86 console
programs through cork; ARM64 is currently a build-only target.

> **Under active development.** No binary release yet. Start with the
> [source build guide](docs/usage.md#build-from-source), which includes Wine
> and the required helper. Allow roughly 12 GB for the installed toolchain
> and download cache, plus space for source builds.

## Quick start

After building cork and its runtime, install the tools and compile your
`hello.c`. `--accept-license` confirms acceptance of Microsoft's terms.

```sh
cork setup --accept-license
eval "$(cork env --shell bash)"
cl /nologo hello.c /Fehello.exe
cork run -- ./hello.exe
```

## CMake

From your project's source directory, with the environment above active:

```sh
cmake -S . -B build-win64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$CORK_TOOLCHAIN_FILE"
cork run -- cmake --build build-win64
ctest --test-dir build-win64 --output-on-failure
```

The toolchain lets CTest run Windows executables through cork.

## Documentation

- [Usage guide](docs/usage.md) — installation, MSBuild, Meson, vcpkg, editors and diagnostics.
- [Development](docs/development.md) — build environment and contribution details (Russian).
- [Internals](docs/generations.md) · [Acceptance tests](docs/acceptance.md) (Russian).

cork is [MIT-licensed](LICENSE.txt). Microsoft tools and third-party
components retain their own licenses.
