# Ymir en PS5

El port tiene un menú propio para DualSense, renderizado de Saturn por software y AudioOut estéreo. El backend Vulkan usa el presentador de SwanStationPS5 para escalar con FSR 1 e interpolar con FSR 3. VideoOut es el backend alternativo. Ymir lee las pistas de datos y audio del CD de Saturn directamente desde un lector óptico USB con CAM/SCSI. Si el lector no entrega el subcanal P–W, Ymir usa la tabla de contenidos para reconstruir la posición del índice 01 de cada pista. Los pregaps y otros índices requieren un lector que entregue el subcanal.

El acceso CAM y la salida VideoOut se adaptaron de SwanStationPS5, que acredita a Al-Azif y PS5SX2 para el acceso a la unidad. El port reutiliza las herramientas de empaquetado y el módulo de compatibilidad de BlackBearReloaded, incluidos en el mismo proyecto. [SwanStationPS5, revisión 9ccc1dc](https://github.com/darkxex/SwanStationPS5/tree/9ccc1dcee0b94758c8389e2cb9c76a3465fe7806)

## Compilar

En Linux o WSL, instala CMake 3.28 o posterior, Ninja y PS5 Payload SDK con las bibliotecas C++ del SDK. Para generar la carpeta de aplicación también necesitas un compilador C++20 para el sistema anfitrión y los encabezados de zlib. [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk)

Desde el repositorio de YmirPS5:

```sh
git submodule update --init --recursive --jobs 4 \
  vendor/fmt vendor/mio vendor/concurrentqueue vendor/xxHash vendor/libchdr vendor/stb

export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
cmake -S . -B build/ps5 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DYmir_DEV_BUILD=OFF -DYmir_ENABLE_IPO=OFF -DYmir_EXTRA_INLINING=OFF -DYmir_AVX2=ON
cmake --build build/ps5 --target ymir-ps5
```

El compilador genera `build/ps5/apps/ymir-ps5/ymir-ps5.elf` para un cargador compatible con PS5 Payload SDK. Si el cargador admite argumentos, usa `ymir-ps5.elf [bios.bin] [/dev/cdN o imagen]`. El segundo argumento también admite formatos de imagen como CUE y CHD.

### Generar la carpeta de aplicación

```sh
git clone https://github.com/darkxex/SwanStationPS5.git ../SwanStationPS5
git -C ../SwanStationPS5 checkout 9ccc1dcee0b94758c8389e2cb9c76a3465fe7806
cmake -S . -B build/ps5 -DYmir_PS5_SWANSTATION_SOURCE="$(realpath ../SwanStationPS5)"
cmake --build build/ps5 --target ymir-ps5-zip
```

El ZIP se genera en `build/ps5/YmirPS5-PPSA98567.zip`. Su carpeta `PPSA98567` contiene `eboot.bin`, `sce_sys/param.json` y el módulo `sce_module/libc.prx` generado desde su código fuente. La compilación de esta sección incluye VideoOut.

### Añadir Vulkan, FSR 1 y generación de fotogramas FSR 3

Descarga `radv-release.tar.zst` y `ps5-payload-sdk.tar.zst` de la publicación indicada. El port usa el SDK que configuraste para compilar Ymir y toma `libps5platform.a` del SDK que acompaña a RADV. [Binarios de Vulkan para PS5](https://github.com/darkxex/SwanStationPS5/releases/tag/deps-radv-7b59ef27)

```sh
mkdir -p build/ps5-deps
tar --zstd -xf radv-release.tar.zst -C build/ps5-deps/
tar --zstd -xf ps5-payload-sdk.tar.zst -C build/ps5-deps/
cmake -S . -B build/ps5 \
  -DYmir_PS5_RADV_ROOT="$(realpath build/ps5-deps/radv-release)" \
  -DYmir_PS5_RADV_SDK="$(realpath build/ps5-deps/ps5-payload-sdk)"
cmake --build build/ps5 --target ymir-ps5-zip
```

El ZIP resultante integra RADV de Mihawk-99, FSR 1 de AMD y la interpolación FSR 3 que SwanStationPS5 adaptó de PS5SX2. Ymir intenta usar Vulkan y vuelve a VideoOut si el backend falla. El ejecutable ELF independiente sigue usando VideoOut. [Procedencia de RADV](https://github.com/darkxex/SwanStationPS5/blob/9ccc1dcee0b94758c8389e2cb9c76a3465fe7806/.github/workflows/radv.yml) · [Créditos de FSR](https://github.com/darkxex/SwanStationPS5/blob/9ccc1dcee0b94758c8389e2cb9c76a3465fe7806/THIRD_PARTY_NOTICES.md)

## Instalación y mando

El [README describe cómo instalar el ZIP, copiar la BIOS y capturar el log](../README.md#ps5). Ymir guarda la memoria interna en `backup.bin` junto a la BIOS si la ruta permite escribir. En modo automático, busca unidades `/dev/cd0` a `/dev/cd15` y da prioridad a una que ya contenga un disco.

La cruceta y el stick izquierdo controlan la dirección. Cruz, círculo y R1 corresponden a A, B y C. Cuadrado, triángulo y L1 corresponden a X, Y y Z. L2 y R2 corresponden a L y R. Opciones es Start. Pulsa L3 y R3 a la vez para salir.

La compilación y el empaquetado se han comprobado en Linux. En consola queda por verificar el arranque, el acceso CAM con el lanzador elegido, la reproducción de un juego con pistas CD-DA y los modos Vulkan en una pantalla compatible.
