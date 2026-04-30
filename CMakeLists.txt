cmake_minimum_required(VERSION 3.22.1)
project(FakeLibNG)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined -Wl,--gc-sections")

# Компилируем наш фейковый античит
add_library(ng SHARED src/libng.cpp)

# Подключаем только логгер (больше ничего не нужно)
target_link_libraries(ng log)
