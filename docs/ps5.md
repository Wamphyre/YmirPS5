# Compilar YmirPS5

La compilación de PS5 genera un único paquete instalable, `YmirPS5-0.6.0-PPSA98567.zip`. Vulkan forma parte del paquete. VideoOut proporciona una ruta de recuperación en tiempo de ejecución.

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
  -DYmir_DEV_BUILD=OFF -DYmir_ENABLE_IPO=OFF -DYmir_EXTRA_INLINING=OFF -DYmir_AVX2=ON \
  -DYmir_PS5_SWANSTATION_SOURCE=/path/to/native-title-sources \
  -DYmir_PS5_RADV_ROOT=/path/to/radv-release \
  -DYmir_PS5_RADV_SDK=/path/to/companion/ps5-payload-sdk
cmake --build build/ps5 --target ymir-ps5-zip
```

La configuración descarga los encabezados de cereal 1.3.2. Para una compilación sin red, usa `-DFETCHCONTENT_SOURCE_DIR_YMIR_PS5_CEREAL=/path/to/cereal-1.3.2`.

El ZIP aparece en `build/ps5/` y contiene la carpeta `PPSA98567`, con `eboot.bin`, `sce_sys/param.json`, `sce_sys/icon0.png`, `sce_module/libc.prx` y los avisos de licencia. El icono del título se incrusta también en el menú.

## Incluir el catálogo de carátulas

El ZIP de distribución incluye un catálogo offline. Para regenerarlo necesitas Python 3, Pillow y acceso a GitHub durante la descarga. La caché permite reutilizar miniaturas al actualizar el catálogo. Las compilaciones que prescinden del catálogo pueden seguir usando carátulas personalizadas.

```sh
python3 apps/ymir-ps5/tools/pack_covers.py build/ps5/cover-catalog \
  --cache build/cover-cache --revision c512bd123db1307f77b50383fb478b4ddb14fd20
cmake -S . -B build/ps5 -DYmir_PS5_COVERS_DIR="$(realpath build/ps5/cover-catalog)"
cmake --build build/ps5 --target ymir-ps5-zip
```

Consulta el [README](../README.md) para instalarlo y preparar los archivos del primer arranque.
