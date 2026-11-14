# Builds native/ao4-frontend (first-run setup, launcher, profile manager) with clang-cl/lld/Ninja
# into analysis/ao4-frontend-build. -Target selects the CMake target (default: all).
param([string]$Target='all')
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$env:PATH=(Split-Path $vswhere)+';'+$env:PATH
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:PATH='C:/Program Files/LLVM/bin;'+(Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin')+';'+(Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja')+';'+$env:PATH
$build=Join-Path $root 'analysis/ao4-frontend-build'
$log=Join-Path $root ('analysis/frontend-build-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.log')
if(-not(Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))){
 & cmake -S (Join-Path $root 'native/ao4-frontend') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
   '-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang-cl.exe' '-DCMAKE_LINKER=C:/Program Files/LLVM/bin/lld-link.exe' *> $log
 if($LASTEXITCODE -ne 0){Get-Content $log -Tail 30;throw 'Configure failed'}
}
& cmake --build $build --target $Target --parallel 12 *>> $log
if($LASTEXITCODE -ne 0){Get-Content $log -Tail 40;throw 'Frontend build failed'}
"built $Target in $build"

