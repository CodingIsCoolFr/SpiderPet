# Rebuilds SpiderPet and puts SpiderPet.exe next to this script.
# Needs Visual Studio 2022 (C++), CMake, and vcpkg with imgui and nlohmann-json
# for x64-windows. vcpkg is found through VCPKG_ROOT or ~\vcpkg.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$toolchain = Join-Path $vcpkg "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) { throw "vcpkg not found. Set VCPKG_ROOT to your vcpkg folder." }
cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }
# A running exe cannot be overwritten, but it can be renamed out of the way.
Remove-Item "$root\SpiderPet.old.exe" -Force -ErrorAction SilentlyContinue
if (Test-Path "$root\SpiderPet.exe") {
  try { Remove-Item "$root\SpiderPet.exe" -Force -ErrorAction Stop }
  catch { Rename-Item "$root\SpiderPet.exe" "SpiderPet.old.exe" }
}
Copy-Item "$root\build\Release\SpiderPet.exe" "$root\SpiderPet.exe" -Force
Write-Host "Built $root\SpiderPet.exe (restart SpiderPet if it was open)"
