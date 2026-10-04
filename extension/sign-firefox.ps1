# Gets the Firefox extension signed by Mozilla, so Firefox keeps it installed.
# It is signed "unlisted": private, not shown in the add-on store.
#
# Once:
#   1. Make a free account at https://addons.mozilla.org
#   2. Create API credentials at https://addons.mozilla.org/developers/addon/api/key/
#   3. Run:  powershell -ExecutionPolicy Bypass -File extension\sign-firefox.ps1
#      It asks for the key and the secret (or reads AMO_JWT_ISSUER / AMO_JWT_SECRET).
# Result: extension\spiderpet-firefox.xpi. Drag it into Firefox and click Add.
param([string]$Key = $env:AMO_JWT_ISSUER, [string]$Secret = $env:AMO_JWT_SECRET)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12  # Mozilla only speaks TLS 1.2+
$here = $PSScriptRoot
$zip = Join-Path $here "spiderpet-firefox-unsigned.zip"
if (-not (Test-Path $zip)) { & (Join-Path $here "build.ps1") }
$manifest = Get-Content (Join-Path $here "manifest.firefox.json") -Raw | ConvertFrom-Json
$guid = $manifest.browser_specific_settings.gecko.id
$version = $manifest.version

if (-not $Key) { $Key = Read-Host "API key (the JWT issuer, like user:12345:678)" }
if (-not $Secret) {
  $s = Read-Host "API secret" -AsSecureString
  $Secret = [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($s))
}

function B64Url([byte[]]$b) { [Convert]::ToBase64String($b).TrimEnd("=").Replace("+", "-").Replace("/", "_") }
function Jwt {
  $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  $head = B64Url ([Text.Encoding]::UTF8.GetBytes('{"alg":"HS256","typ":"JWT"}'))
  $body = B64Url ([Text.Encoding]::UTF8.GetBytes((@{ iss = $Key; jti = [guid]::NewGuid().ToString(); iat = $now; exp = $now + 240 } | ConvertTo-Json -Compress)))
  $hmac = New-Object System.Security.Cryptography.HMACSHA256 (, [Text.Encoding]::UTF8.GetBytes($Secret))
  $sig = B64Url ($hmac.ComputeHash([Text.Encoding]::UTF8.GetBytes("$head.$body")))
  "$head.$body.$sig"
}
function Call($method, $url, $json) {
  $h = @{ Authorization = "JWT $(Jwt)" }
  if ($json) { Invoke-RestMethod -Method $method -Uri $url -Headers $h -ContentType "application/json" -Body $json }
  else { Invoke-RestMethod -Method $method -Uri $url -Headers $h }
}

$api = "https://addons.mozilla.org/api/v5/addons"
Add-Type -AssemblyName System.Net.Http
$client = New-Object System.Net.Http.HttpClient

Write-Host "Uploading SpiderPet $version to Mozilla..."
$form = New-Object System.Net.Http.MultipartFormDataContent
$file = New-Object System.Net.Http.ByteArrayContent (, [IO.File]::ReadAllBytes($zip))
$file.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse("application/zip")
$form.Add($file, "upload", "spiderpet-firefox.zip")
$form.Add((New-Object System.Net.Http.StringContent "unlisted"), "channel")
$req = New-Object System.Net.Http.HttpRequestMessage ([System.Net.Http.HttpMethod]::Post), "$api/upload/"
$req.Headers.Authorization = New-Object System.Net.Http.Headers.AuthenticationHeaderValue "JWT", (Jwt)
$req.Content = $form
$res = $client.SendAsync($req).Result
$text = $res.Content.ReadAsStringAsync().Result
if (-not $res.IsSuccessStatusCode) { throw "Upload failed ($([int]$res.StatusCode)): $text" }
$upload = $text | ConvertFrom-Json

Write-Host "Mozilla is checking it..."
do {
  Start-Sleep 3
  $upload = Call Get "$api/upload/$($upload.uuid)/"
} until ($upload.processed)
if (-not $upload.valid) {
  $upload.validation.messages | Where-Object { $_.type -eq "error" } | ForEach-Object { Write-Host " - $($_.message)" }
  throw "Mozilla's validator rejected the extension."
}

# The first time creates the (unlisted) add-on; later times add a version.
$exists = $true
try { Call Get "$api/addon/$([uri]::EscapeDataString($guid))/" | Out-Null } catch { $exists = $false }
if ($exists) {
  $v = Call Post "$api/addon/$([uri]::EscapeDataString($guid))/versions/" (@{ upload = $upload.uuid } | ConvertTo-Json)
} else {
  $a = Call Put "$api/addon/$([uri]::EscapeDataString($guid))/" (@{ version = @{ upload = $upload.uuid } } | ConvertTo-Json)
  $v = $a.latest_unlisted_version
}

Write-Host "Waiting for the signature..."
for ($i = 0; $i -lt 120; $i++) {
  Start-Sleep 5
  $v = Call Get "$api/addon/$([uri]::EscapeDataString($guid))/versions/$($v.id)/"
  if ($v.file.status -eq "public" -and $v.file.url) { break }
}
if (-not $v.file.url -or $v.file.status -ne "public") { throw "Not signed yet. Run this script again in a few minutes." }
$out = Join-Path $here "spiderpet-firefox.xpi"
Invoke-WebRequest -Uri $v.file.url -Headers @{ Authorization = "JWT $(Jwt)" } -OutFile $out
Write-Host "Signed: $out"
Write-Host "Drag it into a Firefox window and click Add."
