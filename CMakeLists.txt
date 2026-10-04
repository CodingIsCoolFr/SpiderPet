cmake_minimum_required(VERSION 3.21)

# vcpkg: pass -DCMAKE_TOOLCHAIN_FILE, or set VCPKG_ROOT (build.ps1 does this).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "vcpkg toolchain")
endif()
set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")

project(SpiderPet VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive- /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS)
endif()

find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# The checker and the browser connection, shared by the app and the relay.
add_library(spidercore STATIC
  src/core/util.cpp
  src/mind/net.cpp
  src/mind/check.cpp
  src/app/bridge.cpp
  src/app/library.cpp
)
target_include_directories(spidercore PUBLIC src)
target_link_libraries(spidercore PUBLIC nlohmann_json::nlohmann_json winhttp shell32 ole32 advapi32)

# SpiderPet.exe: the window, the tray icon, the library and the checks.
# The Win32 and DX11 ImGui backends are vendored (third_party/imgui_backends,
# MIT) because the installed imgui port only ships the GLFW/OpenGL ones.
add_executable(SpiderPet WIN32
  src/app/main.cpp
  src/app/spiderpet.rc
  third_party/imgui_backends/imgui_impl_win32.cpp
  third_party/imgui_backends/imgui_impl_dx11.cpp
)
target_include_directories(SpiderPet PRIVATE third_party/imgui_backends)
target_link_libraries(SpiderPet PRIVATE spidercore imgui::imgui d3d11 dxgi dwmapi d3dcompiler dxguid)

# SpiderHost.exe: started by the browser, relays the extension to the app.
add_executable(SpiderHost src/app/host.cpp)
target_link_libraries(SpiderHost PRIVATE spidercore)
