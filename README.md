# G5

A project that runs one PS5 title natively on Windows, built on the generic
[ps5-hybrid](https://github.com/zorde23/ps5-hybrid) base: the
[AnyPS5](https://github.com/boykopovar/AnyPS5) relinker, kernel and libraries, with Kyty's GPU
([kyty-gpu-hybrid](https://github.com/zorde23/kyty-gpu-hybrid), submodule `3rdparty/kyty-gpu`)
as the graphics backend.

This repository is the base plus what belongs to this title only: its profile,
`tools/hybrid-profiles/g5.ini` (the base keys, the log in `g5-last.log` and Brazilian Portuguese
as the console language). The title's files, saves, captures and patches are never part of it.

## Status

- Reaches the open world and plays on an AMD Radeon RX 6750 XT (Windows 11); the target is a steady 60 fps.
- Each push to `g5` passes the build, the unit tests, a menu smoke run and a frame-capture replay.

## Install

You need your own copy of the title, dumped from your own console. Nothing from the title is
downloaded or included here.

### 1. Install the tools (once)

Windows 10 or 11 (64-bit) and a GPU with Vulkan (tested on an AMD Radeon RX 6750 XT), plus:

1. [Git for Windows](https://git-scm.com/download/win).
2. [WinLibs GCC 15.2.0](https://github.com/brechtsanders/winlibs_mingw/releases/tag/15.2.0posix-14.0.0-ucrt-r7), the `x86_64-ucrt-posix-seh` zip, extracted to e.g. `C:\tools\winlibs`. It already brings CMake and Ninja.
3. [Python 3](https://www.python.org/downloads/).
4. [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/) with "Desktop development with C++" and "C++ Clang tools for Windows".
5. [Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows); open a new terminal after installing it.

Two compilers are needed because the two halves build differently: GCC builds the relinker and
libraries, clang-cl builds Kyty's GPU.

### 2. Get the source

Use a short path: long paths break the build on Windows.

```powershell
git clone --recursive https://github.com/zorde23/G5 C:\g5src
cd C:\g5src
```

### 3. Prepare your dump

A folder with the decrypted (ELF) `eboot.bin`, `sce_sys\` and the rest of the title's files, for
example `D:\dump`. Put it on the same drive as the output folder of step 4: the script links the
files instead of copying them, and never copies, moves or deletes them.

### 4. Build

In PowerShell, from `C:\g5src` (adjust the WinLibs and Git paths to where you installed them):

```powershell
$env:PATH = "C:\tools\winlibs\mingw64\bin;C:\Program Files\Git\cmd;C:\Program Files\Git\usr\bin;$env:PATH"
powershell -ExecutionPolicy Bypass -File tools\build_hybrid.ps1 -Profile g5 -Eboot D:\dump\eboot.bin -GameDir D:\dump -Output D:\g5
```

If a tool is missing, the script lists it and stops before building. The first build downloads
dependencies and takes a while; it ends with `Ready: D:\g5\eboot.exe`.

### 5. Play

Double-click `D:\g5\eboot.exe` (from a terminal, start it inside `D:\g5`: saves go to `_sd\` in
the current folder). Controllers mapped by SDL work out of the box;
keyboard and mouse are set in `anyps5-input.ini` ([input mapping](docs/user/INPUT_MAPPING.md)).
Settings live in `ps5-hybrid.ini` beside the executable and the log of the last run in
`g5-last.log`. The game stays offline: its sockets are kept on the loopback interface, so Windows
does not ask for network access.

### Update

```powershell
cd C:\g5src
git pull
git submodule update --init --recursive
```

Then run step 4 again with the same `-Output`; your saves (`_sd`) and `ps5-hybrid.ini` are kept.

### Problems

- "Missing prerequisites": install what the script lists, open a new terminal and set `PATH` again.
- `Python was not found` (when running the repo's `python3 tools/...` by hand): use `py -3` instead, or turn off the "App execution aliases" for `python3` in Windows settings.
- `running scripts is disabled on this system`: a group policy blocks PowerShell scripts; see [Build instructions](docs/dev/BUILD.md).
- `opening dependency file ... No such file or directory`: the source path is too long; clone to a short path such as `C:\g5src`.
- Every option of the script: [Usage, "Kyty GPU hybrid build"](docs/user/USAGE.md#kyty-gpu-hybrid-build).

## Branches

- `g5` (default): this project.
- `main`: a mirror of AnyPS5 upstream, kept for reference.

## Credits, disclaimer and license

- boykopovar and the AnyPS5 contributors; InoriRus for Kyty; TheCruZ and the KytyPS5 contributors.
- For interoperability, research, preservation and compatibility. It does not include, distribute or require copyrighted software, firmware, keys or proprietary libraries; users are responsible for obtaining and using any binaries lawfully. Not affiliated with, authorized or endorsed by Sony Interactive Entertainment. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment Inc.
- GNU General Public License version 2 only, as AnyPS5.
