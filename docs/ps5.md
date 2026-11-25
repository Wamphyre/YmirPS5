# Compilar YmirPS5

La compilación de PS5 genera un único paquete instalable, `YmirPS5-0.7.4-PPSA98567.zip`. Vulkan forma parte del paquete. VideoOut proporciona una ruta de recuperación en tiempo de ejecución.

## Dependencias

En Linux o WSL se necesitan CMake 3.28 o posterior, Ninja, LLVM 21, un compilador anfitrión C++20 con zlib y el [PS5 Payload SDK v0.50](https://github.com/ps5-payload-dev/sdk/releases/tag/v0.50) con sus bibliotecas de C++. Para la presentación Vulkan y el empaquetado se usan estas dependencias fijadas.

- [Código del presentador y herramientas del título nativo, revisión 9ccc1dc](https://github.com/darkxex/SwanStationPS5/tree/9ccc1dcee0b94758c8389e2cb9c76a3465fe7806).
- [RADV y SDK complementario, publicación deps-radv-7b59ef27](https://github.com/darkxex/SwanStationPS5/releases/tag/deps-radv-7b59ef27). Extrae `radv-release.tar.zst` y `ps5-payload-sdk.tar.zst` en un directorio de dependencias separado. El SDK de este archivo aporta `libps5platform.a`.

## Generar el ZIP

Desde la raíz del repositorio, apunta cada variable al directorio que corresponda en tu sistema.

```sh
git submodule update --init --recursive --jobs 4 \
  vendor/fmt vendor/mio vendor/concurrentqueue vendor/xxHash vendor/libchdr vendor/stb

cmake -S . -B build/ps5 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/ps5-sdk/toolchain/prospero.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DYmir_DEV_BUILD=OFF -DYmir_ENABLE_IPO=ON -DYmir_EXTRA_INLINING=ON -DYmir_AVX2=ON \
  -DYmir_PS5_SWANSTATION_SOURCE=/path/to/native-title-sources \
  -DYmir_PS5_RADV_ROOT=/path/to/radv-release \
  -DYmir_PS5_RADV_SDK=/path/to/companion/ps5-payload-sdk
cmake --build build/ps5 --target ymir-ps5-zip
```

La configuración descarga los encabezados de cereal 1.3.2. Para una compilación sin red, usa `-DFETCHCONTENT_SOURCE_DIR_YMIR_PS5_CEREAL=/path/to/cereal-1.3.2`.

La build PS5 compila para la CPU Zen 2 de la consola con `-march=znver2 -mtune=znver2` y activa LTO/IPO e inlining agresivo. Es la configuración usada para el paquete de distribución.

El ZIP aparece en `build/ps5/` y contiene la carpeta `PPSA98567`, con `eboot.bin`, `sce_module/libc.prx`, los recursos del dashboard en `sce_sys/` y los avisos de licencia. El catálogo de trucos se incrusta en `eboot.bin`. Si se compila con soporte de discos físicos, incluye el helper Lapy `lapy.elf` con su manifiesto, el ejecutable nativo `tools/ymir-ps5-disc.self` y la licencia MIT de Lapy.

El fondo original está en `apps/ymir-ps5/assets/dashboard.png`. La compilación copia su versión BC7 3840 × 2160 como `sce_sys/pic0.dds` para la selección y `sce_sys/pic1.dds` para el inicio. Incluye también `sce_sys/snd0.at9`, la versión en bucle ATRAC9 de *Cold Orbit*. La partitura y el sintetizador del tema están en `apps/ymir-ps5/tools/dashboard_music.py`.

## Servicio del lector óptico

`ymir-ps5-disc.self` arranca como un proceso nativo y verifica que su entorno ya puede abrir el lector CAM. Atiende a YmirPS5 en la interfaz local de la consola. Admite comprobación de presencia, lectura de la tabla de pistas y lectura de sectores con datos, audio o subcanales. El juego se ejecuta desde el CD y la precarga de sectores se mantiene en memoria.

Al arrancar, YmirPS5 pide primero un jailbreak on-demand del título a etaHEN, OnionHEN, LegacyJB o PS5SX2 Helper. Si ninguno responde, envía el helper empaquetado `lapy.elf` al cargador ELF local en `127.0.0.1:9021`. Ese helper Lapy está compilado específicamente para `PPSA98567` y completa el intercambio de credenciales con la app. Con el proceso ya elevado, YmirPS5 abre el lector USB directamente por CAM. Si ninguna de las dos rutas está disponible, usa el servicio `tools/ymir-ps5-disc.self` a través del mismo cargador y reintenta. También puedes iniciar el servicio manualmente desde un entorno nativo con acceso CAM y dejarlo en ejecución mientras seleccionas **Boot USB disc**. El servicio acepta la ruta del dispositivo CAM como único argumento.

## Incluir el catálogo de carátulas

El ZIP de distribución incluye un catálogo offline. Para regenerarlo necesitas Python 3, Pillow y acceso a GitHub durante la descarga. La caché permite reutilizar miniaturas al actualizar el catálogo. Las compilaciones que prescinden del catálogo pueden seguir usando carátulas personalizadas.

```sh
python3 apps/ymir-ps5/tools/pack_covers.py build/ps5/cover-catalog \
  --cache build/cover-cache --revision c512bd123db1307f77b50383fb478b4ddb14fd20
cmake -S . -B build/ps5 -DYmir_PS5_COVERS_DIR="$(realpath build/ps5/cover-catalog)"
cmake --build build/ps5 --target ymir-ps5-zip
```

Consulta el [README](../README.md) para instalarlo y preparar los archivos del primer arranque.
