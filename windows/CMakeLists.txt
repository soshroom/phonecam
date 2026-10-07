cmake_minimum_required(VERSION 3.25)
project(PhoneCamWindows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN)

add_executable(PhoneCam WIN32
    src/main.cpp
    src/NetworkReceiver.cpp
    src/H264Decoder.cpp
    src/FrameBridge.cpp
    src/VirtualCamera.cpp
)

target_include_directories(PhoneCam PRIVATE src)
target_link_libraries(PhoneCam PRIVATE
    ws2_32
    mfplat
    mf
    mfuuid
    mfsensorgroup
    ole32
    shlwapi
)

add_library(PhoneCamSource SHARED
    source/PhoneCamMediaSource.cpp
    source/PhoneCamSource.def
)

target_compile_definitions(PhoneCamSource PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PhoneCamSource PRIVATE
    ws2_32
    mfplat
    mf
    mfuuid
    ole32
)

# Avoid requiring a separate VC runtime installation inside Windows Frame Server.
if(MSVC)
    set_property(TARGET PhoneCamSource PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
