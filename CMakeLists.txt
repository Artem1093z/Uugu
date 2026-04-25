cmake_minimum_required(VERSION 3.18.1)
project(ManesCheatZygisk)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined -Wl,--gc-sections")

# Ищем основной код (main.cpp) и базовые файлы ImGui, НЕ заходя в папку backends
file(GLOB SRC_FILES 
    "src/*.cpp" 
    "src/imgui/*.cpp"
)

# Точечно добавляем ТОЛЬКО бэкенд OpenGL 3, который нужен для Android
list(APPEND SRC_FILES "src/imgui/backends/imgui_impl_opengl3.cpp")

include_directories(src src/imgui)

# Собираем как разделяемую библиотеку (Zygisk требует .so)
add_library(manes_zygisk SHARED ${SRC_FILES})

# Подключаем Android библиотеки (EGL, GLES3, log)
target_link_libraries(manes_zygisk EGL GLESv3 android log dl)
