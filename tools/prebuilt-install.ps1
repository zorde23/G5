param([string]$GameDir = '', [string]$Output = '', [string]$Eboot = '', [string]$Patches = '')
& (Join-Path $PSScriptRoot 'tools\build_hybrid.ps1') -Prebuilt $PSScriptRoot -Profile g5 -Eboot $Eboot -GameDir $GameDir -Output $Output -Patches $Patches
exit $LASTEXITCODE
