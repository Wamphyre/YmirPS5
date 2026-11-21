<p align="center"><img src="docs/images/ymirps5-logo.png" width="200" alt="YmirPS5 logo"></p>

# YmirPS5

YmirPS5 is a Sega Saturn emulator for PlayStation 5. It is a port of [Ymir](https://github.com/ymir-emu/Ymir) with a DualSense interface, Vulkan presentation, and support for Saturn CDs in USB optical drives.

## Installation

1. Extract `YmirPS5-0.7.2-PPSA98567.zip` and copy the entire `PPSA98567` folder to the location your homebrew launcher scans. If it uses `/data/homebrew/`, the executable will be at `/data/homebrew/PPSA98567/eboot.bin`.
2. Refresh the launcher's app list and select **YmirPS5** from the PS5 home screen to see its original Saturn-themed artwork and hear *Cold Orbit*, the selection music. Open it to start the emulator. On first launch, it creates `/data/YmirPS5/` and the folders listed below if the launcher grants write access.
3. Copy a **512 KiB (524,288 bytes)** Sega Saturn BIOS dump to `/data/YmirPS5/bios/boot.bin`. You can copy it while the menu is open. YmirPS5 checks for it again when you load a game.
4. Copy your disc images to `/data/YmirPS5/games/` and select **Game library**. For an original Saturn CD, follow the [USB optical drive setup](#usb-optical-drive).

| Path under `/data/YmirPS5/` | Contents |
| --- | --- |
| `bios/` | Saturn BIOS named `boot.bin` or `bios.bin` |
| `games/` | Disc images and their companion files |
| `covers/` | Optional custom box art |
| `states/` | Save states organized by disc |
| `backup.bin` | Saturn internal backup memory |
| `saturn.cfg` | Saturn clock offset, language, and BIOS system settings |
| `memory-transfer.bin` | File for importing or exporting internal backup memory |
| `video.cfg` | Video settings |
| `audio.cfg` | Game volume, menu sound volume, and mute |

The emulator also accepts either filename directly under `/data/YmirPS5/` or `/download0/`. If you use the BIOS in `/download0/`, the emulator keeps its saves there as long as it has write access.

## Updating an existing installation

Close YmirPS5 on the PS5. With its FTP server running, use the release ZIP and the console's IP address with `apps/ymir-ps5/tools/update_ps5.py`. The tool verifies the package and the staged uploads, then replaces changed title files. User settings, BIOS files, games, and saves stay in their existing directories.

```sh
python3 apps/ymir-ps5/tools/update_ps5.py YmirPS5-0.7.2-PPSA98567.zip 192.0.2.1
```

Add `--port PORT` if your FTP server uses a port other than 1337. Follow your loader's registration refresh procedure if an update introduces new dashboard media.

If the launcher prevents YmirPS5 from listing `/data/YmirPS5/games/`, generate an index from the files already on the console. Reopen **Game library** after refreshing it.

```sh
python3 apps/ymir-ps5/tools/update_ps5.py --index-games 192.0.2.1
```

## Playing games

- **Game library** displays a grid of Saturn box art from an offline catalog. It matches disc images by title or serial number. To add or replace box art, put a `.png`, `.jpg`, or `.jpeg` with the same base name as the disc image next to it or in `covers/`. [Libretro box art](https://github.com/libretro-thumbnails/Sega_-_Saturn/tree/c512bd123db1307f77b50383fb478b4ddb14fd20/Named_Boxarts)
- The library supports CHD, BIN/CUE, IMG/CCD/SUB, MDF/MDS, and ISO. Keep companion files in the same folder and open the `.cue`, `.ccd`, or `.mds` descriptor. Press **○** from the main games folder to switch locations or browse USB drives that the launcher makes available.
- **Boot USB disc** shows an animation while the emulator detects the optical drive and opens the disc. See [USB optical drive](#usb-optical-drive).
- **Boot Saturn BIOS** starts the Sega Saturn system menu with an empty virtual CD drive. The first setup saves your clock and language for subsequent sessions. In the CD player, open **System Settings** and **Memory Manager** to manage the same internal memory used by Saturn games. The emulator asks for confirmation before ending a paused session. [Sega Saturn instruction manual](https://www.manualslib.com/manual/4140863/Sega-Saturn.html)
- **Video settings** shows the saved game resolution and aspect ratio in the main menu. **Native** uses the pixels produced by the Saturn video hardware with integer vertical scaling and the selected aspect ratio. **1080p**, **1440p**, and **4K (2160p)** set the game's upscaled presentation resolution. Vulkan supports optional AMD FSR 1 in those three modes and FSR 3 frame generation in ×2 or ×4 modes. If Vulkan fails, YmirPS5 attempts to continue with VideoOut while retaining your preferences.
- The emulator menu renders separately at **1920 × 1080** and scales automatically to 4K if the PS5 is outputting 2160p when YmirPS5 starts.
- **Audio settings** saves separate volumes for Saturn audio and the menu's navigation, confirm, and back cues. **Mute all** silences audio inside the emulator. The PS5 home screen controls the dashboard music.
- **Saved games** offers five save state slots per recognized disc and tools to browse, delete, export, and import Saturn internal backup memory.

| DualSense | Menu | Sega Saturn |
| --- | --- | --- |
| × | Select or change a setting | A |
| ← / → | Adjust the value in Video or Audio settings | D-pad |
| ○ | Go back or resume from pause | B |
| □ | Open **Game library** from the main menu | X |
| △ | Show or hide controls | Y |
| D-pad or left stick | Navigate | D-pad |
| L1 / R1 | Change pages | Z / C |
| L2 / R2 | | L / R |
| Options | | Start |
| Touchpad | Go back or resume | Open the game menu |
| L3 + R3 | | Open the game menu |

The game menu offers **Resume**, **Close game**, **Restart game**, **Save state**, **Load state**, and **Exit YmirPS5**. Close game returns to the emulator's Home screen. Save and load use the same five slots per disc as **Saved games**.

## USB optical drive

Connect the USB optical drive, insert an original Saturn CD, and select **Boot USB disc**. YmirPS5 reads the track list and Saturn header before starting the game. It streams data and CD-DA sectors directly from the reader with an in-memory cache. The disc stays in the drive during play.

YmirPS5 needs access to `/dev` to open the reader. At startup it first asks etaHEN, OnionHEN, LegacyJB or PS5SX2 Helper for an on-demand jailbreak of the title. If none answers, it sends the packaged `PPSA98567/lapy.elf` helper to the console's ELF loader at `127.0.0.1:9021`; this Lapy helper is built specifically for `PPSA98567` and completes a credential exchange with the app. Once elevated, YmirPS5 opens the USB reader through CAM directly. If neither route is available, it falls back to the packaged `PPSA98567/tools/ymir-ps5-disc.self` service through the same loader and retries; you can also start that service manually from a native environment that already grants CAM access, wait for `Ready for YmirPS5 on loopback` in `/data/YmirPS5/disc-service.log`, and leave it running while using **Boot USB disc**. If automatic reader selection fails, the service accepts the CAM device path as its only argument.

## Troubleshooting

The sidebar shows the BIOS status and the games folder path. You can check the active graphics backend in **Video settings**. If a physical CD appears unavailable, check `/data/YmirPS5/unlock.log` first: it records whether etaHEN or the Lapy helper granted `/dev` access. Then check `/data/YmirPS5/usb.log` for direct CAM results and the automatic service start. When no unlock service and no ELF loader are available, the log says so; enable etaHEN's on-demand jailbreak or start an ELF loader and retry. If the service cannot open the drive, check `/data/YmirPS5/disc-service.log` for the failing device path. The optical drive model, console firmware version, and game title help reproduce the issue.

If the menu does not appear, check `/data/YmirPS5/launch.log` for the last completed step. A failed **Boot USB disc** attempt writes reader detection details to `/data/YmirPS5/usb.log`. After closing the app, select another tile and then highlight YmirPS5 again to restart the dashboard track.

To build the ZIP from source, see the [Vulkan package build guide](docs/ps5.md).

## License

[GNU General Public License v3.0](LICENSE). The package includes notices for its dependencies.
