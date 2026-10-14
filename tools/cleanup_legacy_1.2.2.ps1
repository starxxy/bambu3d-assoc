# Bambu3DAssoc -- one-time elevated cleanup of 1.2.2 leftovers.
#
# Background
# ----------
# The 1.2.2 uninstaller ran with administrative rights (it was a machine-wide
# install) and removed the association and the thumbnail-handler values, but
# it left two kinds of trace behind that only an elevated process can reach:
#
#   1. 20 empty HKLM shell-extension keys, one pair per extension:
#        HKLM\SOFTWARE\Classes\.<ext>\ShellEx
#        HKLM\SOFTWARE\Classes\SystemFileAssociations\.<ext>\ShellEx
#      They hold no subkeys and no values -- pure scaffolding.
#
#   2. C:\Program Files\Bambu3DAssoc -- bs3d_assoc.exe was still mapped when
#      the uninstaller tried to delete it, so the folder survived.
#
# The 1.2.4 uninstaller removes the same keys itself (RegDeleteKeyIfEmpty) on
# any machine-wide install, because Inno marks the uninstaller as requiring
# administrator rights whenever admin rights were available at install time.
# A per-user uninstall cannot touch HKLM, which is why this script exists as a
# one-off for machines that once had the 1.2.2 machine-wide install.
#
# Usage: right-click -> Run with PowerShell, or from an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File cleanup_legacy_1.2.2.ps1
#
# Safety: a key is removed only when it is genuinely empty. If another product
# has put a handler in one of these ShellEx keys it is reported and kept.

$ErrorActionPreference = 'SilentlyContinue'

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host ''
    Write-Host '[!] This script must be run as administrator.' -ForegroundColor Red
    Write-Host '    Right-click it and pick "Run with PowerShell" from an'
    Write-Host '    elevated console, or start PowerShell as administrator.'
    Write-Host ''
    Read-Host 'Press Enter to exit'
    exit 1
}

$exts = @('3mf','stl','obj','step','stp','fbx','amf','gcode','g','gco')
$removed = 0
$kept = 0

Write-Host ''
Write-Host 'Step 1/2 -- empty HKLM shell-extension keys'
foreach ($e in $exts) {
    $paths = @(
        "HKLM:\SOFTWARE\Classes\.$e\ShellEx",
        "HKLM:\SOFTWARE\Classes\SystemFileAssociations\.$e\ShellEx"
    )
    foreach ($p in $paths) {
        if (Test-Path $p) {
            $k = Get-Item $p
            if ($k.SubKeyCount -eq 0 -and $k.ValueCount -eq 0) {
                Remove-Item -Path $p -Force -Confirm:$false
                Write-Host ("  removed  " + $p)
                $removed++
            } else {
                Write-Host ("  KEPT     " + $p + "   (not empty -- belongs to something else)")
                $kept++
            }
        }
    }
    # the SystemFileAssociations parent may now be empty too
    $par = "HKLM:\SOFTWARE\Classes\SystemFileAssociations\.$e"
    if (Test-Path $par) {
        $k = Get-Item $par
        if ($k.SubKeyCount -eq 0 -and $k.ValueCount -eq 0) {
            Remove-Item -Path $par -Force -Confirm:$false
            Write-Host ("  removed  " + $par)
            $removed++
        }
    }
}
Write-Host ("  -> $removed removed, $kept kept")

Write-Host ''
Write-Host 'Step 2/2 -- leftover install folder'
$dir = 'C:\Program Files\Bambu3DAssoc'
if (Test-Path $dir) {
    Remove-Item -Path $dir -Recurse -Force -Confirm:$false
    if (Test-Path $dir) {
        Write-Host ("  [!] could not remove " + $dir) -ForegroundColor Yellow
        Write-Host '      Close any program using it and run this script again.'
    } else {
        Write-Host ("  removed  " + $dir)
    }
} else {
    Write-Host '  (nothing to do)'
}

Write-Host ''
Write-Host 'Done.'
Read-Host 'Press Enter to exit'
