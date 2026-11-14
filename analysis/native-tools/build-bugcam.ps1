# Builds native/diagnostics/bug_recorder.cpp -> analysis/ao4-frontend-build/ao4_bugcam.exe (static CRT).
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:PATH='C:/Program Files/LLVM/bin;'+$env:PATH
$out=Join-Path $root 'analysis/ao4-frontend-build'
$obj=Join-Path $out 'ao4_bugcam.obj'
& clang-cl /nologo /std:c++20 /EHsc /O2 /MT (Join-Path $root 'native/diagnostics/bug_recorder.cpp') "/Fe:$out/ao4_bugcam.exe" "/Fo:$obj" /link d3d11.lib dxgi.lib windowsapp.lib windowscodecs.lib ole32.lib user32.lib
if($LASTEXITCODE){throw 'bugcam build failed'}
"built $out/ao4_bugcam.exe"
