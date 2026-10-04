# Rebuilds SpiderPet and puts SpiderPet.exe, SpiderHost.exe and the browser
# extension (extension\chromium, extension\firefox) next to this script.
# Needs Visual Studio 2022 (C++), CMake and vcpkg (found through VCPKG_ROOT or
# ~\vcpkg). vcpkg.json lists the libraries; the first build installs them into build\.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE "vcpkg" }
$toolchain = Join-Path $vcpkg "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path $toolchain)) { throw "vcpkg not found. Set VCPKG_ROOT to your vcpkg folder." }
cmake -S $root -B "$root\build" -G "Visual Studio 17 2022" -A x64 "-DCMAKE_TOOLCHAIN_FILE=$toolchain" "-DVCPKG_MANIFEST_MODE=ON"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
cmake --build "$root\build" --config Release
if ($LASTEXITCODE -ne 0) { throw "Build failed" }

# A running exe cannot be overwritten, but it can be renamed out of the way.
foreach ($name in "SpiderPet", "SpiderHost") {
  Remove-Item "$root\$name.old.exe" -Force -ErrorAction SilentlyContinue
  if (Test-Path "$root\$name.exe") {
    try { Remove-Item "$root\$name.exe" -Force -ErrorAction Stop }
    catch { Rename-Item "$root\$name.exe" "$name.old.exe" }
  }
  Copy-Item "$root\build\Release\$name.exe" "$root\$name.exe" -Force
}

& "$root\extension\build.ps1"
Write-Host "Built $root\SpiderPet.exe, SpiderHost.exe and the extension (restart SpiderPet if it was open)"
