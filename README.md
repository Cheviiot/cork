# cork

[![build](https://img.shields.io/github/actions/workflow/status/Cheviiot/cork/ci.yml?branch=main&label=build&style=flat-square)](https://github.com/Cheviiot/cork/actions/workflows/ci.yml)
[![licence](https://img.shields.io/badge/licence-MIT-blue?style=flat-square)](LICENSE.txt)
[![status](https://img.shields.io/badge/status-pre--release-orange?style=flat-square)](#where-things-stand)

Microsoft's own C++ compiler, on a Linux machine, under its own name.

```console
$ cork download --accept-license
$ cork install
$ export PATH=~/.cork/toolchains/current/bin/x64:$PATH

$ cl /nologo hello.c /Fehello.exe
hello.c
$ file hello.exe
hello.exe: PE32+ executable (console) x86-64, for MS Windows
```

That is `cl.exe` from Visual Studio Build Tools, not a compiler pretending to
be it. Object layout, name mangling, `#pragma` handling, the linker's section
ordering, the exact diagnostics — all of it is the real thing, because it is
the real thing. Use it when "close enough to MSVC" is not close enough:
shipping Windows binaries from Linux CI, reproducing a compiler bug, or
building code that only ever compiled on Windows.

## What you need

A 64-bit Linux machine and roughly 12 GB of disk: about 3 GB of downloads
kept for reuse, 7 GB of unpacked toolchain, the rest Wine and its prefix.

Nothing else. cork carries its own Wine, unpacks Microsoft's installers
itself, and asks nothing of your package manager — there is no `wine`,
`msitools`, `cabextract` or `git` to install first.

Targets today: `x64`, `x86` and `arm64` Windows, from an x86-64 Linux host.

## What it is careful about

Running a Windows compiler through Wine is the easy half. The half that eats
weeks is everything around it, and that is where cork spends its effort.

**An install is either whole or absent.** Each installation is a *generation*:
a tree assembled in a staging directory, checked there, and only then renamed
into place. The digest of the tree is part of its directory name, so
publishing is a rename into a name nothing else holds, and the switch of
`toolchains/current` is a single atomic symlink replacement. Kill cork
mid-download, mid-unpack or mid-verify and nothing on your `PATH` changes.

**What was installed is written down.** A receipt records the manifest it came
from, every payload by hash, how many files came out of which package, the
digest of the tree, and the composition of the bundled Wine — down to how many
symlinks and Mono assemblies it should contain. `cork doctor` compares the
installation against that record rather than against a list of directory
names, so a tree that lost its symlinks during unpacking is reported as
broken instead of passing because the folders exist.

```console
$ cork doctor           # receipt, key binaries, Wine composition
$ cork doctor --deep    # plus every file against the recorded digest
$ cork doctor --build   # plus compile and link a probe for each target
```

**Failures do not get lost.** Every step that can fail returns a result the
caller has to handle. There is no step that tries something, prints a note
when it does not work, and reports success anyway.

**No temporary files outside its own tree.** Response files, run directories
and staging all live under `$CORK_HOME` (`~/.cork` unless you say otherwise),
never in `/tmp`.

## How it is built

cork is a native Linux binary that never links against Wine. The work that
genuinely requires Windows semantics — spawning the process, translating Unix
paths into DOS paths through Wine's own translation, holding the whole process
tree in a Job Object so nothing survives a cancelled build — lives in a small
PE executable compiled by `wineg++` and embedded into the binary at build
time. The two halves talk through a binary request file, which sidesteps both
command-line quoting and Windows' 32 767-character limit.

Wine itself is a submodule, built from source with a two-patch series whose
only purpose is to make it *refuse* a system wineserver or a system Mono
instead of quietly pairing our loader with someone else's. A mismatched pair
does not fail cleanly; it corrupts the prefix and surfaces days later in an
unrelated build.

The containers Microsoft ships packages in — CFBF, MSI tables, CAB, including
cabinets stored as streams inside the `.msi` itself — are read by cork
directly. This keeps `download` testable offline and makes a failure say which
file went wrong and why, instead of handing you an installer's exit code.

## Shell setup

```sh
eval "$(cork env)"                  # put the tools on PATH
eval "$(cork completion bash)"      # tab completion; also zsh and fish
```

`cork env` points at `toolchains/current`, not at a particular generation, so
the setting survives the next install.

## Building something

The wrappers are ordinary programs named `cl`, `link`, `lib`, `rc` and so on,
so anything that shells out to a compiler works unchanged.

```sh
eval "$(cork env)"
cl /nologo hello.c
cork run -- ./hello.exe            # run what you just built
```

`cork run` gives the command its own Wine prefix and takes it away afterwards,
Ctrl-C included. A program name ending in `.exe` is run through Wine; anything
else is run natively, so `cork run -- ninja` covers a whole build.

For CMake, a toolchain file ships with every installation and `cork env` points
at it:

```sh
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$CORK_TOOLCHAIN_FILE"
cork run -- cmake --build build
```

Pass `-DCORK_TARGET=x86` for a 32-bit build.

## Language

Help, progress and prompts follow your locale; `CORK_LANG` overrides it for
cork alone. English and Russian ship today.

```sh
CORK_LANG=ru cork help
```

Error messages stay in English whatever the setting. They end up in bug
reports and in web searches, and a translated one is findable by nobody.

## Documentation

- [docs/generations.md](docs/generations.md) — how an installation is laid out,
  what the states mean, what `doctor` actually checks
- [docs/development.md](docs/development.md) — building cork itself
- [docs/acceptance.md](docs/acceptance.md) — what has to work before a release,
  and how to run the whole matrix

## Where things stand

Pre-release, under active development. Working: manifest resolution, payload
download with resume, VSIX and MSI unpacking, the wrappers, atomic
publication, the three levels of checking, compiling and linking for all three
targets, per-build Wine prefixes cloned from a portable template, several
compiler toolsets installed side by side, and MSBuild projects.

Ogre3D 14.5.2 builds: OgreMain configures in half a minute and links a 3.3 MB
x64 DLL. The rest of the acceptance matrix is in
[docs/acceptance.md](docs/acceptance.md) and runs nightly.

Not there yet: the size reduction passes, the Windows Driver Kit and the
DirectX SDK.

## Licensing

cork's own source is MIT; see [LICENSE.txt](LICENSE.txt). The Microsoft
toolchain that `download` fetches stays under Microsoft's terms, which you
accept with `--accept-license`. cork does not redistribute it and does not
alter what you agree to.
