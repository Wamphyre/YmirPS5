set(_native "${Ymir_PS5_SWANSTATION_SOURCE}/tooling/native")
set(_runtime "${Ymir_PS5_SWANSTATION_SOURCE}/runtime/libc.prx.sha256")
find_program(Ymir_PS5_HOST_CXX NAMES c++ clang++ g++ NO_CMAKE_FIND_ROOT_PATH REQUIRED)

# Use the reference project's CRT and ELF conversion tools with Ymir's own application and title metadata.
add_executable(ymir-ps5-pie EXCLUDE_FROM_ALL ${PS5_APP_SOURCES} "${_native}/app_crt.cpp")
target_link_libraries(ymir-ps5-pie PRIVATE ${PS5_APP_LIBRARIES})
target_compile_definitions(ymir-ps5-pie PRIVATE YMIR_PS5_NATIVE_TITLE=1)
target_link_options(ymir-ps5-pie PRIVATE
    -nostartfiles
    -Qunused-arguments # The SDK wrapper consumes -nostartfiles when selecting startup objects.
    "LINKER:--no-dynamic-linker"
    "LINKER:-e,_start"
    "LINKER:-T,${_native}/ps5-pie.ld"
    "LINKER:-T,${CMAKE_CURRENT_SOURCE_DIR}/unwind.ld"
    "LINKER:--version-script,${_native}/app-symbols.map"
)
set_target_properties(ymir-ps5-pie PROPERTIES
    SUFFIX ".elf"
    LINK_DEPENDS "${_native}/ps5-pie.ld;${_native}/app-symbols.map;${CMAKE_CURRENT_SOURCE_DIR}/unwind.ld"
)

set(_converter "${CMAKE_CURRENT_BINARY_DIR}/ps5-native-tool")
set(_converter_sources
    "${_native}/native_app_builder.cpp"
    "${_native}/elf_object.cpp"
    "${_native}/sce_module_writer.cpp"
    "${_native}/self_container.cpp"
)
add_custom_command(OUTPUT "${_converter}"
    COMMAND "${Ymir_PS5_HOST_CXX}" -std=c++20 -O2 ${_converter_sources} -lz -o "${_converter}"
    DEPENDS ${_converter_sources} "${_native}/elf_object.hpp" "${_native}/sce_module_writer.hpp"
            "${_native}/self_container.hpp" "${_native}/hash.hpp"
    VERBATIM
)
set(_libc_builder "${CMAKE_CURRENT_BINARY_DIR}/libc-builder")
add_custom_command(OUTPUT "${_libc_builder}"
    COMMAND "${Ymir_PS5_HOST_CXX}" -std=c++20 -O2 "${_native}/libc_builder.cpp" -o "${_libc_builder}"
    DEPENDS "${_native}/libc_builder.cpp"
    VERBATIM
)

set(_eboot "${CMAKE_CURRENT_BINARY_DIR}/eboot.elf")
set(_libc_elf "${CMAKE_CURRENT_BINARY_DIR}/libc.elf")
set(_title "${CMAKE_BINARY_DIR}/title/PPSA98567")
set(_param "${CMAKE_CURRENT_SOURCE_DIR}/param.json")
set(_font_license "${_desktop_dir}/res/licenses/Spline_Sans/OFL.txt")
set(_cereal_license "${ymir_ps5_cereal_SOURCE_DIR}/LICENSE")
file(GLOB _sdk_stubs CONFIGURE_DEPENDS "${CMAKE_SYSROOT}/lib/*.so")
include(Radv.cmake)
add_custom_command(OUTPUT "${_title}/eboot.bin" "${_title}/sce_module/libc.prx" "${_title}/sce_sys/param.json"
                         "${_title}/LICENSE" "${_title}/licenses/SplineSans.txt" "${_title}/licenses/cereal.txt"
                         ${_gpu_notice_files}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_title}/sce_sys" "${_title}/sce_module" "${_title}/licenses"
    COMMAND "${_converter}" link --in "$<TARGET_FILE:ymir-ps5-pie>" --out "${_eboot}"
            --stub-dir "${CMAKE_SYSROOT}/lib" ${_extra_converter_args} --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
    COMMAND "${_converter}" self --sign --in "${_eboot}" --out "${_title}/eboot.bin" --magic 0x1D3D154F
    COMMAND "${_libc_builder}" "${_native}/runtime/api-surface.txt" "${_native}/runtime/imports.txt" "${_libc_elf}"
    COMMAND "${_converter}" self --sign --in "${_libc_elf}" --out "${_title}/sce_module/libc.prx"
    COMMAND "${CMAKE_COMMAND}" "-DPRX=${_title}/sce_module/libc.prx" "-DMANIFEST=${_runtime}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntime.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_param}" "${_title}/sce_sys/param.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSE" "${_title}/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_font_license}" "${_title}/licenses/SplineSans.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cereal_license}" "${_title}/licenses/cereal.txt"
    ${_gpu_notice_commands}
    DEPENDS ymir-ps5-pie "${_converter}" "${_libc_builder}" "${_param}" "${_runtime}"
            "${_native}/runtime/api-surface.txt" "${_native}/runtime/imports.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntime.cmake" "${PROJECT_SOURCE_DIR}/LICENSE"
            "${_font_license}" "${_cereal_license}" ${_gpu_notice_sources} ${_sdk_stubs}
    VERBATIM
)
add_custom_target(ymir-ps5-title DEPENDS "${_title}/eboot.bin" "${_title}/sce_module/libc.prx"
                                       "${_title}/sce_sys/param.json" "${_title}/LICENSE"
                                       "${_title}/licenses/SplineSans.txt" "${_title}/licenses/cereal.txt"
                                       ${_gpu_notice_files})
add_custom_target(ymir-ps5-zip
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${CMAKE_BINARY_DIR}/YmirPS5-PPSA98567.zip" --format=zip PPSA98567
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/title" DEPENDS ymir-ps5-title VERBATIM
)
