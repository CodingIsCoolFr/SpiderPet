# Builds the browser extension from extension\src:
#   extension\chromium\          load unpacked in Brave, Chrome, Edge
#   extension\firefox\           load as a temporary add-on in Firefox
#   extension\spiderpet-firefox-unsigned.zip   what sign-firefox.ps1 sends to Mozilla
#   extension\spiderpet-chromium.zip           for the release page
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
# Two top-level functions with one name: JavaScript quietly keeps the second,
# and every caller of the first breaks (it once stopped the spider reading pages).
foreach ($js in Get-ChildItem (Join-Path $here "src\*.js")) {
  $dupes = Select-String -Path $js.FullName -Pattern '^  function (\w+)\(' | ForEach-Object { $_.Matches[0].Groups[1].Value } |
    Group-Object | Where-Object Count -gt 1 | ForEach-Object Name
  if ($dupes) { throw "$($js.Name): more than one function named $($dupes -join ', ')" }
}
foreach ($target in "chromium", "firefox") {
  $out = Join-Path $here $target
  if (Test-Path $out) { Remove-Item $out -Recurse -Force }
  New-Item -ItemType Directory $out | Out-Null
  Copy-Item (Join-Path $here "src\*") $out -Recurse
  Copy-Item (Join-Path $here "manifest.$target.json") (Join-Path $out "manifest.json")
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($pair in @(@("firefox", "spiderpet-firefox-unsigned.zip"), @("chromium", "spiderpet-chromium.zip"))) {
  $zip = Join-Path $here $pair[1]
  if (Test-Path $zip) { Remove-Item $zip -Force }
  # Forward slashes inside the zip: Mozilla's validator rejects backslash paths.
  $src = Join-Path $here $pair[0]
  $archive = [System.IO.Compression.ZipFile]::Open($zip, "Create")
  try {
    Get-ChildItem $src -Recurse -File | ForEach-Object {
      $rel = $_.FullName.Substring($src.Length + 1).Replace("\", "/")
      [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $_.FullName, $rel)
    }
  } finally { $archive.Dispose() }
}
Write-Host "Built the extension in $here"
