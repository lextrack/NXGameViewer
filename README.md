# NX Game Viewer

A desktop app for browsing the files of Nintendo Switch games: open textures, 3D models,
sounds, videos, tables and archives, export them to common formats, and make simple mods.

It works on your own game dumps. Nothing in the game is ever changed: exports and mods are
written to separate folders.

## What it does

- **Browse** a game folder (or a single file) and see inside archives, even nested ones.
- **View** textures, 3D models with their textures, play sounds and videos, and read tables,
  messages and game data.
- **Export** to formats any tool can open: PNG, glTF (`.glb`, with animations where the game
  has them), WAV, JSON and CSV. A whole folder can be exported in one go.
- **Extract games**: unpack your own NSP, XCI, NSZ, XCZ or NUP dumps into `exefs` and `romfs`
  folders, using the `prod.keys` from your own console. **Verify** checks a dump against its
  hashes to find damaged files.
- **Make mods**: replace textures with a PNG, replace any file, or edit data and messages as
  JSON. The app saves the result in a mod folder ready for yuzu-based emulators, Ryujinx or
  Atmosphère, and can pack it as a zip. See the [Modding guide](Documents/Modding-Guide.md).

## Supported formats

| Kind | Formats |
|---|---|
| Archives | SARC (`.sarc`, `.szs`, `.pack`), zstd (`.zs`) and Yaz0 compression, RAGE RPF6, CRI AFS2/ACB, Unity asset bundles |
| Game dumps | NSP, XCI, NSZ, XCZ, NUP |
| Textures | BNTX, XTX, DDS, RAGE texture dictionaries, Unity textures; PNG, JPEG and other common images |
| Models | BFRES, Falcom MDL, RAGE models and animations, Unity meshes |
| Sound | BWAV, BARS, Nintendo Opus, Falcom nxopus, RAGE AWC; WAV, MP3, OGG, FLAC |
| Video | CRI USM, WebM, MKV, MP4 |
| Data | BYML/BGYML, MSBT, BCSV, RESTBL, AINB, BFEVFL, ASB, BAEV, terrain (TARB, TSCB), CRI @UTF, .NET BinaryFormatter |
| Falcom games | TBL, binary JSON, scripts |
| Other | Fonts (TTF, OTF, BFTTF, BFOTF), JSON, text, and a hex view for anything else |

Some formats belong to specific engines and games (Falcom's Trails games, Red Dead
Redemption's RAGE engine, Unity and CRI middleware). See
[Red Dead Redemption animations](Documents/Red-Dead-Redemption-Animations.md) for an example.

## Download

Ready-made builds are on the [Releases](https://github.com/lextrack/NXGameViewer/releases) page:

- **Linux:** `NXGameViewer-…-x86_64.AppImage`. Make it executable (`chmod +x`) and run it.
- **Windows:** `NXGameViewer-…-windows-x64.zip`. Unzip it and run `NXGameViewer.exe`.

## Building from source

### Linux

**Option 1: AppImage with Docker (works on any distribution).** Install
[Docker](https://docs.docker.com/engine/install/), then run from the project folder:

```sh
./packaging/linux/make-appimage.sh
```

The script builds everything inside an Ubuntu 22.04 container, runs the tests, checks that the
AppImage starts, and writes `dist/NXGameViewer-…-x86_64.AppImage`. The AppImage runs on most
distributions released since 2022. Set `RUN_TESTS=0` to skip the tests.

**Option 2: build with your distribution's packages.** You need Qt 6.8 or newer, so this works
on Debian 13, Ubuntu 25.04, Fedora 41, Arch and newer (Ubuntu 24.04 has Qt 6.4: use Option 1).

| Distribution | Install |
|---|---|
| Debian, Ubuntu | `sudo apt install build-essential cmake ninja-build pkg-config qt6-base-dev qt6-multimedia-dev libopus-dev libzstd-dev zlib1g-dev libssl-dev libgl-dev` |
| Fedora | `sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config qt6-qtbase-devel qt6-qtmultimedia-devel opus-devel libzstd-devel zlib-devel openssl-devel` |
| Arch | `sudo pacman -S --needed base-devel cmake ninja pkgconf qt6-base qt6-multimedia qt6-multimedia-ffmpeg opus zstd zlib openssl` |

Then build, test and run:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
./build/NXGameViewer
```

The `asan` preset builds with address and undefined-behavior sanitizers, and the `fuzz` preset
builds the fuzzing target (needs clang).

### Windows

The Windows build uses [MSYS2](https://www.msys2.org/), which provides the compiler, Qt and the
libraries. Nothing else is needed (no Visual Studio, no separate Qt download).

1. **Install MSYS2** in its default folder, `C:\msys64`. From PowerShell you can use
   `winget install --id MSYS2.MSYS2 -e`, or the installer from the website.
2. **Update MSYS2 once.** Open **MSYS2 UCRT64** from the Start menu and type `pacman -Syu`.
   If the window closes, open it again and repeat until it says there is nothing to do.
3. **Get the code**, with Git (`git clone https://github.com/lextrack/NXGameViewer.git`) or by
   downloading the ZIP from GitHub (**Code → Download ZIP**) and unzipping it.
4. **Build:** double-click `packaging\windows\build-windows.cmd`.

The first build takes a while because it downloads Qt. When it finishes, the app is in
`dist\NXGameViewer-…-windows-x64.zip`, together with all the DLLs it needs. The zip runs on any
64-bit Windows 10 or 11 PC without installing anything.

If MSYS2 is installed somewhere else, set `MSYS2_ROOT` to its folder before running the script.
From the MSYS2 UCRT64 window you can also run `bash packaging/windows/build-windows.sh`, adding
`--install-deps` the first time so it installs the packages.

### Automatic builds (GitHub Actions)

[`.github/workflows/build.yml`](.github/workflows/build.yml) runs the same two scripts on every
push to `main` and every pull request, so a build that works locally works there too.

The builds can be downloaded from the run's **Artifacts** section. To publish a release, push
a version tag; the workflow builds both and attaches them to a new GitHub release:

```sh
git tag v0.2
git push origin v0.2
```

## Legal

NX Game Viewer contains no game data, keys or copyrighted assets. Use it only with games you own
and keys dumped from your own console.

MIT licence; see [LICENSE](LICENSE). Third-party code and credits are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
