param(
    [Parameter(Mandatory=$true)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [string]$BuildDirectory = '',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $repoRoot 'build' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repoRoot 'build\releases' }
$buildRoot = (Resolve-Path $BuildDirectory).Path
$releaseRoot = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) "v$Version"
$packageName = "DLSSKytyPS5-v$Version-Windows-x64"
$packageRoot = Join-Path $releaseRoot $packageName
$zipPath = Join-Path $releaseRoot "$packageName.zip"
if ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $zipPath)) {
    throw 'Staging directory or ZIP already exists; choose a fresh output directory.'
}
New-Item -ItemType Directory -Path $packageRoot -Force | Out-Null

# Install only the two application directories, not dependency development files.
foreach ($script in @((Join-Path $buildRoot 'cmake_install.cmake'),
                      (Join-Path $buildRoot 'launcher\cmake_install.cmake'))) {
    & cmake "-DCMAKE_INSTALL_PREFIX=$packageRoot" '-DCMAKE_INSTALL_CONFIG_NAME=Release' `
        '-DCMAKE_INSTALL_LOCAL_ONLY=1' -P $script
    if ($LASTEXITCODE -ne 0) { throw "CMake runtime install failed: $script" }
}
foreach ($file in @('LICENSE','README.md','README_DLSS.md','README_FSR.md',
                    'README_DIRECTSTORAGE.md','README_DLSS_RR.md','ASTRO_BOT_RESOLUTIONS.md',
                    "RELEASE_NOTES_$Version.md")) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $file) -Destination $packageRoot
}
Copy-Item -LiteralPath (Join-Path $repoRoot '_Patches') -Destination $packageRoot -Recurse
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSES') -Destination $packageRoot -Recurse -Force
$sdkRoot = Join-Path $repoRoot '..\streamline-sdk-v2.14.1'
Copy-Item -LiteralPath (Join-Path $sdkRoot 'license.txt') -Destination (Join-Path $packageRoot 'LICENSES\Streamline.txt')
Copy-Item -LiteralPath (Join-Path $sdkRoot '3rd-party-licenses.md') -Destination (Join-Path $packageRoot 'LICENSES\Streamline-third-party.md')
$storageSdk = Join-Path $repoRoot '..\DirectStorage_SDK'
Copy-Item -LiteralPath (Join-Path $storageSdk 'LICENSE.txt') -Destination (Join-Path $packageRoot 'LICENSES\DirectStorage.txt')
Copy-Item -LiteralPath (Join-Path $storageSdk 'NOTICES.txt') -Destination (Join-Path $packageRoot 'LICENSES\DirectStorage-NOTICES.txt')

$required = @('kyty_emulator.exe','kyty_launcher.exe','kyty_updater.exe','u59-preset.json',
    'Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','platforms\qwindows.dll',
    'sl.interposer.dll','sl.common.dll','sl.dlss.dll','nvngx_dlss.dll','sl.dlss_g.dll',
    'nvngx_dlssg.dll','sl.reflex.dll','NvLowLatencyVk.dll','sl.pcl.dll','sl.nis.dll',
    'sl.dlss_d.dll','nvngx_dlssd.dll','dstorage.dll','dstoragecore.dll','libwinpthread-1.dll',
    'ffx_backend_vk_x64.dll','ffx_fsr3_x64.dll','ffx_fsr3upscaler_x64.dll',
    'ffx_opticalflow_x64.dll','ffx_frameinterpolation_x64.dll','_Patches\PPSA01325.json',
    '_Patches\PPSA21567.json','LICENSES\Streamline.txt','LICENSES\FidelityFX-SDK.txt')
foreach ($file in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $packageRoot $file) -PathType Leaf)) {
        throw "Missing packaged runtime file: $file"
    }
}
if (-not (Get-ChildItem (Join-Path $packageRoot 'tls') -Filter '*.dll' -ErrorAction SilentlyContinue)) {
    throw 'Missing Qt TLS backend'
}
$help = & (Join-Path $packageRoot 'kyty_emulator.exe') --help
if ($LASTEXITCODE -ne 0 -or -not ($help -match "ver = $([regex]::Escape($Version))")) {
    throw 'Packaged emulator did not report the expected version'
}
$commit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot obtain source commit' }
$files = @()
foreach ($file in Get-ChildItem -LiteralPath $packageRoot -Recurse -File | Sort-Object FullName) {
    $relative = $file.FullName.Substring($packageRoot.Length+1).Replace('\','/')
    if ($relative -match '(^|/)(_SaveData|_PipelineCache|_DownloadData|_TempData|_HangTrace)(/|$)|\.(pdb|map|ini|cmake)$') {
        throw "Unwanted local/development file in package: $relative"
    }
    $files += [ordered]@{ path=$relative; bytes=$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
[ordered]@{ version=$Version; commit=$commit; origin='Fork'; files=$files } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $packageRoot 'MANIFEST.json') -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($packageRoot,$zipPath,[IO.Compression.CompressionLevel]::Optimal,$false)
$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $packageName.zip" | Set-Content -LiteralPath "$zipPath.sha256" -Encoding ASCII
Write-Output "Package: $zipPath"
Write-Output "SHA256: $hash"
