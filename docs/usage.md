# Using cork

[README](../README.md) · [Русский](usage.ru.md)

## Status

**Pre-release, under active development.** Compiler wrappers, SDK installation,
MSBuild, isolated build sessions and build-system integration are implemented.
The [acceptance suite](../tools/acceptance/run.sh) exercises real compilation,
linking and execution, including CMake/CTest, clang-cl, ccache and clangd;
Ogre3D is an optional larger build.

There is no published cork release yet. Automatic Wine download is implemented,
but [runtime pins](../wine/runtime-pins.txt) are still empty pending the first
runtime release. **For now, build cork and its Wine runtime from source.**
The recipe below includes the PE helper required to run the tools.

## Installation

You need an **x86-64 Linux host**, network access for the initial setup, and
roughly **12 GB** for an installed toolchain and its download cache. Allow
additional space for source builds, retained installations and your projects.
The exact size depends on the selected targets and toolset versions.

Once installed, cork uses its own Wine and reads Microsoft installers itself.
System Wine, `msitools` and `cabextract` are not runtime prerequisites. Build
systems such as CMake, Ninja or Meson are separate tools you supply as needed.

### Build from source

The development environment is a Fedora 44 Distrobox named `cork-dev`. Reuse
it if it already exists; otherwise create it, then enter it:

```sh
distrobox create --name cork-dev --image registry.fedoraproject.org/fedora:44 --yes
distrobox enter cork-dev
```

Run the following inside the container. Install development dependencies there:

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

Use vcpkg at the baseline recorded in the repository. If you already have a
suitable checkout, set `VCPKG_ROOT` to it and skip the clone and bootstrap:

```sh
export VCPKG_ROOT="$CORK_HOME/dev/vcpkg"
git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
cork_vcpkg_baseline="$(python3 -c 'import json; print(json.load(open("vcpkg.json"))["builtin-baseline"])')"
git -C "$VCPKG_ROOT" checkout --detach "$cork_vcpkg_baseline"
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
. tools/toolchain_env.sh
```

Build Wine, then build the PE helper against the SDK split out by that build.
The Wine build can take tens of minutes:

```sh
tools/wine/build.sh
tools/wine/build-helper.sh --wine "$PWD/build/wine-staging-sdk/opt/cork-wine"

cmake --preset linux-release -DCORK_HELPER="$PWD/build/helper/cork-helper.exe"
cmake --build --preset linux-release
ctest --preset linux-release
export PATH="$PWD/build/linux-release:$PATH"
```

Make the runtime available under the ID expected by this cork binary:

```sh
cork_runtime_id="$(cork version | awk '/^wine runtime / {print $3}')"
mkdir -p "$CORK_HOME/runtime"
ln -sT "$PWD/build/wine-staging/opt/cork-wine" "$CORK_HOME/runtime/$cork_runtime_id"
cork version
```

Keep the checkout in place: the runtime link points into it. If that runtime
path already exists, check which build it contains before replacing anything.
Continue with [Quick start](#quick-start) in the same container.

**Run the resulting binary inside `cork-dev`.** A binary built against the
container's glibc may not run on an older host distribution. More build details
and sanitizer presets are in [the development guide](development.md)
(Russian).

Future binary releases will be listed on the
[releases page](https://github.com/Cheviiot/cork/releases). The release workflows
exist, but the complete release installation path is not yet verified.

## Quick start

With the source build above ready, install Microsoft's tools. Pass
`--accept-license` only after reviewing and accepting Microsoft's applicable
license terms:

```sh
cork setup --accept-license
eval "$(cork env --shell bash)"
```

`setup` downloads and unpacks the selected packages, prepares the installation
and Wine prefix, and makes the new toolchain current. `env` adds the wrappers
to `PATH` and exports `CORK_TOOLCHAIN_FILE` for CMake.

Create and run a small program in your project directory:

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

The program prints `Hello from MSVC on Linux!`. `hello.exe` is a Windows
executable; `cork run` executes it through the bundled Wine.

For repeated use, add the `cork env` line to your shell configuration. It
follows `toolchains/current`, so it continues to work after a new installation.
Use `--shell zsh` for zsh; in fish use `cork env --shell fish | source`.
Completion is available for all three shells:

```sh
eval "$(cork completion bash)"
```

## Build systems

The examples below use an installed x64 toolchain and your project's source
directory. Configure separate build directories for different targets.

### CMake and Ninja

```sh
eval "$(cork env --shell bash --arch x64)"
cmake -S . -B build-win64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$CORK_TOOLCHAIN_FILE" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cork run -- cmake --build build-win64
ctest --test-dir build-win64 --output-on-failure
```

The supplied toolchain sets `CMAKE_CROSSCOMPILING_EMULATOR` to `cork run --`,
allowing CMake to run built executable targets and CTest to run their tests.
Wrap the build in `cork run` to give it one Wine session and clean up on exit.

### MSBuild

```sh
eval "$(cork env --shell bash --arch x64)"
cork run -- msbuild MyProject.vcxproj /p:Configuration=Release /p:Platform=x64
```

Use your project's file name, configuration and platform. MSBuild runs under
the bundled Wine Mono; support for a particular project also depends on the
tasks and components it requires.

### Meson

```sh
eval "$(cork env --shell bash --arch x64)"
meson setup build-meson-win64 \
  --cross-file "$(dirname "$CORK_TOOLCHAIN_FILE")/cork-x64-cross.ini"
cork run -- meson compile -C build-meson-win64
meson test -C build-meson-win64
```

Installation generates a cross file for each target, including an executable
wrapper for tests. Execution through Wine is available for x86 and x64 targets.

### vcpkg

```sh
eval "$(cork env --shell bash --arch x64)"
vcpkg install zlib --triplet x64-windows \
  --overlay-triplets="$(dirname "$CORK_TOOLCHAIN_FILE")"
```

The generated overlay triplets point vcpkg at cork's CMake toolchain, so
dependencies use the same compiler as the project. The supplied triplets select
dynamic libraries and the dynamic CRT. Individual ports may have additional
host-tool requirements.

### Editors, clang-cl and ccache

For clangd, generate a configuration in your project's root:

```sh
cork env --clangd > .clangd
```

If `.clangd` already exists, merge the generated `CompileFlags` settings into
it. These settings locate the installed headers through `/winsysroot` and
select MSVC's compatibility version. Regenerate them when changing toolsets.
Point clangd at your build directory's `compile_commands.json` as usual.

Native `clang-cl` and `lld-link`, when installed separately, can use the same
MSVC headers and libraries:

```sh
clang-cl --target=x86_64-pc-windows-msvc \
  /winsysroot "${CORK_HOME:-$HOME/.cork}/toolchains/current" \
  -fuse-ld=lld /nologo hello.c /Fehello-clang.exe
cork run -- ./hello-clang.exe
```

The acceptance suite also checks ccache through the compiler wrappers,
including a cache hit with a byte-identical object file.

## Targets

| Windows target | `--arch` / `--architecture` | Equivalent target triple | Run through cork on x86-64 Linux |
| --- | --- | --- | --- |
| x86-64 | `x64` | `x86_64-pc-windows-msvc` | Yes |
| x86, 32-bit | `x86` | `i686-pc-windows-msvc` | Yes |
| ARM64 | `arm64` | `aarch64-pc-windows-msvc` | Build only |

Select packages at installation with `cork setup --architecture x64 --accept-license`;
repeat `--architecture` to request more targets. Select an installed target
for your shell with `cork env --arch`. For a 32-bit CMake build, select both
the wrappers and the CMake target:

```sh
eval "$(cork env --shell bash --arch x86)"
cmake -S . -B build-win32 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$CORK_TOOLCHAIN_FILE" -DCORK_TARGET=x86
cork run -- cmake --build build-win32
```

`cork setup` also accepts `--msvc-version` and `--sdk-version` for selecting
versions present in the manifest. See `cork help setup` for the current options.

The current runtime supports x86-64 Linux hosts. It is a console-oriented Wine
build without graphical, audio or Vulkan support. Windows Driver Kit and
DirectX SDK support are not implemented. Treat successful compilation and
execution under Wine as part of testing; validate shipped software on Windows.

## Diagnostics

```sh
cork doctor           # Receipt, configuration, key files and Wine composition
cork doctor --deep    # Also verify the full installation digest
cork doctor --build   # Also compile and link a probe for each installed target
```

Installations are assembled and checked in staging before `toolchains/current`
is switched atomically. Each installation has a `receipt.json` recording its
packages, hashes and runtime composition. An interrupted download or unpack
does not replace the current toolchain.

Each build session gets its own Wine prefix. Prefixes use reflinks where
available and fall back to copying. On filesystems without reflinks, session
creation takes more time and disk space; `CORK_PREFIX_MODE=shared` trades
isolation for a shared prefix.

| Command | Purpose |
| --- | --- |
| `cork help <command>` | Show the current command options |
| `cork version` | Report the cork revision and expected Wine runtime |
| `cork download --accept-license` | Download and stage packages separately |
| `cork install` | Finish a staged installation |
| `cork session list` | Inspect build sessions |
| `cork session stop <key>` | Clean up a session that is no longer in use |
| `cork gc` | Clean up stale staging directories and sessions |

If setup reports that no Wine runtime has been published, complete the source
bootstrap above before retrying. If the helper is missing, build it and rerun
CMake configuration before rebuilding cork. For a bug report, include
`cork version`, your distribution, the failing command, its complete output,
and relevant `cork doctor` results in a
[GitHub issue](https://github.com/Cheviiot/cork/issues).

## Storage and language

`CORK_HOME` defaults to `~/.cork`. It holds the download cache, toolchains,
Wine runtime, prefixes and cork's temporary files. Set it before setup to
choose another location. Keep `staging/` and `toolchains/` on the same
filesystem so publication can use an atomic rename.

Help, progress and prompts are available in English and Russian, following
your locale. `CORK_LANG=en` or `CORK_LANG=ru` overrides the language for cork.
Error messages remain in English.
