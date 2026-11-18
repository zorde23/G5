param([string]$GameDir = '', [string]$Output = '', [string]$Eboot = '')
& (Join-Path $PSScriptRoot 'tools\build_hybrid.ps1') -Prebuilt $PSScriptRoot -Profile g5 -Eboot $Eboot -GameDir $GameDir -Output $Output
exit $LASTEXITCODE
