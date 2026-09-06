# WiiCompiled Installer

A graphical installer (GTK3) for [WiiCompiled](https://github.com/patchzyy/Wiicompiled), packaged as an AppImage.

![demo](docs/demo.webp)

### [To download, click here!](https://github.com/FlaipyTheHost/WiiDecompLinuxInstaller/releases/latest)

## Why this project exists

The original WiiCompiled AppImage is, in practice, just a set of manual recompilation instructions — no interface, no validation, and it requires the user to run several steps by hand (extracting the ISO, extracting the NAND, setting up the folder structure, etc). I found that process pretty confusing for anyone who just wants to install and play, so I built this installer to automate all of it behind a simple UI.

## What it does

- Extracts the game's data files from the ISO using **WIT**.
- Optionally extracts a NAND from `nand.bin` + `keys.bin`.
- Sets up the installation at `~/.local/share/WiiCompiled`.
- Detects an existing installation and shows a blue **Play** button to launch the game directly.
- If an installation already exists and the user starts a new one, asks for confirmation before wiping and overwriting it.
- Creates a `.desktop` shortcut in the applications menu after installation, using the `preferences-desktop-gaming` icon.
- Runs as an AppImage, so end users don't need to install any development dependencies.

## Credits and thanks

This project would not be possible without the work of several people and communities:

### Dolphin Emulator

The NAND extraction logic (`bootmii_nand_import`) is based on source code from the [Dolphin project](https://github.com/dolphin-emu/dolphin). I took their original implementation and rewrote it in C++ for standalone use in this installer. Deep thanks to the Dolphin team for the reverse-engineering and implementation work that made this possible.

### Wiimm's ISO Tools (WIT)

Extracting the game's data from the ISO is done using **WIT**, from the [wit.wiimm.de](https://wit.wiimm.de/) project. Without this tool, ISO data extraction would have had to be reimplemented from scratch. Thanks to Wiimm and the community for maintaining it.

### WiiCompiled

Special and heartfelt thanks to the folks at [patchzyy/Wiicompiled](https://github.com/patchzyy/Wiicompiled) for recompiling the game itself. This installer exists solely to make their work more accessible — all credit for the recompilation goes to the original team.

## Building

Build dependencies (Fedora):

```bash
sudo dnf install gtk3-devel
sudo dnf groupinstall "Development Tools"
```

Compile:

```bash
gcc wiicompiled_installer.c -o setup $(pkg-config --cflags --libs gtk+-3.0)
```

## Building the AppImage

This project uses [`appimage-builder`](https://github.com/AppImageCrafters/appimage-builder) via `AppImageBuilder.yml`:

```bash
./appimage-builder-1.1.0-x86_64.AppImage --recipe AppImageBuilder.yml
```

## Expected AppDir structure

```
AppDir/
├── setup.desktop
├── setup.png
└── usr/
    ├── bin/
    │   ├── setup                  # main binary (GTK)
    │   ├── wit                    # Wiimm's ISO Tools
    │   ├── bootmii_nand_import    # NAND extraction (based on Dolphin)
    │   └── WiiCompiled_dist.zip   # recompiled game files
    └── share/
        └── icons/hicolor/256x256/apps/setup.png
```

## Disclaimer

This installer does not distribute any game, ISO, NAND, or keys. Users are responsible for providing their own files, legally obtained from hardware they own.
