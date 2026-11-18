param(
    [string]$Eboot = '',
    [string]$Output = '',
    [string]$Name = 'eboot',
    [Alias('Profile')] [string]$ProfileName = 'base',
    [string]$Patches = '',
    [string]$GameDir = '',
    [int]$Jobs = [Environment]::ProcessorCount,
    [string]$Build = '',
    [string]$KytyBuild = '',
    [string]$Prebuilt = ''
)
$root = Split-Path -Parent $PSScriptRoot
if (-not $Build) { $Build = Join-Path $root 'build-hybrid' }
if (-not $KytyBuild) { $KytyBuild = Join-Path $root 'build-kyty-gpu' }
if ($Prebuilt -and $GameDir -and -not $Eboot) { $Eboot = "$GameDir\eboot.bin" }
if ($Eboot -and -not $Output -and -not $Prebuilt) { throw '-Eboot needs -Output, the folder to lay the converted title out in' }

function Run([string]$what, [scriptblock]$command) {
    Write-Host "== $what"
    $ErrorActionPreference = 'Continue'
    & $command
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

function Real([string]$tool) {
    Get-Command $tool -All -ErrorAction SilentlyContinue | Where-Object { $_.Source -notmatch '\\(WindowsApps|System32)\\' } | Select-Object -First 1
}
$missing = @()
if ($Prebuilt) {
    foreach ($file in 'relinker.exe', 'libs\libc.prx', 'kyty_gpu.dll', 'libwinpthread-1.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $Prebuilt $file))) { $missing += "$file in the prebuilt folder $Prebuilt" }
    }
    if (-not $Eboot) { $missing += "the title folder: -GameDir <the folder holding the title's eboot.bin, sce_module and sce_sys>" }
    if (-not $Output) { $missing += 'the folder to lay the converted title out in: -Output <folder>' }
} else {
$git = Real 'git'
if ($git) {
    $gitBin = Join-Path (Split-Path -Parent (Split-Path -Parent $git.Source)) 'usr\bin'
    if (Test-Path (Join-Path $gitBin 'bash.exe')) { $env:PATH = "$gitBin;$env:PATH" }
}
foreach ($tool in 'git', 'cmake', 'ninja', 'gcc', 'g++', 'objdump', 'bash') {
    if (-not (Real $tool)) { $missing += "$tool (put the WinLibs mingw64\bin, CMake and Git for Windows cmd and usr\bin on PATH; see docs/dev/BUILD.md)" }
}
$python = ''
foreach ($candidate in 'python3', 'python', 'py') {
    $command = Real $candidate
    if (-not $command) { continue }
    $found = "$(& $command.Source -c 'import sys; print(sys.version_info[0], sys.executable)' 2>$null)"
    if ($LASTEXITCODE -eq 0 -and $found -match '^3 (.+)$') { $python = $Matches[1].Trim(); break }
}
if (-not $python) { $missing += 'Python 3 (python.org or the Python Install Manager; the Microsoft Store alias does not run it)' }
if ((Real 'gcc') -and ((& gcc -dumpmachine) -notmatch 'mingw')) { $missing += "a MinGW gcc first on PATH (found $((Get-Command gcc).Source))" }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath } else { '' }
$vsDevCmd = if ($vs) { Join-Path $vs 'Common7\Tools\VsDevCmd.bat' } else { '' }
if (-not $vsDevCmd -or -not (Test-Path $vsDevCmd)) { $missing += 'Visual Studio or Build Tools with "C++ Clang tools for Windows" (clang-cl) and a Windows SDK' }
$vsCmake = if ($vs) { Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin' } else { '' }
if ($vsCmake -and (Test-Path (Join-Path $vsCmake 'cmake.exe'))) { $env:PATH = "$vsCmake;$env:PATH" }
$glslang = if ($env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK 'Bin\glslangValidator.exe' } else { '' }
if (-not $glslang -or -not (Test-Path $glslang)) { $missing += 'the Vulkan SDK (VULKAN_SDK must point at it; Kyty compiles its shaders with glslangValidator)' }
}
if ($Eboot -and -not (Test-Path -LiteralPath $Eboot)) { $missing += "the executable given to -Eboot: $Eboot" }
if ($Patches -and -not (Test-Path -LiteralPath $Patches)) { $missing += "the patch file $Patches" }
$profileFile = if (Test-Path -LiteralPath $ProfileName -PathType Leaf) { $ProfileName } else { Join-Path $PSScriptRoot "hybrid-profiles\$ProfileName.ini" }
if (-not (Test-Path -LiteralPath $profileFile -PathType Leaf)) { $missing += "the profile given to -Profile: $ProfileName" }
if ($GameDir -and -not (Test-Path -LiteralPath $GameDir -PathType Container)) { $missing += "the title folder given to -GameDir: $GameDir" }
if ($missing) {
    Write-Host 'Missing prerequisites:'
    $missing | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
$ErrorActionPreference = 'Stop'

if ($Prebuilt) {
    $libs = Join-Path $Prebuilt 'libs'
    $relinkerExe = Join-Path $Prebuilt 'relinker.exe'
    $kytyFiles = (Join-Path $Prebuilt 'kyty_gpu.dll'), (Join-Path $Prebuilt 'libwinpthread-1.dll')
} else {
Run 'submodules' { git -C $root submodule update --init --recursive }
$pathMap = "-ffile-prefix-map=$root=. -ffile-prefix-map=$Build=build"

Run 'configure the runtime' { cmake -S $root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DAPS5_HOST_NO_RED_ZONE=ON "-DPython3_EXECUTABLE=$python" "-DCMAKE_C_FLAGS=$pathMap" "-DCMAKE_CXX_FLAGS=$pathMap" }
Run 'build the relinker and the runtime libraries' { cmake --build $Build --target relinker libs --parallel $Jobs }
$libs = Join-Path $Build 'core\libs\libs'
$toolchain = Split-Path -Parent (Get-Command gcc).Source
foreach ($runtime in 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll') { Copy-Item (Join-Path $toolchain $runtime) $libs -Force }

$kyty = Join-Path $root '3rdparty\kyty-gpu'
$kytyPathMap = "/clang:-ffile-prefix-map=$kyty=. /clang:-ffile-prefix-map=$KytyBuild=build"
$env:PATH = "$(Split-Path -Parent $glslang);$env:PATH"
Run 'configure kyty_gpu.dll' { cmd /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && cmake -S `"$kyty`" -B `"$KytyBuild`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DKYTY_BUILD_LAUNCHER=OFF -DKYTY_NATIVE_V3=ON -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl `"-DCMAKE_C_FLAGS_INIT=$kytyPathMap`" `"-DCMAKE_CXX_FLAGS_INIT=$kytyPathMap`"" }
Run 'build kyty_gpu.dll' { cmd /c "call `"$vsDevCmd`" -arch=x64 -host_arch=x64 >nul && cmake --build `"$KytyBuild`" --target kyty_gpu --parallel $Jobs" }
$relinkerExe = Join-Path $Build 'core\relinker\relinker.exe'
$kytyFiles = (Join-Path $KytyBuild 'kyty_gpu.dll'), (Join-Path $kyty '3rdparty\winpthread\bin\libwinpthread-1.dll')
}

if (-not $Eboot) {
    Write-Host "Built: $Build\core\relinker\relinker.exe, $libs and $KytyBuild\kyty_gpu.dll"
    exit 0
}

$outLibs = Join-Path $Output 'libs'
New-Item -ItemType Directory -Force $outLibs | Out-Null
Remove-Item (Join-Path $outLibs '*.redzone.txt') -ErrorAction SilentlyContinue
Get-ChildItem -File $libs | Copy-Item -Destination $outLibs -Force
Copy-Item $kytyFiles $Output -Force
$exe = Join-Path $Output "$Name.exe"
$app0 = Join-Path $Output 'app0'
$gameItems = @()
if ($GameDir) {
    $gameItems = @(Get-ChildItem -LiteralPath $GameDir -Force | Where-Object { $_.Name -ne 'sce_module' })
    foreach ($item in $gameItems) {
        $link = Join-Path $app0 $item.Name
        if ((Test-Path -LiteralPath $link) -and -not (@((Get-Item -LiteralPath $link -Force).Target) -contains $item.FullName)) {
            throw "$link already exists and is not a link to $($item.FullName); move it away and run the script again"
        }
        if (-not $item.PSIsContainer -and [IO.Path]::GetPathRoot($item.FullName) -ne [IO.Path]::GetPathRoot($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output))) {
            throw "$($item.FullName) is on another volume than $Output; a hard link needs both on the same one"
        }
    }
}
$profileLines = @{ relinker = @(); hybrid = @() }
$profileSection = ''
foreach ($line in Get-Content -LiteralPath $profileFile) {
    $line = $line.Trim()
    if ($line -match '^\[(.+)\]$') { $profileSection = $Matches[1]; continue }
    if ($line -and -not $line.StartsWith(';') -and $profileLines.ContainsKey($profileSection)) { $profileLines[$profileSection] += $line }
}
$flags = @($profileLines.relinker)
if (-not $flags) { throw "the profile $profileFile has no [relinker] flags" }
if ($Patches) { $flags += '--patches', $Patches }
Run 'convert the executable' { & $relinkerExe @flags $Eboot $exe }
if ($gameItems) { New-Item -ItemType Directory -Force $app0 | Out-Null }
foreach ($item in $gameItems) {
    $link = Join-Path $app0 $item.Name
    if (Test-Path -LiteralPath $link) { continue }
    if ($item.PSIsContainer) { Run "link app0\$($item.Name)" { cmd /c "mklink /J `"$link`" `"$($item.FullName)`"" } }
    else { Run "link app0\$($item.Name)" { cmd /c "mklink /H `"$link`" `"$($item.FullName)`"" } }
}
if (-not $Prebuilt) { Run 'red-zone tables of the host libraries' { & $python (Join-Path $root 'tools\host_red_zone_table.py') --libs $outLibs --objdump (Get-Command objdump).Source } }
if (Test-Path (Join-Path $outLibs 'libc.prx.redzone.txt')) { throw 'libc.prx got a red-zone table: a Kyty write fault inside its memcpy would end the title' }
$ini = Join-Path $Output 'ps5-hybrid.ini'
if (-not (Test-Path $ini)) { Set-Content -Encoding ascii $ini (@('[hybrid]') + $profileLines.hybrid) }
if ($GameDir) { Write-Host "Ready: $exe" } else { Write-Host "Ready: $exe. Put the title's files in $app0, or run again with -GameDir (see docs/user/USAGE.md)." }
