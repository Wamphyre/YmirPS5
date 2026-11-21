# Vulkan presenter for the installable title.
set(_swan "${Ymir_PS5_SWANSTATION_SOURCE}")
set(_vk "${_swan}/src/platform/vk")
set(_fg "${_swan}/third_party/framegen")
set(_radv "${Ymir_PS5_RADV_ROOT}/lib/libvulkan_radeon.ps5.a")
set(_platform "${Ymir_PS5_RADV_SDK}/target/lib/libps5platform.a")
if (NOT EXISTS "${_radv}" OR NOT EXISTS "${_platform}" OR
    NOT EXISTS "${Ymir_PS5_RADV_ROOT}/include/vulkan/vulkan.h")
    message(FATAL_ERROR "Ymir Vulkan needs the RADV release and its companion libps5platform archive")
endif ()
file(SHA256 "${_radv}" _radv_digest)
if (NOT _radv_digest STREQUAL "39921c4e1c19fecc5c7914e022cd10be01e0325ca990cc027960a1777cf85b96")
    message(FATAL_ERROR "Use the RADV archive from deps-radv-7b59ef27 for the SwanStationPS5 presenter")
endif ()

set(_radv_work "${CMAKE_CURRENT_BINARY_DIR}/radv")
file(MAKE_DIRECTORY "${_radv_work}")
set(_radv_sync "${_radv_work}/libvulkan_radeon.ps5.a")
set(_wsi wsi_common_videoout.c.o)
set(_wait winsys_ps5_radv_ps5_sync.c.o)
add_custom_command(OUTPUT "${_radv_sync}"
    COMMAND "${CMAKE_AR}" x "${_radv}" "${_wsi}" "${_wait}"
    COMMAND "${CMAKE_OBJCOPY}" --redefine-sym cnd_timedwait=ss_radv_cnd_timedwait "${_wsi}"
    COMMAND "${CMAKE_OBJCOPY}" --redefine-sym cnd_timedwait=ss_radv_cnd_timedwait "${_wait}"
    COMMAND "${CMAKE_COMMAND}" -E copy "${_radv}" "${_radv_sync}"
    COMMAND "${CMAKE_AR}" rcs "${_radv_sync}" "${_wsi}" "${_wait}"
    WORKING_DIRECTORY "${_radv_work}" DEPENDS "${_radv}" VERBATIM
)

set(_agc_stubs)
foreach (_stub IN ITEMS libSceAgc libSceAgcDriver)
    if (_stub STREQUAL "libSceAgc")
        set(_stub_src "${_swan}/tooling/ps5-stubs/agc_link_stub.c")
    else ()
        set(_stub_src "${_swan}/tooling/ps5-stubs/agc_driver_link_stub.c")
    endif ()
    set(_stub_path "${_radv_work}/${_stub}.so")
    add_custom_command(OUTPUT "${_stub_path}"
        COMMAND "${CMAKE_C_COMPILER}" -fPIC -c "${_stub_src}" -o "${_stub_path}.o"
        COMMAND "${CMAKE_LINKER}" --shared -soname "${_stub}.prx" -o "${_stub_path}" "${_stub_path}.o"
        DEPENDS "${_stub_src}" VERBATIM
    )
    list(APPEND _agc_stubs "${_stub_path}")
    list(APPEND _extra_converter_args --stub "${_stub_path}")
endforeach ()

add_custom_target(ymir-ps5-radv-inputs DEPENDS "${_radv_sync}" ${_agc_stubs})
add_dependencies(ymir-ps5-pie ymir-ps5-radv-inputs)

add_library(ymir-ps5-framegen STATIC "${_fg}/ps5_framegen.cpp" "${_fg}/bridge/fg_bridge.cpp")
target_compile_features(ymir-ps5-framegen PRIVATE cxx_std_20)
target_compile_options(ymir-ps5-framegen PRIVATE -include "${_fg}/bridge/fg_vk.h" -march=znver2)
target_include_directories(ymir-ps5-framegen PRIVATE "${_fg}" "${_fg}/bridge" "${Ymir_PS5_RADV_ROOT}/include")
target_sources(ymir-ps5-pie PRIVATE src/gpu.c "${_vk}/radv_sync.c")
target_include_directories(ymir-ps5-pie PRIVATE "${_vk}" "${_swan}/src" "${Ymir_PS5_RADV_ROOT}/include")
target_compile_definitions(ymir-ps5-pie PRIVATE YMIR_PS5_GPU=1 SwanStationPS5_VULKAN=1)
# Search RADV before the SDK's libc so only one C11 threads implementation is linked.
target_link_options(ymir-ps5-pie PRIVATE -nodefaultlibs)
# Mesa uses weak references for optional entry points. Missing ones must stay null in its dispatch tables.
target_link_options(ymir-ps5-pie PRIVATE "LINKER:-z,nodynamic-undefined-weak")

# Use the pinned driver's platform compatibility recipe for the SDK's missing libc functions.
set(_recipe_root "${_radv_work}/recipe")
file(MAKE_DIRECTORY "${_recipe_root}")
execute_process(
    COMMAND bash -c [=[
        set -e
        source "$1"
        export PS5_CLANG="$2" LLVM_NM="$3"
        radv_link_recipe "$4" "$5" "$6"
        printf '%s\n' "${radv_link_flags[@]}"
    ]=] _ "${_swan}/tooling/radv/radv-link.sh" "${CMAKE_C_COMPILER}" "${CMAKE_NM}"
         "${_recipe_root}" "${Ymir_PS5_RADV_SDK}" "${_radv}"
    OUTPUT_VARIABLE _radv_flags OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY
)
string(REPLACE "\n" ";" _radv_flags "${_radv_flags}")
foreach (_flag IN LISTS _radv_flags)
    if (NOT _flag MATCHES "^--version-script|[.]map$" AND
        NOT _flag MATCHES "^--defsym=(opendir|fdopendir|readdir|rewinddir|dirfd|closedir|access|getaddrinfo|freeaddrinfo|nl_langinfo|nl_langinfo_l|strcasestr)=")
        target_link_options(ymir-ps5-pie PRIVATE "LINKER:${_flag}")
    endif ()
endforeach ()

execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
    OUTPUT_VARIABLE _clang_resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
target_link_libraries(ymir-ps5-pie PRIVATE
    ymir-ps5-framegen
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_radv_sync}>"
    "$<LINK_GROUP:RESCAN,${_platform},${CMAKE_SYSROOT}/lib/libc++.a,${CMAKE_SYSROOT}/lib/libc++abi.a,${CMAKE_SYSROOT}/lib/libunwind.a,${_clang_resource}/lib/linux/libclang_rt.builtins-x86_64.a,${CMAKE_SYSROOT}/lib/libc.a>"
    SceLibcInternal SceNet SceSysmodule SceSystemService
    ${_agc_stubs}
)
list(APPEND _sdk_stubs ${_agc_stubs})

set(_gpu_notice_sources "${_fg}/framegen/LICENSE.txt" "${_swan}/THIRD_PARTY_NOTICES.md"
                        "${Ymir_PS5_RADV_ROOT}/PROVENANCE.txt")
set(_gpu_notice_names AMD-FidelityFX.txt SwanStation-THIRD-PARTY.md RADV-PROVENANCE.txt)
foreach (_i RANGE 2)
    list(GET _gpu_notice_sources ${_i} _notice_source)
    list(GET _gpu_notice_names ${_i} _notice_name)
    set(_notice_output "${_title}/licenses/${_notice_name}")
    list(APPEND _gpu_notice_files "${_notice_output}")
    list(APPEND _gpu_notice_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                                      "${_notice_source}" "${_notice_output}")
endforeach ()
