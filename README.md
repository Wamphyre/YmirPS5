<p align="center"><img src="apps/ymir-ps5/assets/icon0.png" width="160" alt="Emblema de YmirPS5"></p>

# YmirPS5

YmirPS5 es un emulador de Sega Saturn para PlayStation 5. Es un port de [Ymir](https://github.com/ymir-emu/Ymir) con interfaz para DualSense, presentación Vulkan y soporte para CDs de Saturn en lectores ópticos USB.

## Instalación

1. Extrae `YmirPS5-0.6.0-PPSA98567.zip` y copia la carpeta `PPSA98567` completa a la ubicación que escanea tu lanzador de homebrew. Si usa `/data/homebrew/`, el ejecutable quedará en `/data/homebrew/PPSA98567/eboot.bin`.
2. Actualiza el listado del lanzador y abre **YmirPS5** desde Inicio. El primer arranque crea `/data/YmirPS5/` y las carpetas indicadas abajo, siempre que el lanzador permita escribir en esa ruta.
3. Copia un volcado de tu BIOS de Sega Saturn, de **512 KiB (524 288 bytes)**, como `/data/YmirPS5/bios/bios.bin`. Puedes copiarlo mientras el menú está abierto. YmirPS5 vuelve a buscarlo al cargar un juego.
4. Copia las imágenes a `/data/YmirPS5/games/` y elige **Game library**. Para un CD físico, conecta el lector óptico USB, inserta el disco y selecciona **Boot USB disc**.

| Ruta dentro de `/data/YmirPS5/` | Contenido |
| --- | --- |
| `bios/` | BIOS de Saturn, con el nombre `bios.bin` |
| `games/` | Imágenes de disco y sus archivos asociados |
| `covers/` | Carátulas personalizadas opcionales |
| `states/` | Estados rápidos organizados por disco |
| `backup.bin` | Memoria interna de Saturn |
| `memory-transfer.bin` | Archivo para importar o exportar la memoria interna |
| `video.cfg` | Preferencias de vídeo |

Para instalaciones anteriores, también se busca la BIOS en `/data/YmirPS5/bios.bin` y `/download0/bios.bin`. Si usas la BIOS de `/download0/`, el emulador mantiene allí sus guardados siempre que pueda escribir en esa ubicación.

## Jugar

- **Game library** muestra una cuadrícula con carátulas de Saturn que se consultan sin conexión. Busca coincidencias por título o por el número de serie de la imagen. Para añadir o sustituir una carátula, pon un `.png`, `.jpg` o `.jpeg` con el mismo nombre base que la imagen junto al juego o en `covers/`. [Carátulas de Libretro](https://github.com/libretro-thumbnails/Sega_-_Saturn/tree/c512bd123db1307f77b50383fb478b4ddb14fd20/Named_Boxarts)
- La biblioteca admite CHD, BIN/CUE, IMG/CCD/SUB, MDF/MDS e ISO. Mantén los archivos asociados en la misma carpeta y abre el descriptor `.cue`, `.ccd` o `.mds`. Pulsa **○** desde la carpeta principal de juegos para cambiar de ubicación o explorar las unidades USB que exponga el lanzador.
- **Boot USB disc** muestra una animación mientras detecta el lector y abre el disco. El lector debe estar disponible para la aplicación como dispositivo óptico a través de CAM. YmirPS5 puede leer pistas de datos y audio CD-DA.
- **Video settings** permite elegir 1080p, 1440p o 2160p y ajustar la relación de aspecto. Con Vulkan puedes activar el escalado AMD FSR 1 y la generación de fotogramas FSR 3 en modos ×2 o ×4. La interpolación efectiva depende del juego, la pantalla y el modo de salida. Si falla Vulkan, YmirPS5 intenta continuar con VideoOut.
- **Saved games** ofrece cinco ranuras de estado por disco identificado y herramientas para consultar, borrar, exportar e importar la memoria interna de Saturn.

| DualSense | Menú | Sega Saturn |
| --- | --- | --- |
| × | Seleccionar o cambiar un ajuste | A |
| ○ | Volver o reanudar desde la pausa | B |
| □ | Abrir **Game library** desde el menú principal | X |
| △ | Mostrar u ocultar los controles | Y |
| Cruceta o stick izquierdo | Navegar | Cruceta |
| L1 / R1 | Cambiar de página | Z / C |
| L2 / R2 | | L / R |
| Options | | Start |
| Panel táctil | Volver | Abrir el menú de pausa |
| L3 + R3 | Cerrar YmirPS5 | Cerrar YmirPS5 |

## Diagnóstico

La barra lateral indica el estado de la BIOS y la ruta de los juegos. Puedes consultar el backend activo en **Video settings**. Si un CD físico aparece como no disponible, comprueba que el lanzador exponga tanto el lector como el acceso CAM. Si la consola detecta el disco y el emulador no lo identifica, captura las líneas que empiezan por `Ymir PS5:` en el registro de tu lanzador. El nombre del lector, la versión del firmware, el título del juego y el modo de vídeo ayudan a reproducir el problema.

Consulta la [guía de compilación del paquete Vulkan](docs/ps5.md) para generar el ZIP desde el código fuente.

## Licencia

[GNU General Public License v3.0](LICENSE). El paquete incluye los avisos correspondientes a sus dependencias.
