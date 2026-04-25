cmake_minimum_required(VERSION 3.18.1)
project(ManesCheatZygisk)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined -Wl,--gc-sections")

# ЯВНО УКАЗЫВАЕМ ТОЛЬКО НУЖНЫЕ ФАЙЛЫ
# Игнорируем imgui_demo.cpp и imgui_freetype.cpp!
set(SRC_FILES 
    "src/main.cpp"
    "src/imgui/imgui.cpp"
    "src/imgui/imgui_draw.cpp"
    "src/imgui/imgui_tables.cpp"
    "src/imgui/imgui_widgets.cpp"
    "src/imgui/backends/imgui_impl_opengl3.cpp"
)

include_directories(src src/imgui)

# Собираем как разделяемую библиотеку (.so)
add_library(manes_zygisk SHARED ${SRC_FILES})

# Подключаем Android библиотеки (EGL, GLES3, log)
target_link_libraries(manes_zygisk EGL GLESv3 android log dl)
