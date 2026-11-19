# Ymir

[![Stable release](https://github.com/ymir-emu/Ymir/actions/workflows/stable-release.yaml/badge.svg)](https://github.com/ymir-emu/Ymir/actions/workflows/stable-release.yaml) [![Nightly release](https://github.com/ymir-emu/Ymir/actions/workflows/nightly-release.yaml/badge.svg)](https://github.com/ymir-emu/Ymir/actions/workflows/nightly-release.yaml) <a href="https://discord.gg/NN3A7n5dzn">![Discord Shield](https://discord.com/api/guilds/1368676375627694341/widget.png?style=shield)</a> <a href="https://patreon.com/StrikerX3">![Patreon Shield](https://img.shields.io/badge/Patreon-F96854?style=flat&logo=patreon&logoColor=white)</a>

A work-in-progress Sega Saturn emulator.

Para probar el port de PS5 con un CD original, consulta la [instalación y diagnóstico](#ps5).

Join the [Discord community](https://discord.gg/NN3A7n5dzn).

Find the official compatibility list [here](https://docs.google.com/spreadsheets/d/1SLZzL9LelSlpEmTKy8cjaQnE7mew2uW1rfCgcekO58Q/edit?usp=sharing).

Grab the latest release: [stable](https://github.com/ymir-emu/Ymir/releases/latest), [nightly](https://github.com/ymir-emu/Ymir/releases/latest-nightly).

> [!IMPORTANT]
> Windows users: install the latest [Microsoft Visual C++ Redistributable package](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) ([x86_64 installer](https://aka.ms/vc14/vc_redist.x64.exe), [AArch64/ARM64 installer](https://aka.ms/vc14/vc_redist.arm64.exe)) before running the emulator.
> This is **mandatory** to avoid crashes on startup.
>
> macOS users: follow [these instructions](https://support.apple.com/guide/mac-help/open-a-mac-app-from-an-unknown-developer-mh40616/mac) to allow Ymir to run on your system. Ymir is signed with an ad-hoc certificate, flagging it as unverified.
>
> Linux users: if you installed the Flatpak version, make sure to [grant it permission to access the filesystem](TROUBLESHOOTING.md#game-discs-dont-load-with-the-flatpak-release) or your disc images won't load properly.

<div class="grid" markdown>
  <img width="49.5%" src="https://github.com/ymir-emu/Ymir/blob/main/docs/images/cd-player.png"/>
  <img width="49.5%" src="https://github.com/ymir-emu/Ymir/blob/main/docs/images/sonic-r.png"/>
  <img width="49.5%" src="https://github.com/ymir-emu/Ymir/blob/main/docs/images/virtua-fighter-2.png"/>
  <img width="49.5%" src="https://github.com/ymir-emu/Ymir/blob/main/docs/images/radiant-silvergun.png"/>
  <img width="49.5%" src="https://github.com/ymir-emu/Ymir/blob/main/docs/images/panzer-dragoon-saga.png"/>
  <img width="49.5%" src="https://github.com/ymir-emu/Ymir/blob/main/docs/images/nights-into-dreams.png"/>
  <img width="100%" src="https://github.com/ymir-emu/Ymir/blob/main/docs/images/debugger.png"/>
</div>


## Features

- Load games from MAME CHD, BIN+CUE, IMG+CCD, MDF+MDS or ISO files
- Automatic IPL (BIOS) ROM detection
- Automatic region switching
- Up to two players with a variety of controllers on both ports
- Fully customizable keybindings
- Backup RAM, DRAM and ROM cartridges (more to come)
- Integrated backup memory manager to import and export saves, and transfer between internal and cartridge RAM
- Forwards-compatible save states
- Rewinding (up to one minute at 60 fps), turbo speed, frame step (forwards and backwards)
- Full screen mode with VRR support and low input lag
- Graphics enhancements such as optional deinterlaced/progressive rendering of high resolution modes and transparent mesh polygon rendering
- Optional low level CD block emulation
- A work-in-progress feature-rich debugger

Ymir runs on Windows 10 or later, macOS 12 (Monterey) or later, most modern and popular Linux distributions and FreeBSD, and supports x86-64 (Intel, AMD) and ARM CPUs.


## PS5

Ymir PS5 es una build de desarrollo con un menú para DualSense, lectura directa de CDs de Saturn por USB y carga de imágenes. El paquete incluye un backend Vulkan con AMD FSR 1 y generación de fotogramas FSR 3. La compilación y el paquete se han comprobado en Linux. El arranque, el lector físico y los modos de vídeo siguen pendientes de verificar en consola.

### Instalar el ZIP y la BIOS

1. Extrae `YmirPS5-PPSA98567.zip` en tu PC. Copia la carpeta `PPSA98567` completa por FTP a `/data/homebrew/` de la PS5, usando el puerto de tu servidor FTP. Con la configuración predeterminada de ShadowMountPlus, la ruta final del ejecutable es `/data/homebrew/PPSA98567/eboot.bin`. La misma carpeta contiene `sce_sys/param.json` y `sce_module/libc.prx`. [Rutas de ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus#scan-paths)
2. Crea `/data/YmirPS5/` en la PS5 y copia el volcado de la BIOS de tu Sega Saturn como `/data/YmirPS5/bios.bin`. Debe medir **512 KiB (524 288 bytes)**. Si Ymir no encuentra la BIOS ahí, también busca `/download0/bios.bin` dentro del entorno de la aplicación. Usa esa ruta alternativa si tu lanzador te permite copiar archivos allí.
3. Si **Ymir PS5** aún no aparece en Inicio, recarga el lanzador homebrew para que vuelva a buscar aplicaciones. Abre Ymir PS5. Para un CD original, conecta el lector óptico USB, inserta el disco y elige **Arrancar disco USB**. Para una imagen, elige **Cargar una imagen**.

El ID de Ymir PS5 es **`PPSA98567`**. SwanStationPS5 usa **`PPSA98510`**. Las dos carpetas pueden coexistir bajo `/data/homebrew/`. [ID de Ymir](apps/ymir-ps5/param.json) · [Instalación de SwanStationPS5](https://github.com/darkxex/SwanStationPS5#getting-started)

### Menú de Ymir PS5

- **Cargar una imagen** admite CHD, BIN/CUE, IMG/CCD/SUB, MDF/MDS e ISO. Copia juegos en `/data/YmirPS5/games/`, junto a la aplicación en `/data/homebrew/PPSA98567/games/` o en una unidad USB que el lanzador exponga a Ymir. Mantén juntos los archivos de cada juego y selecciona su `.cue`, `.ccd` o `.mds` cuando corresponda.
- **Vídeo** ofrece salida 1080p, 1440p o 2160p y relaciones de aspecto 4:3, 16:9, 16:10 o píxeles cuadrados. Con Vulkan también puedes activar **FSR 1** y **generación de fotogramas FSR 3** en x2 o x4. La interpolación efectiva depende del ritmo de fotogramas del juego y de la frecuencia de la pantalla. [Presentador de SwanStationPS5](https://github.com/darkxex/SwanStationPS5/tree/9ccc1dcee0b94758c8389e2cb9c76a3465fe7806/src/platform/vk)
- **Partidas guardadas** permite guardar, cargar y borrar estados en cinco ranuras por disco. Desde **Memoria interna de Saturn** puedes ver y borrar partidas, o exportar e importar la memoria completa mediante `memory-transfer.bin`. Los estados y la exportación se guardan junto a la BIOS si se puede escribir en esa carpeta. Ymir también puede usar `/data/YmirPS5/` o `/download0/` como destino alternativo.

**Cruz** acepta y **círculo** vuelve. La **cruceta** o el **stick** mueven la selección y **L1/R1** pasan una página. Durante el juego, pulsa el **panel táctil** para abrir el menú de pausa. **L3 + R3** cierra Ymir.

### Ver el log del lector USB

Esta build escribe en `stderr` y en la salida de depuración del sistema. Si tu lanzador captura `stderr`, abre su consola. Si ya tienes `klogsrv` activo en la PS5, conéctate desde el PC **antes de arrancar Ymir** y busca las líneas que empiezan por `Ymir PS5:`. Sustituye `192.168.1.50` por la IP de tu consola. [Uso de klogsrv](https://github.com/ps5-payload-dev/klogsrv#usage)

```sh
nc 192.168.1.50 3232 | tee ymir-ps5.log
```

`tee` guarda la captura en el PC. Ymir no crea un archivo de log en la PS5.

| Mensaje | Qué indica |
| --- | --- |
| `Expected a 512 KiB Saturn BIOS at …` | Revisa la ruta, el nombre `bios.bin` y el tamaño. Ymir necesita la BIOS para arrancar el disco o la imagen desde el menú. |
| `Cannot open USB CD drive…` | Ymir no pudo abrir el lector. Comprueba que el entorno de ejecución ve una unidad `/dev/cdN` y permite que Ymir acceda a `/dev/xpt0` y al dispositivo `/dev/passN` asociado. |
| `Connected to /dev/cd0` | Ymir abrió el lector. Puede aparecer aunque la bandeja esté vacía. |
| `Disc present, but no Saturn header could be read` | El lector informó de que hay un disco, pero Ymir no pudo leer o validar su cabecera de Saturn. |
| `Saturn disc: …` | Ymir leyó y reconoció la cabecera. Comprueba que el juego pasa de la BIOS a su pantalla de inicio. |

Consulta la [compilación y los controles de PS5](docs/ps5.md).


## Usage

Grab the latest release [here](https://github.com/ymir-emu/Ymir/releases/latest).
Check the [Releases](https://github.com/ymir-emu/Ymir/releases) page for previous versions.

Ymir does not require installation. Simply download it to any directory and run the executable.
On Windows you also need to install the latest [Microsoft Visual C++ Redistributable package](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) ([x86_64 installer](https://aka.ms/vc14/vc_redist.x64.exe), [AArch64/ARM64 installer](https://aka.ms/vc14/vc_redist.arm64.exe)).
It is recommended to install the latest GPU drivers in order to use Direct3D or Vulkan:
- Nvidia: [Find your GPU here](https://www.nvidia.com/Download/index.aspx?lang=en-us) or [use the Nvidia App to update drivers automatically](https://www.nvidia.com/en-us/software/nvidia-app/)
- AMD: [Find your GPU or CPU here](https://www.amd.com/en/support) or use the download buttons at the top to update drivers automatically
- Intel: [Find your GPU or CPU here](https://www.intel.com/content/www/us/en/download-center/home.html)

The program accepts command-line arguments. Invoke `ymir-sdl3 --help` to list the options:

```
Ymir - Sega Saturn emulator
Usage:
  Ymir [OPTION...] path to disc image

  -d, --disc arg      Path to Saturn disc image (.ccd, .chd, .cue, .iso, 
                      .mds)
  -p, --profile arg   Path to profile directory
  -u, --user          Force user profile
  -h, --help          Display help text
  -f, --fullscreen    Start in fullscreen mode
  -P, --paused        Start paused
  -F, --fast-forward  Start in fast-forward mode
  -D, --debug         Start with debug tracing enabled
  -E, --exceptions    Capture all unhandled exceptions

```

The options are case-sensitive -- lowercase `-p` sets the profile path, uppercase `-P` makes the emulator start paused.

Pass a path to a valid Saturn disc image as an argument to `ymir-sdl3` to launch the emulator with the disc. `-d`/`--disc` is optional.

Use `-p <profile-path>` to point to a separate set of configuration and state files, useful if you wish to have different user profiles (hence the name).

The `-u` option forces usage of the OS's user profile folder (e.g. `C:\Users\<username>\AppData\Roaming\StrikerX3\Ymir` on Windows or `/home/<username>/.local/share/Ymir` on Linux).

`-f` forces the emulator to start in fullscreen mode, ignoring the preference in Ymir.toml.

`-D` starts the emulator with debug tracing enabled.

`-F` starts the emulator in fast-forward mode.

`-E` captures all unhandled exceptions, which can be useful to troubleshoot crashes or failure to start the emulator.

Note that the Windows version does not output anything to the console, but it does honor the command line parameters. You can pipe the output of the command to a file:

```sh
ymir-sdl3 > out.txt
```

Ymir requires an IPL (BIOS) ROM to work. You can place the ROMs under the `roms\ipl` directory created alongside the executable on the first run.
The emulator will scan and automatically select the IPL ROM matching the loaded disc. If no disc is loaded, it will use a ROM matching the first preferred region. Failing that, it will pick whatever is available.
You can override the selection on Settings > IPL.

Ymir can load game disc images from MAME CHD, BIN+CUE, IMG+CCD, MDF+MDS or ISO files. It does not support injecting .elf files directly at the moment.

When using low level CD block emulation (LLE), Ymir also requires the CD block ROM to be placed in `roms\cdb`.


## Troubleshooting

See [TROUBLESHOOTING.md](TROUBLESHOOTING.md) if the emulator crashes or misbehaves.

> [!NOTE]
> The author (@StrikerX3) works primarily with Windows and Linux systems and only provides support for other platforms on a best-effort basis.
> For Linux specifically, only the .tar.xz package on Ubuntu (both native and under WSL) is officially supported by the author.
> If you use custom-built Linux systems or anything that deviates too much from the norm, you're most likely on your own.
>
> The project relies on community support for other Linux packages, macOS and FreeBSD.


## Compiling

See [COMPILING.md](COMPILING.md).


## Support my work

If you enjoy my projects and want to help me keep developing them, consider supporting me:
- [Patreon](https://www.patreon.com/StrikerX3) for ongoing support
- PIX for one-time donations in Brazil: ask me on Discord.

Your support is completely optional but genuinely appreciated. It helps me dedicate more time and energy to these passion projects while keeping everything open-source and free for everyone. Thank you!
