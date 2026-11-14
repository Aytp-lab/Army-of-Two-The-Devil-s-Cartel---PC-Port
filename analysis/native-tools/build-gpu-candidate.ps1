# Builds only rexgpu-xenos from the SDK source tree into a new candidate; runtime and main
# are copied from the given base candidate. The SDK out/ DLLs are backed up first and
# restored byte-identically afterwards (unlike build-sdk-candidate.ps1, which restores an
# old 2026-10-06 backup).
param([ValidatePattern('^sdk-[a-z0-9-]+$')][string]$CandidateName,
      [ValidatePattern('^sdk-[a-z0-9-]+$')][string]$BaseCandidate)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if(Get-Process ao4_launcher -ErrorAction SilentlyContinue){throw 'Close the game before building.'}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$env:PATH=(Split-Path $vswhere)+';'+$env:PATH
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$env:PATH='C:/Program Files/LLVM/bin;'+(Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin')+';'+(Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja')+';'+$env:PATH
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$candidate=Join-Path $root ('analysis/'+$CandidateName)
$base=Join-Path $root ('analysis/'+$BaseCandidate)
if(Test-Path -LiteralPath $candidate){throw 'Preserve existing candidate.'}
$out=Join-Path $root 'tools/rexglue-sdk-source-0.10.0/out/win-amd64'
$backup=Join-Path $root ('analysis/sdk-out-before-gpu-'+$stamp)
$log=Join-Path $root ('analysis/gpu-build-'+$stamp+'.log')
New-Item -ItemType Directory -Path $backup | Out-Null
$saved=@{}
foreach($ext in 'dll','pdb','lib','exp'){
 $file=Join-Path $out "rexgpu-xenos.$ext"
 if(Test-Path -LiteralPath $file){Copy-Item -LiteralPath $file -Destination $backup;$saved[$ext]=(Get-FileHash $file).Hash}
}
try {
 foreach($ext in 'dll','pdb'){Remove-Item -LiteralPath (Join-Path $out "rexgpu-xenos.$ext") -ErrorAction SilentlyContinue}
 & cmake --build (Join-Path $root 'analysis/rexglue-sdk-symbol-build') --target rexgpu-xenos --parallel 8 *> $log
 if($LASTEXITCODE){Get-Content $log -Tail 30;throw 'GPU build failed'}
 New-Item -ItemType Directory -Path $candidate | Out-Null
 foreach($ext in 'dll','pdb'){Copy-Item -LiteralPath (Join-Path $out "rexgpu-xenos.$ext") -Destination $candidate}
} finally {
 foreach($file in Get-ChildItem -LiteralPath $backup -File){Copy-Item -LiteralPath $file.FullName -Destination $out -Force}
 foreach($ext in $saved.Keys){if((Get-FileHash (Join-Path $out "rexgpu-xenos.$ext")).Hash -ne $saved[$ext]){throw "SDK out restore failed for rexgpu-xenos.$ext"}}
}
foreach($module in 'rexruntime','ao4_main_native'){
 foreach($ext in 'dll','pdb'){Copy-Item -LiteralPath (Join-Path $base "$module.$ext") -Destination $candidate}
}
Get-FileHash -LiteralPath (Join-Path $candidate 'rexgpu-xenos.dll'),(Join-Path $candidate 'rexruntime.dll'),(Join-Path $candidate 'ao4_main_native.dll') |
 ConvertTo-Json | Set-Content -LiteralPath (Join-Path $candidate 'hashes.json') -Encoding UTF8
Write-Output "Candidate: $candidate; log: $log; SDK out restored from $backup"
