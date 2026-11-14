param(
    [ValidateSet('launcher','main')][string]$Project = 'launcher',
    [ValidateRange(1,8)][int]$ParallelJobs = 4
)
$ErrorActionPreference = 'Stop'
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools missing.' }
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$llvmDirectory = 'C:\Program Files\LLVM\bin'
$cmakeDirectory = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDirectory = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = "$llvmDirectory;$cmakeDirectory;$ninjaDirectory;$env:PATH"
$sdk = Join-Path $workspace 'tools\rexglue-sdk-0.10.0\win-amd64'
$source = Join-Path $workspace "native\rexglue-$Project"
$build = Join-Path $workspace "analysis\rexglue-$Project-build"
$ErrorActionPreference = 'Continue'
& clang-cl --version
if ($LASTEXITCODE -ne 0) { throw 'Clang check failed.' }
& cmake -S $source -B $build -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_CXX_COMPILER=$llvmDirectory/clang-cl.exe" "-DCMAKE_PREFIX_PATH=$sdk"
if ($LASTEXITCODE -ne 0) { throw 'ReXGlue launcher configuration failed.' }
& cmake --build $build --parallel $ParallelJobs
if ($LASTEXITCODE -ne 0) { throw 'ReXGlue launcher build failed.' }
if ($Project -eq 'launcher') {
    & cmake --build $build --target ao4_roundeven_check --parallel $ParallelJobs
    if ($LASTEXITCODE -ne 0) { throw 'Rounding check build failed.' }
    & (Join-Path $build 'ao4_roundeven_check.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Rounding check failed.' }
    & cmake --build $build --target ao4_input_check --parallel $ParallelJobs
    if ($LASTEXITCODE -ne 0) { throw 'Input check build failed.' }
    & (Join-Path $build 'ao4_input_check.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Input check failed.' }
}
