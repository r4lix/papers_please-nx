# papers_please-nx

Papers, Please (Lucas Pope) running natively on the Nintendo Switch, built from the Steam PC version (Unity 2020.3, IL2CPP).

**No game assets are included.** You need your own copy of the game (the Steam version, 1.4.15).

## Disclaimer

This project was created **entirely with the help of AI coding tools**. I am **not a developer**, so the code may contain bugs or unfinished features. Use it at your own risk, with no guarantee of support or updates.

## Takedown

The NRO contains the game's code translated to ARM64. If you are a right holder and want it removed, contact me (r4lix on GitHub) and I will take it down within 48 hours.

## Install

1. Download `papers.zip` from the [latest release](https://github.com/r4lix/papers_please-nx/releases/latest) and extract it to the root of your SD card. This creates `sdmc:/switch/papers/`.
2. On a PC, extract the game data from your Steam copy: `python -I tools/prepare_data.py "<Steam>/PapersPlease" out` (needs UnityPy). Copy the result to `sdmc:/switch/papers/data`.
3. Copy `GameAssembly.dll` from the Steam folder to `sdmc:/switch/papers/game/`, and `PapersPlease_Data/il2cpp_data` to `sdmc:/switch/papers/game/il2cpp_data`.
4. Launch **Papers, Please** from the homebrew menu.

## Controls

A / ZR click, B / ZL right-click, X inspect, Y stamp bar, R toggle booth shutter, L call next, + Esc, sticks / D-pad move the cursor, touch works as a mouse.

## Building

`tools/build-lift.sh` / `tools/build-switch.sh` (devkitPro + python). The lifter (`tools/lift`) translates the x64 `GameAssembly.dll` of your own copy into C++. Notes in `RESUME.md`.
