include(FetchContent)
FetchContent_Declare(ymir_ps5_cereal
    URL https://codeload.github.com/USCiLab/cereal/tar.gz/refs/tags/v1.3.2
    URL_HASH SHA256=16a7ad9b31ba5880dac55d62b5d6f243c3ebc8d46a3514149e56b5e7ea81f85f
    SOURCE_SUBDIR ymir-headers-only
)
FetchContent_MakeAvailable(ymir_ps5_cereal)

set(_desktop_dir "${CMAKE_CURRENT_SOURCE_DIR}/../ymir-sdl3")
set(_menu_font "${_desktop_dir}/res/embed/fonts/SplineSans-Medium.ttf")
cmrc_add_resource_library(ymir-ps5-font
    NAMESPACE ymir_ps5_font WHENCE "${_desktop_dir}/res/embed/fonts" "${_menu_font}"
)
add_library(ymir-ps5-ui STATIC src/menu.cpp src/storage.cpp)
target_include_directories(ymir-ps5-ui PRIVATE
    "${ymir_ps5_cereal_SOURCE_DIR}/include"
    "${_desktop_dir}/src"
)
target_link_libraries(ymir-ps5-ui PUBLIC ymir::ymir-core PRIVATE ymir-ps5-font)

set(PS5_APP_SOURCES src/main.cpp src/platform.cpp src/video.cpp)
set(PS5_APP_LIBRARIES ymir-ps5-ui SceAudioOut ScePad SceUserService SceVideoOut kernel)

add_executable(ymir-ps5 ${PS5_APP_SOURCES})
target_link_libraries(ymir-ps5 PRIVATE ${PS5_APP_LIBRARIES})
set_target_properties(ymir-ps5 PROPERTIES SUFFIX ".elf")
install(TARGETS ymir-ps5 RUNTIME DESTINATION .)

set(Ymir_PS5_SWANSTATION_SOURCE "" CACHE PATH "Path to SwanStationPS5 for native title packaging")
if (Ymir_PS5_SWANSTATION_SOURCE)
    include(NativeTitle.cmake)
endif ()
