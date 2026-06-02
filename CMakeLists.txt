cmake_minimum_required(VERSION 3.20)
project(ScopeFS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(scopefs
  src/main.cpp
  src/util.cpp
  src/trace.cpp
  src/coordinator.cpp
  src/block_device.cpp
  src/kernel.cpp
  src/shell.cpp
  src/terminal.cpp
  src/ui.cpp
)

target_include_directories(scopefs PRIVATE include)

if (MSVC)
  target_compile_options(scopefs PRIVATE /W4 /permissive-)
else()
  target_compile_options(scopefs PRIVATE -Wall -Wextra -Wpedantic)
  if (MINGW)
    target_compile_options(scopefs PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_link_options(scopefs PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
endif()
