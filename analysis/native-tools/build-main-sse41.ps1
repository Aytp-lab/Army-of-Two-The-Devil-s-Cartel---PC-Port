# A15: builds ao4_main_native with AO4_HOST_SSE41=ON in its own build directory
# (analysis/rexglue-main-build-sse41), so the stable analysis/rexglue-main-build and its
# objects stay untouched. Runtime/GPU are copied from the given SDK candidate.
param([ValidatePattern('^sdk-[a-z0-9-]+$')][string]$CandidateName,
      [ValidatePattern('^sdk-[a-z0-9-]+$')][string]$RuntimeCandidate)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if(Get-Process ao4_launcher -ErrorAction SilentlyContinue){throw 'Close game before building.'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$env:PATH=(Split-Path $vswhere)+';'+$env:PATH
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:PATH='C:/Program Files/LLVM/bin;'+(Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin')+';'+(Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja')+';'+$env:PATH
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$candidate=Join-Path $root ('analysis/'+$CandidateName)
$runtime=Join-Path $root ('analysis/'+$RuntimeCandidate)
if(Test-Path -LiteralPath $candidate){throw 'Preserve existing candidate.'}
$build=Join-Path $root 'analysis/rexglue-main-build-sse41'
$log=Join-Path $root ('analysis/main-sse41-build-'+$stamp+'.log')
if(-not(Test-Path -LiteralPath (Join-Path $build 'CMakeCache.txt'))){
 & cmake -S (Join-Path $root 'native/rexglue-main') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
   '-DCMAKE_CXX_COMPILER=C:/Program Files/LLVM/bin/clang-cl.exe' '-DCMAKE_LINKER=C:/Program Files/LLVM/bin/lld-link.exe' `
   "-DCMAKE_PREFIX_PATH=$root\tools\rexglue-sdk-0.10.0\win-amd64" -DREXGLUE_RECOMP_DEBUG_INFO=line-tables-only `
   -DAO4_HOST_SSE41=ON *> $log
 if($LASTEXITCODE -ne 0){Get-Content $log -Tail 30;throw 'Configure failed'}
}
& cmake --build $build --target ao4_main_native --parallel 12 *>> $log
if($LASTEXITCODE -ne 0){Get-Content $log -Tail 30;throw 'Main SSE4.1 build failed'}
New-Item -ItemType Directory -Path $candidate | Out-Null
foreach($ext in 'dll','pdb'){Copy-Item (Join-Path $build "ao4_main_native.$ext") $candidate}
foreach($module in 'rexruntime','rexgpu-xenos'){
 foreach($ext in 'dll','pdb'){Copy-Item (Join-Path $runtime "$module.$ext") $candidate}
}
$strings=[Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $candidate 'ao4_main_native.dll')))
foreach($marker in 'AO4_NET_GATE','AO4_LOCAL_COOP','AO4_QOL settings'){
 if(-not $strings.Contains($marker)){throw "Missing Main feature $marker"}
}
Get-FileHash -LiteralPath @((Join-Path $candidate 'rexgpu-xenos.dll'),(Join-Path $candidate 'rexruntime.dll'),(Join-Path $candidate 'ao4_main_native.dll')) |
 ConvertTo-Json | Set-Content -Encoding UTF8 (Join-Path $candidate 'hashes.json')
Get-Content $log -Tail 3
Write-Output "Candidate: $candidate; log: $log; build dir: $build"
