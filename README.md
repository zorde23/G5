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

## Build and run

```powershell
git clone --recursive https://github.com/zorde23/G5
powershell -ExecutionPolicy Bypass -File tools\build_hybrid.ps1 -Profile g5 -Eboot C:\dump\eboot.bin -GameDir C:\dump -Output C:\g5
```

Then run `C:\g5\eboot.exe`. Requirements and every option:
[Usage, "Kyty GPU hybrid build"](docs/user/USAGE.md#kyty-gpu-hybrid-build).

## Branches

- `g5` (default): this project.
- `main`: a mirror of AnyPS5 upstream, kept for reference.

## Credits, disclaimer and license

- boykopovar and the AnyPS5 contributors; InoriRus for Kyty; TheCruZ and the KytyPS5 contributors.
- For interoperability, research, preservation and compatibility. It does not include, distribute or require copyrighted software, firmware, keys or proprietary libraries; users are responsible for obtaining and using any binaries lawfully. Not affiliated with, authorized or endorsed by Sony Interactive Entertainment. "PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment Inc.
- GNU General Public License version 2 only, as AnyPS5.
