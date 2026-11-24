set(_native "${Ymir_PS5_SWANSTATION_SOURCE}/tooling/native")
set(_runtime "${Ymir_PS5_SWANSTATION_SOURCE}/runtime/libc.prx.sha256")
find_program(Ymir_PS5_HOST_CXX NAMES c++ clang++ g++ NO_CMAKE_FIND_ROOT_PATH REQUIRED)

# Use the reference project's CRT and ELF conversion tools with Ymir's own application and title metadata.
function(ymir_ps5_native_executable target)
    add_executable(${target} EXCLUDE_FROM_ALL ${ARGN} "${_native}/app_crt.cpp")
    target_link_options(${target} PRIVATE
        -nostartfiles
        -Qunused-arguments # The SDK wrapper consumes -nostartfiles when selecting startup objects.
        "LINKER:--no-dynamic-linker"
        "LINKER:--build-id=none" # The native converter needs read-only sections to end before writable data.
        "LINKER:-e,_start"
        "LINKER:-T,${_native}/ps5-pie.ld"
        "LINKER:-T,${CMAKE_CURRENT_SOURCE_DIR}/unwind.ld"
        "LINKER:--version-script,${_native}/app-symbols.map"
    )
    set_target_properties(${target} PROPERTIES
        SUFFIX ".elf"
        LINK_DEPENDS "${_native}/ps5-pie.ld;${_native}/app-symbols.map;${CMAKE_CURRENT_SOURCE_DIR}/unwind.ld"
    )
endfunction()

ymir_ps5_native_executable(ymir-ps5-pie ${PS5_APP_SOURCES})
target_link_libraries(ymir-ps5-pie PRIVATE ${PS5_APP_LIBRARIES})

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
set(_title_files "${_title}/eboot.bin" "${_title}/sce_module/libc.prx" "${_title}/sce_sys/param.json"
                 "${_title}/sce_sys/icon0.png" "${_title}/sce_sys/pic0.dds" "${_title}/sce_sys/pic1.dds"
                 "${_title}/sce_sys/snd0.at9" "${_title}/LICENSE" "${_title}/licenses/SplineSans.txt"
                 "${_title}/licenses/cereal.txt" "${_title}/licenses/Cheats-CC-BY-SA-4.0.txt"
                 "${_title}/licenses/Cheats-SOURCE.txt")
set(_param "${CMAKE_CURRENT_SOURCE_DIR}/param.json")
set(_dashboard_background "${CMAKE_CURRENT_SOURCE_DIR}/assets/dashboard.dds")
set(_dashboard_music "${CMAKE_CURRENT_SOURCE_DIR}/assets/snd0.at9")
set(_font_license "${_desktop_dir}/res/licenses/Spline_Sans/OFL.txt")
set(_cereal_license "${ymir_ps5_cereal_SOURCE_DIR}/LICENSE")
set(_cheat_license "${CMAKE_CURRENT_SOURCE_DIR}/assets/cheats-license.txt")
set(_cheat_source "${CMAKE_CURRENT_SOURCE_DIR}/assets/cheats-source.txt")
file(GLOB _sdk_stubs CONFIGURE_DEPENDS "${CMAKE_SYSROOT}/lib/*.so")
include(Radv.cmake)
add_custom_command(OUTPUT ${_title_files} ${_gpu_notice_files}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_title}/sce_sys" "${_title}/sce_module" "${_title}/licenses"
    COMMAND "${_converter}" link --in "$<TARGET_FILE:ymir-ps5-pie>" --out "${_eboot}"
            --stub-dir "${CMAKE_SYSROOT}/lib" ${_extra_converter_args} --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
    COMMAND "${_converter}" self --sign --in "${_eboot}" --out "${_title}/eboot.bin" --magic 0x1D3D154F
    COMMAND "${_libc_builder}" "${_native}/runtime/api-surface.txt" "${_native}/runtime/imports.txt" "${_libc_elf}"
    COMMAND "${_converter}" self --sign --in "${_libc_elf}" --out "${_title}/sce_module/libc.prx"
    COMMAND "${CMAKE_COMMAND}" "-DPRX=${_title}/sce_module/libc.prx" "-DMANIFEST=${_runtime}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntime.cmake"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_param}" "${_title}/sce_sys/param.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_title_icon}" "${_title}/sce_sys/icon0.png"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dashboard_background}" "${_title}/sce_sys/pic0.dds"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dashboard_background}" "${_title}/sce_sys/pic1.dds"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dashboard_music}" "${_title}/sce_sys/snd0.at9"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSE" "${_title}/LICENSE"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_font_license}" "${_title}/licenses/SplineSans.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cereal_license}" "${_title}/licenses/cereal.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cheat_license}" "${_title}/licenses/Cheats-CC-BY-SA-4.0.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_cheat_source}" "${_title}/licenses/Cheats-SOURCE.txt"
    ${_gpu_notice_commands}
    DEPENDS ymir-ps5-pie "${_converter}" "${_libc_builder}" "${_param}" "${_title_icon}" "${_runtime}"
            "${_native}/runtime/api-surface.txt" "${_native}/runtime/imports.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/VerifyRuntime.cmake" "${PROJECT_SOURCE_DIR}/LICENSE"
            "${_dashboard_background}" "${_dashboard_music}"
            "${_font_license}" "${_cereal_license}" "${_cheat_license}" "${_cheat_source}" ${_gpu_notice_sources} ${_sdk_stubs}
    VERBATIM
)
set(Ymir_PS5_COVERS_DIR "" CACHE PATH "Offline cover catalog produced by apps/ymir-ps5/tools/pack_covers.py")
set(_cover_files)
if (Ymir_PS5_COVERS_DIR)
    foreach (_name IN ITEMS index.tsv art.bin source.txt)
        set(_source "${Ymir_PS5_COVERS_DIR}/${_name}")
        set(_output "${_title}/covers/${_name}")
        if (NOT EXISTS "${_source}")
            message(FATAL_ERROR "The PS5 cover catalog is missing ${_source}")
        endif ()
        add_custom_command(OUTPUT "${_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_title}/covers"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_source}" "${_output}"
            DEPENDS "${_source}" VERBATIM)
        list(APPEND _cover_files "${_output}")
    endforeach ()
endif ()

set(_lapy_files)
if (Ymir_FF_HOST_CD_DRIVES)
    set(_lapy_asset_dir "${CMAKE_CURRENT_SOURCE_DIR}/assets")
    set(_lapy_files "${_title}/lapy.elf" "${_title}/lapy-manifest.json" "${_title}/licenses/Lapy-MIT.txt")
    add_custom_command(OUTPUT ${_lapy_files}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_title}/licenses"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_lapy_asset_dir}/lapy.elf" "${_title}/lapy.elf"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_lapy_asset_dir}/lapy-manifest.json" "${_title}/lapy-manifest.json"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_lapy_asset_dir}/Lapy-MIT.txt" "${_title}/licenses/Lapy-MIT.txt"
        DEPENDS "${_lapy_asset_dir}/lapy.elf" "${_lapy_asset_dir}/lapy-manifest.json" "${_lapy_asset_dir}/Lapy-MIT.txt"
        VERBATIM)
endif ()

set(_disc_service_files)
if (Ymir_FF_HOST_CD_DRIVES)
    ymir_ps5_native_executable(ymir-ps5-disc src/disc_service.cpp)
    target_include_directories(ymir-ps5-disc PRIVATE "${PROJECT_SOURCE_DIR}/libs/ymir-core/src")
    # Match the title's native kernel and libc imports without the SDK payload CRT or kernel_web.
    target_link_options(ymir-ps5-disc PRIVATE -nodefaultlibs)
    target_link_libraries(ymir-ps5-disc PRIVATE
        ymir::ymir-core
        "$<LINK_GROUP:RESCAN,${CMAKE_SYSROOT}/lib/libc++.a,${CMAKE_SYSROOT}/lib/libc++abi.a,${CMAKE_SYSROOT}/lib/libunwind.a,${_clang_resource}/lib/linux/libclang_rt.builtins-x86_64.a,${CMAKE_SYSROOT}/lib/libc.a>"
        kernel SceLibcInternal SceNet
    )
    set(_disc_service_elf "${CMAKE_CURRENT_BINARY_DIR}/disc-service.elf")
    set(_disc_service_files "${_title}/tools/ymir-ps5-disc.self")
    add_custom_command(OUTPUT "${_disc_service_files}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_title}/tools"
        COMMAND "${_converter}" link --in "$<TARGET_FILE:ymir-ps5-disc>" --out "${_disc_service_elf}"
                --stub-dir "${CMAKE_SYSROOT}/lib" --module-sdk 0x02000009 --companion-sdk 0x08050001
                --file-name ymir-ps5-disc.elf
        COMMAND "${_converter}" self --sign --in "${_disc_service_elf}" --out "${_disc_service_files}"
                --magic 0x1D3D154F
        DEPENDS ymir-ps5-disc "${_converter}" ${_sdk_stubs} VERBATIM)
endif ()

set(_package_files ${_title_files} ${_gpu_notice_files} ${_cover_files} ${_lapy_files} ${_disc_service_files})
add_custom_target(ymir-ps5-title DEPENDS ${_package_files})
set(_zip_entries)
foreach (_output IN LISTS _package_files)
    file(RELATIVE_PATH _entry "${CMAKE_BINARY_DIR}/title" "${_output}")
    list(APPEND _zip_entries "${_entry}")
endforeach ()
set(_zip "${CMAKE_BINARY_DIR}/YmirPS5-${PROJECT_VERSION}-PPSA98567.zip")
add_custom_command(OUTPUT "${_zip}"
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${_zip}" --format=zip ${_zip_entries}
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/title"
    DEPENDS ${_package_files}
    VERBATIM
)
add_custom_target(ymir-ps5-zip ALL DEPENDS "${_zip}")
