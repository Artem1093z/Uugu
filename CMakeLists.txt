cmake_minimum_required(VERSION 3.22.1)
project(ManesTrojanNG)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS ON) 
add_compile_options(-Wno-null-conversion)

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined -Wl,--gc-sections")

# Указываем новый гигантский файл и исходники ImGui
set(SRC_FILES 
    "src/libng.cpp"
    "src/imgui/imgui.cpp"
    "src/imgui/imgui_draw.cpp"
    "src/imgui/imgui_tables.cpp"
    "src/imgui/imgui_widgets.cpp"
    "src/imgui/backends/imgui_impl_opengl3.cpp"
)

include_directories(src src/imgui)

# Собираем как libng.so (эмулятор античита)
add_library(ng SHARED ${SRC_FILES})

# Подключаем графику, логи и инлайн-хукатор (Dobby)
target_link_libraries(ng EGL GLESv3 android log dl ${CMAKE_SOURCE_DIR}/src/libdobby.a)
