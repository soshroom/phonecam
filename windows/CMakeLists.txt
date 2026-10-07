cmake_minimum_required(VERSION 3.25)
project(PhoneCamWindows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN)

add_executable(PhoneCam WIN32
    src/main.cpp
    src/NetworkReceiver.cpp
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
