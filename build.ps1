# Build script for WallpaperFeed (MSVC)
# Usage: powershell -ExecutionPolicy Bypass -File .\build.ps1
# Sources are copied to an ASCII temp dir before compiling to avoid
# cl.exe/rc.exe issues with non-ASCII paths.

$ErrorActionPreference = 'Stop'

$projDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir    = Join-Path $projDir 'src'
$binDir    = Join-Path $projDir 'bin'
$buildRoot = Join-Path $env:TEMP 'wallpaperfeed_build'

New-Item -ItemType Directory -Force -Path $binDir | Out-Null
Remove-Item $buildRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

Copy-Item (Join-Path $srcDir '*') $buildRoot -Recurse -Force
Copy-Item (Join-Path $projDir 'tools\icon_gen.cpp') $buildRoot -Force

$vcvarsCandidates = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat'
)
$vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($inst) { $vcvars = Join-Path $inst 'VC\Auxiliary\Build\vcvars64.bat' }
    }
}
if (-not $vcvars -or -not (Test-Path $vcvars)) {
    throw 'vcvars64.bat not found. Install Visual Studio C++ build tools.'
}

$sources = 'main.cpp app.cpp common.cpp config.cpp desk.cpp imaging.cpp net.cpp wf.cpp wic.cpp library.cpp ui.cpp'
$libs = 'user32.lib gdi32.lib gdiplus.lib shell32.lib ole32.lib oleaut32.lib uuid.lib winhttp.lib advapi32.lib comctl32.lib windowscodecs.lib dwmapi.lib'

function Invoke-BuildStep {
    param([string]$Label, [string]$CommandLine)
    Write-Output "==> $Label"
    $full = "call `"$vcvars`" >nul && cd /d `"$buildRoot`" && $CommandLine"
    & cmd.exe /c $full
    if ($LASTEXITCODE -ne 0) {
        throw "Step failed: $Label (exit code $LASTEXITCODE)"
    }
}

Invoke-BuildStep 'build icon generator' 'cl /nologo /utf-8 /O2 /MT /EHsc /DUNICODE /D_UNICODE icon_gen.cpp /link /SUBSYSTEM:CONSOLE /OUT:icon_gen.exe gdiplus.lib ole32.lib user32.lib'
Invoke-BuildStep 'generate icon'        'icon_gen.exe "res\app.ico"'
Invoke-BuildStep 'compile resources'    'rc /nologo /fo app.res app.rc'
Invoke-BuildStep 'compile main app'     "cl /nologo /utf-8 /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE $sources app.res /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /OUT:WallpaperFeed.exe $libs"

$exe = Join-Path $buildRoot 'WallpaperFeed.exe'
$pdb = Join-Path $buildRoot 'WallpaperFeed.pdb'
if (-not (Test-Path $exe)) { throw 'WallpaperFeed.exe was not produced.' }

Copy-Item $exe (Join-Path $binDir 'WallpaperFeed.exe') -Force
Copy-Item (Join-Path $buildRoot 'res\app.ico') (Join-Path $binDir 'app.ico') -Force
if (Test-Path $pdb) { Copy-Item $pdb (Join-Path $binDir 'WallpaperFeed.pdb') -Force }

$size = (Get-Item (Join-Path $binDir 'WallpaperFeed.exe')).Length
Write-Output ("Build OK: {0} ({1:N0} KB)" -f (Join-Path $binDir 'WallpaperFeed.exe'), ($size / 1KB))
