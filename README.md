<p align="center"><img src="docs/images/ymirps5-logo.png" width="200" alt="YmirPS5 logo"></p>

# YmirPS5

YmirPS5 is a Sega Saturn emulator for PlayStation 5. It is a port of [Ymir](https://github.com/ymir-emu/Ymir) with a DualSense interface, Vulkan presentation, and support for Saturn CDs in USB optical drives.

## Installation

1. Extract `YmirPS5-0.6.0-PPSA98567.zip` and copy the entire `PPSA98567` folder to the location your homebrew launcher scans. If it uses `/data/homebrew/`, the executable will be at `/data/homebrew/PPSA98567/eboot.bin`.
2. Refresh the launcher's app list and open **YmirPS5** from the PS5 home screen. On first launch, the emulator creates `/data/YmirPS5/` and the folders listed below if the launcher grants write access.
3. Copy a **512 KiB (524,288 bytes)** Sega Saturn BIOS dump to `/data/YmirPS5/bios/bios.bin`. You can copy it while the menu is open. YmirPS5 checks for it again when you load a game.
4. Copy your disc images to `/data/YmirPS5/games/` and select **Game library**. To play a physical CD, connect your USB optical drive, insert the disc, and select **Boot USB disc**.

| Path under `/data/YmirPS5/` | Contents |
| --- | --- |
| `bios/` | Saturn BIOS named `bios.bin` |
| `games/` | Disc images and their companion files |
| `covers/` | Optional custom box art |
| `states/` | Save states organized by disc |
| `backup.bin` | Saturn internal backup memory |
| `memory-transfer.bin` | File for importing or exporting internal backup memory |
| `video.cfg` | Video settings |

For older installations, the emulator also looks for the BIOS at `/data/YmirPS5/bios.bin` and `/download0/bios.bin`. If you use the BIOS in `/download0/`, the emulator keeps its saves there as long as it has write access.

## Playing games

- **Game library** displays a grid of Saturn box art from an offline catalog. It matches disc images by title or serial number. To add or replace box art, put a `.png`, `.jpg`, or `.jpeg` with the same base name as the disc image next to it or in `covers/`. [Libretro box art](https://github.com/libretro-thumbnails/Sega_-_Saturn/tree/c512bd123db1307f77b50383fb478b4ddb14fd20/Named_Boxarts)
- The library supports CHD, BIN/CUE, IMG/CCD/SUB, MDF/MDS, and ISO. Keep companion files in the same folder and open the `.cue`, `.ccd`, or `.mds` descriptor. Press **○** from the main games folder to switch locations or browse USB drives that the launcher makes available.
- **Boot USB disc** shows an animation while the emulator detects the optical drive and opens the disc. The app must be able to access the drive through CAM. YmirPS5 can read data and CD-DA audio tracks.
- **Video settings** lets you choose 1080p, 1440p, or 2160p and adjust the aspect ratio. Vulkan supports AMD FSR 1 upscaling and FSR 3 frame generation in ×2 or ×4 modes. The frame generation result depends on the game, the display, and the output mode. If Vulkan fails, YmirPS5 attempts to continue with VideoOut.
- **Saved games** offers five save state slots per recognized disc and tools to browse, delete, export, and import Saturn internal backup memory.

| DualSense | Menu | Sega Saturn |
| --- | --- | --- |
| × | Select or change a setting | A |
| ○ | Go back or resume from pause | B |
| □ | Open **Game library** from the main menu | X |
| △ | Show or hide controls | Y |
| D-pad or left stick | Navigate | D-pad |
| L1 / R1 | Change pages | Z / C |
| L2 / R2 | | L / R |
| Options | | Start |
| Touchpad | Go back | Open the pause menu |
| L3 + R3 | Exit YmirPS5 | Exit YmirPS5 |

## Troubleshooting

The sidebar shows the BIOS status and the games folder path. You can check the active graphics backend in **Video settings**. If a physical CD appears unavailable, check that the launcher exposes the drive and grants CAM access. If the console detects a disc that the emulator cannot identify, capture the lines starting with `Ymir PS5:` in your launcher's log. The optical drive model, firmware version, game title, and video mode help reproduce the issue.

To build the ZIP from source, see the [Vulkan package build guide](docs/ps5.md).

## License

[GNU General Public License v3.0](LICENSE). The package includes notices for its dependencies.
