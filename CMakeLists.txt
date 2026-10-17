cmake_minimum_required(VERSION 3.26)
project(Scarlet VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32)
    message(FATAL_ERROR "Scarlet is a Windows application (Direct3D 11).")
endif ()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(FetchContent)

FetchContent_Declare(
        libSouls
        GIT_REPOSITORY https://github.com/jakerieger/libSouls.git
        GIT_TAG 6ccd8dc6dc1634a493d26e14d4aa7fa89f7cb6c6
)
FetchContent_MakeAvailable(libSouls)

add_subdirectory(Source)
