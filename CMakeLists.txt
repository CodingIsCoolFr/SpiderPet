cmake_minimum_required(VERSION 3.21)

# vcpkg: pass -DCMAKE_TOOLCHAIN_FILE, or set VCPKG_ROOT (build.ps1 does this).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "vcpkg toolchain")
endif()
set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg triplet")

project(SpiderPet VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive- /bigobj /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS)
endif()

find_package(imgui CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Everything except the two front ends, so the simulator runs the real code.
add_library(spidercore STATIC
  src/core/util.cpp
  src/see/image.cpp
  src/see/ocr.cpp
  src/see/scroll.cpp
  src/see/vision.cpp
  src/see/window_capture.cpp
  src/see/inspector.cpp
  src/page/page.cpp
  src/pet/spider.cpp
  src/pet/pet.cpp
  src/pet/persona.cpp
  src/render/overlay.cpp
  src/render/paint.cpp
  src/render/scene.cpp
  src/render/offscreen.cpp
)
target_include_directories(spidercore PUBLIC src)
target_link_libraries(spidercore PUBLIC
  d2d1 d3d11 dxgi dcomp dwrite dwmapi windowscodecs windowsapp shcore shell32 ole32 oleaut32 uiautomationcore)

# The Win32 and DX11 ImGui backends are vendored (third_party/imgui_backends,
# MIT) because the installed imgui port only ships the GLFW/OpenGL ones.
add_executable(SpiderPet WIN32
  src/app/main.cpp
  src/app/store.cpp
  src/mind/llm.cpp
  src/ui/panel.cpp
  src/app/spiderpet.rc
  third_party/imgui_backends/imgui_impl_win32.cpp
  third_party/imgui_backends/imgui_impl_dx11.cpp
)
target_include_directories(SpiderPet PRIVATE third_party/imgui_backends)
target_link_libraries(SpiderPet PRIVATE spidercore imgui::imgui nlohmann_json::nlohmann_json winhttp d3dcompiler dxguid)

add_executable(SpiderTool src/app/tool.cpp)
target_link_libraries(SpiderTool PRIVATE spidercore)
