cmake_minimum_required(VERSION 3.20)

project(RemoteLabTrojanExperiment LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This experiment uses Windows APIs and must be built on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(remote_lab_common
    src/common.cpp
)

target_include_directories(remote_lab_common PUBLIC src)

target_compile_features(remote_lab_common PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(remote_lab_common PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(remote_lab_server
    src/server.cpp
)

add_executable(remote_lab_client
    src/client.cpp
)

add_executable(remote_lab_server_gui WIN32
    src/server_gui.cpp
)

add_executable(remote_lab_client_gui WIN32
    src/client_gui.cpp
)

target_link_libraries(remote_lab_server PRIVATE remote_lab_common ws2_32 gdi32 user32)
target_link_libraries(remote_lab_client PRIVATE remote_lab_common ws2_32)
target_link_libraries(remote_lab_server_gui PRIVATE remote_lab_common ws2_32 gdi32 user32 comdlg32)
target_link_libraries(remote_lab_client_gui PRIVATE remote_lab_common ws2_32 user32 comdlg32)

if(MSVC)
    target_compile_options(remote_lab_server PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(remote_lab_client PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(remote_lab_server_gui PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(remote_lab_client_gui PRIVATE /W4 /permissive- /utf-8)
endif()
