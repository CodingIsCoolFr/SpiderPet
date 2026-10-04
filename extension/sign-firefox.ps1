# Puts SpiderPet on addons.mozilla.org, the official Firefox add-on store.
# Mozilla reviews it; once approved, anyone can install it from there and
# Firefox keeps it up to date. Its store page, privacy policy and screenshots
# are filled in from here.
#
# Once:
#   1. Make a free account at https://addons.mozilla.org
#   2. Create API credentials at https://addons.mozilla.org/developers/addon/api/key/
#   3. Run:  powershell -ExecutionPolicy Bypass -File extension\sign-firefox.ps1
#      It asks for the key and the secret (or reads AMO_JWT_ISSUER / AMO_JWT_SECRET).
# For a new version: raise "version" in both manifests, build, run it again.
#
#   -Private   sign it without listing it in the store (only you can install
#              it): makes extension\spiderpet-firefox.xpi to drag into Firefox.
param([string]$Key = $env:AMO_JWT_ISSUER, [string]$Secret = $env:AMO_JWT_SECRET, [switch]$Private)
$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12  # Mozilla only speaks TLS 1.2+
$here = $PSScriptRoot
$root = Split-Path $here
$zip = Join-Path $here "spiderpet-firefox-unsigned.zip"
if (-not (Test-Path $zip)) { & (Join-Path $here "build.ps1") }
$manifest = Get-Content (Join-Path $here "manifest.firefox.json") -Raw | ConvertFrom-Json
$guid = $manifest.browser_specific_settings.gecko.id
$version = $manifest.version
$channel = if ($Private) { "unlisted" } else { "listed" }
$repo = "https://github.com/CodingIsCoolFr/SpiderPet"

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
function Call($method, $url, $data) {
  $h = @{ Authorization = "JWT $(Jwt)" }
  if ($null -ne $data) {
    $json = $data | ConvertTo-Json -Depth 8 -Compress
    Invoke-RestMethod -Method $method -Uri $url -Headers $h -ContentType "application/json; charset=utf-8" -Body ([Text.Encoding]::UTF8.GetBytes($json))
  } else { Invoke-RestMethod -Method $method -Uri $url -Headers $h }
}

$api = "https://addons.mozilla.org/api/v5/addons"
$g = [uri]::EscapeDataString($guid)
Add-Type -AssemblyName System.Net.Http
$client = New-Object System.Net.Http.HttpClient
function PostFile($url, $field, $path, $type, $extra) {
  $form = New-Object System.Net.Http.MultipartFormDataContent
  $file = New-Object System.Net.Http.ByteArrayContent (, [IO.File]::ReadAllBytes($path))
  $file.Headers.ContentType = [System.Net.Http.Headers.MediaTypeHeaderValue]::Parse($type)
  $form.Add($file, $field, [IO.Path]::GetFileName($path))
  foreach ($k in $extra.Keys) { $form.Add((New-Object System.Net.Http.StringContent $extra[$k]), $k) }
  $req = New-Object System.Net.Http.HttpRequestMessage ([System.Net.Http.HttpMethod]::Post), $url
  $req.Headers.Authorization = New-Object System.Net.Http.Headers.AuthenticationHeaderValue "JWT", (Jwt)
  $req.Content = $form
  $res = $client.SendAsync($req).Result
  $text = $res.Content.ReadAsStringAsync().Result
  if (-not $res.IsSuccessStatusCode) { throw "Upload to $url failed ($([int]$res.StatusCode)): $text" }
  $text | ConvertFrom-Json
}

Write-Host "Uploading SpiderPet $version to Mozilla ($channel)..."
$upload = PostFile "$api/upload/" "upload" $zip "application/zip" @{ channel = $channel }
Write-Host "Mozilla is checking it..."
do {
  Start-Sleep 3
  $upload = Call Get "$api/upload/$($upload.uuid)/"
} until ($upload.processed)
if (-not $upload.valid) {
  $upload.validation.messages | Where-Object { $_.type -eq "error" } | ForEach-Object { Write-Host " - $($_.message)" }
  throw "Mozilla's validator rejected the extension."
}

# What the store page says. Mozilla asks add-ons that need other software to say so up front.
$summary = "A spider walks the page, harvests DOIs, ISBNs, paper ids, titles and key facts, and checks each one against " +
  "Crossref, PubMed, arXiv and Wikipedia with your local AI. Needs the free SpiderPet app for Windows."
$description = @"
SpiderPet drops a little node-and-line spider onto the page you are reading. It walks across the text, lassoes what matters and restyles it right in the page: DOIs turn into green code, ISBNs into big blue code, paper ids into tags, titles into salmon serif.

Every find gets a check from a real source, shown as a badge next to it: verified, wrong, not found, unclear, opinion or ad. Hover a badge for the reason; click it to open the source.

Type what you are looking for in the SpiderPet app and the spider hunts for it on the page you have open: every match lights up, and the spider goes from match to match.

<b>Needs the SpiderPet app (Windows, free and open source).</b> The add-on is the spider; the app is its brain. It does the checks with a local AI model (Ollama) on your own PC and keeps a library of everything the spider found. Download it from $repo/releases/latest and run it once.

Privacy: the page never leaves your PC. With Online checks on (a switch in the app), only each single find (an id, a title or a few search words) is looked up in Crossref, OpenLibrary, Google Books, PubMed, arXiv or Wikipedia.

Source code: $repo
"@
$privacy = @"
SpiderPet reads the text of the pages you start it on, inside your browser, and passes what it finds to the SpiderPet app on your own computer (through Firefox's native messaging). Nothing is sent to the developer, and there are no analytics.

When "Online checks" is on in the SpiderPet app, the app looks up each single find (a DOI, ISBN, PubMed/PMC/arXiv id, a citation title or a few search words from a sentence) in these public services: Crossref, OpenLibrary, Google Books, NCBI PubMed, arXiv and Wikipedia. The rest of the page is never sent. Turn Online checks off and nothing leaves your computer.

The app keeps its library of finds in Documents\SpiderPet and its settings in %APPDATA%\SpiderPet on your computer. Delete those folders to remove everything.
"@
$listing = @{
  categories  = @("search-tools")
  summary     = @{ "en-US" = $summary }
  description = @{ "en-US" = $description }
  homepage    = @{ "en-US" = $repo }
  support_url = @{ "en-US" = "$repo/issues" }
}

# The first time creates the add-on; later times add a version.
$exists = $true
try { Call Get "$api/addon/$g/" | Out-Null } catch { $exists = $false }
if ($exists) {
  if (-not $Private) { Call Patch "$api/addon/$g/" $listing | Out-Null }
  Call Post "$api/addon/$g/versions/" @{ upload = $upload.uuid; license = "MIT" } | Out-Null
} else {
  $body = if ($Private) { @{} } else { $listing.Clone() }
  $body.version = @{ upload = $upload.uuid; license = "MIT" }
  Call Put "$api/addon/$g/" $body | Out-Null
}
$addon = Call Get "$api/addon/$g/"

if (-not $Private) {
  try { Call Patch "$api/addon/$g/eula_policy/" @{ privacy_policy = @{ "en-US" = $privacy } } | Out-Null }
  catch { Write-Host "Could not set the privacy policy ($($_.Exception.Message)). Add it on the add-on's edit page." }
  if (-not $addon.previews -or $addon.previews.Count -eq 0) {
    foreach ($shot in "page.png", "app.png") {
      $path = Join-Path $root "docs\media\$shot"
      if (Test-Path $path) {
        try { PostFile "$api/addon/$g/previews/" "image" $path "image/png" @{} | Out-Null }
        catch { Write-Host "Could not add the screenshot $shot ($($_.Exception.Message))." }
      }
    }
  }
}

$filter = if ($Private) { "all_with_unlisted" } else { "all_without_unlisted" }
$all = (Call Get "$api/addon/$g/versions/?filter=$filter").results
$v = $all | Where-Object { $_.version -eq $version } | Select-Object -First 1
if (-not $v) { $v = $all | Select-Object -First 1 }
Write-Host "Waiting for Mozilla to sign it..."
for ($i = 0; $i -lt 60; $i++) {
  if ($v.file.status -eq "public" -and $v.file.url) { break }
  Start-Sleep 5
  $v = Call Get "$api/addon/$g/versions/$($v.id)/"
}
if ($v.file.status -eq "public" -and $v.file.url) {
  $out = Join-Path $here "spiderpet-firefox.xpi"
  Invoke-WebRequest -Uri $v.file.url -Headers @{ Authorization = "JWT $(Jwt)" } -OutFile $out
  Write-Host "Signed: $out"
}
if ($Private) {
  if (-not $v.file.url -or $v.file.status -ne "public") { throw "Not signed yet. Run this script again in a few minutes." }
  Write-Host "Drag it into a Firefox window and click Add."
} elseif ($v.file.status -eq "public") {
  Write-Host "SpiderPet $version is live in the Firefox add-on store: $($addon.url)"
} else {
  Write-Host "Sent. Mozilla is reviewing SpiderPet $version; you get an e-mail when it is live."
  Write-Host "Its store page: $($addon.url)"
}
