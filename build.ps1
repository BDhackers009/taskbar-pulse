$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root "src"
$build = Join-Path $root "build"
New-Item -ItemType Directory -Force -Path $build | Out-Null

if (-not (Test-Path (Join-Path $src "icon.ico"))) {
  & (Join-Path $root "tools\make-icon.ps1") | Out-Null
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ toolset was not found." }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

# Import the MSVC environment into this process.
$envLines = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) { throw "vcvars64 failed" }
foreach ($line in $envLines) {
  if ($line -match '^([^=]+)=(.*)$') {
    Set-Item -Path ("env:" + $matches[1]) -Value $matches[2]
  }
}

& rc.exe /nologo /fo "$build\app.res" "$src\app.rc"
if ($LASTEXITCODE -ne 0) { throw "rc failed ($LASTEXITCODE)" }

$exe = Join-Path $root "TaskbarPulse.exe"
& cl.exe /nologo /std:c++20 /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE `
  /Fo"$build\main.obj" /Fe"$exe" "$src\main.cpp" "$build\app.res" `
  /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /INCREMENTAL:NO
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }
"OK: $exe  ({0:N0} bytes)" -f (Get-Item $exe).Length
