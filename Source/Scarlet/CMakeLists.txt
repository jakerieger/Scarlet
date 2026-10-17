add_executable(Scarlet WIN32
        ${CMAKE_SOURCE_DIR}/Resource/App.rc
        ${CMAKE_SOURCE_DIR}/Resource/Resource.h
        ${CMAKE_SOURCE_DIR}/Resource/Version.h
        App.cpp
        App.hpp
        Camera.cpp
        Camera.hpp
        Edit.cpp
        Edit.hpp
        GameData.cpp
        GameData.hpp
        Main.cpp
        Model.cpp
        Model.hpp
        Player.cpp
        Player.hpp
        Renderer.cpp
        Renderer.hpp
        Save.cpp
        Save.hpp
)

target_link_libraries(Scarlet PRIVATE Souls imgui d3d11 d3dcompiler dxgi shell32 ole32)
target_compile_definitions(Scarlet PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(Scarlet PRIVATE "${CMAKE_SOURCE_DIR}/Resource")

# Elden Ring's archive keys and path dictionaries (from the BinderKeys submodule) go next to the executable, for
# reading the game's packed archives.
set(BINDERKEYS_DIR ${PROJECT_SOURCE_DIR}/Vendor/BinderKeys)
if (NOT EXISTS ${BINDERKEYS_DIR}/EldenRing_PC)
    message(WARNING "Vendor/BinderKeys is missing; run `git submodule update --init`. Without it Scarlet can only "
            "read unpacked games.")
else ()
    add_custom_command(TARGET Scarlet POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${BINDERKEYS_DIR}/EldenRing_PC $<TARGET_FILE_DIR:Scarlet>/BinderKeys/EldenRing_PC
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${BINDERKEYS_DIR}/LICENSE $<TARGET_FILE_DIR:Scarlet>/BinderKeys/LICENSE
            VERBATIM)
endif ()

if (MSVC)
    # TAE3Events.hpp (through TAE3.hpp) holds a huge variant of event types.
    target_compile_options(Scarlet PRIVATE /W4 /permissive- /utf-8 /bigobj)
endif ()
