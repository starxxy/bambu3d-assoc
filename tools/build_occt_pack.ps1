# build_occt_pack.ps1
# Builds bs3d_occ.dll (OpenCASCADE tessellator plugin) together with every
# OCCT runtime DLL it needs, and packs them into occt-pack.zip -- the exact
# artifact that the "Environment check" panel of bs3d_assoc.exe downloads,
# verifies (SHA-256) and extracts next to itself at runtime.
#
# Requirements (run on a builder machine, NOT inside explorer):
#   - Visual Studio 2022 (C++ workload) or Build Tools
#   - CMake >= 3.21
#   - vcpkg with bootstrap done; set -VcpkgRoot or env VCPKG_ROOT
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\build_occt_pack.ps1
#   powershell -File tools\build_occt_pack.ps1 -VcpkgRoot D:\vcpkg
#
# Output:
#   dist-occt\occt-pack.zip  (+ printed SHA-256 and occpack.url template)

param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Triplet = 'x64-windows',
    [switch]$SkipVcpkgInstall
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$outDir = Join-Path $repo 'dist-occt'
$buildDir = Join-Path $repo 'build-occ'
$stageDir = Join-Path $outDir 'stage'

if (-not $VcpkgRoot) {
    throw "vcpkg root not found. Pass -VcpkgRoot <path> or set VCPKG_ROOT. Get vcpkg from https://github.com/microsoft/vcpkg"
}
$vcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path $vcpkgExe)) {
    throw "vcpkg.exe not found at $vcpkgExe (run bootstrap-vcpkg.bat first)"
}

# 1) Install OCCT (release + debug libs; we only ship the release DLLs).
if (-not $SkipVcpkgInstall) {
    Write-Host "==> vcpkg install occt:$Triplet (this can take a while)"
    & $vcpkgExe install "occt:$Triplet" --x-download-mode=retry
    if ($LASTEXITCODE -ne 0) { throw "vcpkg install failed with exit code $LASTEXITCODE" }
}

# 2) Configure + build only the plugin target.
$toolchain = Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
Write-Host "==> Configure $buildDir"
cmake -S $repo -B $buildDir -G "Visual Studio 17 2022" -A x64 -T host=x64 `
    -DBUILD_OCC_PLUGIN=ON `
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" `
    -DVCPKG_TARGET_TRIPLET=$Triplet
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "==> Build bs3d_occ (Release)"
cmake --build $buildDir --config Release --target bs3d_occ
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$pluginDll = Join-Path $buildDir 'dist\bs3d_occ.dll'
if (-not (Test-Path $pluginDll)) {
    throw "Expected plugin output missing: $pluginDll (was bs3d_occ target skipped?)"
}

# 3) Stage plugin + OCCT runtime DLLs at the ARCHIVE ROOT.
#    The loader (envcheck.cpp) extracts with path sanitization and requires
#    bs3d_occ.dll to be resolvable next to the TK* DLLs it loads.
if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null
Copy-Item $pluginDll $stageDir -Force

$occtBin = Join-Path $VcpkgRoot "installed\$Triplet\bin"
Write-Host "==> Collecting OCCT runtime DLLs from $occtBin"
$patterns = @('TK*.dll', 'tbb*.dll', 'freetype*.dll', 'zlib*.dll', 'cairo*.dll', 'pcre2*.dll', 'graphite*.dll', 'harfbuzz*.dll', 'icudt*.dll', 'icuin*.dll', 'icuuc*.dll')
$copied = 0
foreach ($pat in $patterns) {
    Get-ChildItem (Join-Path $occtBin $pat) -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item $_.FullName $stageDir -Force
        $copied++
    }
}
if ($copied -eq 0) { throw "No OCCT runtime DLLs found under $occtBin" }
Write-Host "    copied $copied runtime DLL(s)"

# 4) Zip (files at root, no wrapper folder) + hash.
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$version = Get-Date -Format 'yyyyMMdd.1'
$zip = Join-Path $outDir "occt-pack-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $zip
$sha = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
$sizeMb = [math]::Round((Get-Item $zip).Length / 1MB, 1)

# 5) Emit the occpack.url template for the release host.
Write-Host ''
Write-Host '================ occt-pack ready ================'
Write-Host "Zip     : $zip ($sizeMb MB)"
Write-Host "SHA256  : $sha"
Write-Host ''
Write-Host 'Upload the zip to your release host, then create occpack.url with:'
Write-Host '  line1 = <public download URL of the zip>'
Write-Host '  line2 = ' + $sha
Write-Host '  line3 = ' + $version
Write-Host 'Ship occpack.url beside bs3d_assoc.exe (programDir) or in'
Write-Host "%ProgramData%\Bambu3DAssoc\ ; occPackSpec() reads both in that order."
Write-Host '================================================='
