cmake_minimum_required(VERSION 3.20)

# Use the name of the current source directory as the project/target name
get_filename_component(APP_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

project(${APP_NAME})

# Collect all .cpp files in src/ automatically
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_executable(${APP_NAME} ${SOURCES})


target_compile_definitions(${APP_NAME} PRIVATE
    _WIN32_WINNT=0x0603
    WINAPI_FAMILY=WINAPI_FAMILY_PHONE_APP
)

target_include_directories(${APP_NAME} PRIVATE
    "C:/Program Files (x86)/Windows Phone Kits/8.1/Include"
    "C:/Program Files (x86)/Windows Phone Kits/8.1/Include/abi"
    "C:/Program Files (x86)/Windows Phone Kits/8.1/Include/mincore"
    "C:/Program Files (x86)/Windows Phone Kits/8.1/Include/minwin"
    "C:/Program Files (x86)/Microsoft Visual Studio 11.0/VC/WPSDK/include"
)

target_compile_options(${APP_NAME} PRIVATE
    /clang:-fno-sized-deallocation
    "SHELL:/external:I \"C:/Program Files (x86)/Windows Phone Kits/8.1/Include\""
    "SHELL:/external:I \"C:/Program Files (x86)/Windows Phone Kits/8.1/Include/abi\""
    "SHELL:/external:I \"C:/Program Files (x86)/Windows Phone Kits/8.1/Include/mincore\""
    "SHELL:/external:I \"C:/Program Files (x86)/Windows Phone Kits/8.1/Include/minwin\""
    "SHELL:/external:I \"C:/Program Files (x86)/Microsoft Visual Studio 11.0/VC/WPSDK/include\""
    /external:W0
)

set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)

target_link_directories(${APP_NAME} PRIVATE
    "C:/Program Files (x86)/Windows Phone Kits/8.1/lib/ARM"
    "C:/Program Files (x86)/Windows Phone Kits/8.1/lib/winv6.3/um/arm"
    "C:/Program Files (x86)/Microsoft Visual Studio 11.0/VC/WPSDK/lib/arm"
)

target_link_libraries(${APP_NAME} PRIVATE
    mincore.lib
)