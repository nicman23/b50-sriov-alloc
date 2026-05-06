cmake_minimum_required(VERSION 3.16)
project(b50-sriov-alloc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

add_executable(b50-sriov-alloc main.cpp)
target_link_libraries(b50-sriov-alloc PRIVATE Vulkan::Vulkan)
target_compile_definitions(b50-sriov-alloc PRIVATE VK_USE_PLATFORM_XLIB_KHR)

# Static linking for C++ runtime (libvulkan must remain dynamic)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
