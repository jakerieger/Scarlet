set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Vendor/imgui)
add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dxgi dwmapi)

add_subdirectory(Scarlet)
