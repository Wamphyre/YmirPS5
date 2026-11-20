set(Ymir_PS5_SWANSTATION_SOURCE "" CACHE PATH "Path to the pinned native packaging and Vulkan presenter sources")
set(Ymir_PS5_RADV_ROOT "" CACHE PATH "Path to the pinned RADV distribution")
set(Ymir_PS5_RADV_SDK "" CACHE PATH "Path to the companion SDK with libps5platform.a")
if (NOT Ymir_PS5_SWANSTATION_SOURCE OR NOT Ymir_PS5_RADV_ROOT OR NOT Ymir_PS5_RADV_SDK)
    message(FATAL_ERROR "The YmirPS5 installable package requires the native tools and Vulkan dependencies. See docs/ps5.md.")
endif ()

include(FetchContent)
FetchContent_Declare(ymir_ps5_cereal
    URL https://codeload.github.com/USCiLab/cereal/tar.gz/refs/tags/v1.3.2
    URL_HASH SHA256=16a7ad9b31ba5880dac55d62b5d6f243c3ebc8d46a3514149e56b5e7ea81f85f
    SOURCE_SUBDIR ymir-headers-only
)
FetchContent_MakeAvailable(ymir_ps5_cereal)

set(_desktop_dir "${CMAKE_CURRENT_SOURCE_DIR}/../ymir-sdl3")
set(_menu_font "${_desktop_dir}/res/embed/fonts/SplineSans-Medium.ttf")
set(_title_icon "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon0.png")
cmrc_add_resource_library(ymir-ps5-assets
    NAMESPACE ymir_ps5_assets WHENCE "${_desktop_dir}/res/embed/fonts" "${_menu_font}"
)
cmrc_add_resources(ymir-ps5-assets WHENCE "${CMAKE_CURRENT_SOURCE_DIR}/assets" "${_title_icon}")
add_library(ymir-ps5-ui STATIC src/covers.cpp src/menu.cpp src/storage.cpp)
target_include_directories(ymir-ps5-ui PRIVATE
    "${ymir_ps5_cereal_SOURCE_DIR}/include"
    "${_desktop_dir}/src"
)
target_link_libraries(ymir-ps5-ui PUBLIC ymir::ymir-core PRIVATE ymir-ps5-assets)

set(PS5_APP_SOURCES src/main.cpp src/platform.cpp src/video.cpp)
set(PS5_APP_LIBRARIES ymir-ps5-ui SceAudioOut ScePad SceUserService SceVideoOut kernel)

include(NativeTitle.cmake)
