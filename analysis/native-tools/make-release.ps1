# Builds the public test package (no game data): AO4Launcher.exe + bin (recompiled port binaries,
# app-local VC++ runtime) + config defaults + server (embedded Python + Blaze script) + readmes.
# The player supplies their own ISO on first start (launcher setup page).
#   -Candidate   folder in analysis/ with ao4_main_native.dll, rexruntime.dll, rexgpu-xenos.dll
#   -Version     package version (also shown in the launcher)
#   -Zip         also create the ZIP
param([ValidatePattern('^sdk-[a-z0-9-]+$')][string]$Candidate='sdk-release-rt-20261109',
      [ValidatePattern('^[0-9a-z.\-]+$')][string]$Version='0.1.0-test1',
      [switch]$Zip)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$name="AO4-NativePort-$Version"
$out=Join-Path $root "analysis/release/$name"
if(Test-Path -LiteralPath $out){Remove-Item -LiteralPath $out -Recurse -Force}
$bin=Join-Path $out 'bin'; $config=Join-Path $out 'config'; $server=Join-Path $out 'server'
New-Item -ItemType Directory -Path $out,$bin,$config,$server,(Join-Path $server 'python') | Out-Null

# Launcher (frontend) build.
& (Join-Path $PSScriptRoot 'build-frontend.ps1') -Target AO4Launcher | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'analysis/ao4-frontend-build/AO4Launcher.exe') -Destination $out
# Bug capture tool (launcher option, off by default).
& (Join-Path $PSScriptRoot 'build-bugcam.ps1') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'analysis/ao4-frontend-build/ao4_bugcam.exe') -Destination $bin

# Game binaries: launcher exe + recompiled splash/buildinfo modules, candidate main/runtime/GPU.
$lb=Join-Path $root 'analysis/rexglue-launcher-build'
foreach($f in 'ao4_launcher.exe','ao4_launcher_AO4_Splash.dll','ao4_launcher_Engine_BuildInfo.dll'){
 Copy-Item -LiteralPath (Join-Path $lb $f) -Destination $bin
}
$cand=Join-Path $root "analysis/$Candidate"
foreach($f in 'ao4_main_native.dll','rexruntime.dll','rexgpu-xenos.dll'){Copy-Item -LiteralPath (Join-Path $cand $f) -Destination $bin}
# App-local Visual C++ runtime (redistributable files of Visual Studio 2022).
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$crt=Get-ChildItem -Path (Join-Path $vs 'VC/Redist/MSVC') -Directory | Where-Object {$_.Name -match '^\d+\.'} |
 Sort-Object {[version]$_.Name} -Descending | ForEach-Object {Join-Path $_.FullName 'x64/Microsoft.VC143.CRT'} |
 Where-Object {Test-Path $_} | Select-Object -First 1
if(-not $crt){throw 'VC++ redistributable folder not found'}
Copy-Item -Path (Join-Path $crt '*.dll') -Destination $bin

# Config defaults (the switches of the normal starters; 60 FPS and splash skip off).
$qol=Get-Content -LiteralPath (Join-Path $root 'native/quality-of-life.ini') -Raw -Encoding UTF8
$qol=$qol -replace '(?m)^bildrate_60\s*=.*$','bildrate_60 = aus' -replace '(?m)^splash_ueberspringen\s*=.*$','splash_ueberspringen = aus'
$qol=$qol -replace 'Wirkt nur mit dem 60-FPS-Stand der Haupt-DLL \(Start-60FPS\.cmd\)\.','Im Launcher unter Einstellungen > Grafik.'
[IO.File]::WriteAllText((Join-Path $config 'quality-of-life.ini'),$qol,(New-Object Text.UTF8Encoding($false)))
# Key bindings of the tested starters; release defaults: English game language, F4 port menu.
$bindings=Get-Content -LiteralPath (Join-Path $root 'native/input-bindings.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$bindings.language='en'
if(-not $bindings.hotkeys){$bindings | Add-Member hotkeys ([pscustomobject]@{})}
$bindings.hotkeys | Add-Member port_menu 'F4' -Force
[IO.File]::WriteAllText((Join-Path $config 'input-bindings.json'),($bindings | ConvertTo-Json -Depth 5),(New-Object Text.UTF8Encoding($false)))
@{invert_y=$false;radians_per_count=0.0012418426900000002;turret_horizontal=1.625548755750484;turret_invert_y=$false;turret_vertical=2.187761623949553} |
 ConvertTo-Json | Set-Content -LiteralPath (Join-Path $config 'native-mouse.json') -Encoding ASCII
Copy-Item -LiteralPath (Join-Path $root 'native/partner-customizer/equipment.ini') -Destination (Join-Path $config 'partner-equipment.ini')
# Start in the original Alpha mode with the original partner gear (F9 / F6 switch and remember).
Set-Content -LiteralPath (Join-Path $config 'partner-active.ini') -Encoding ASCII -Value "[state]`r`nactive=0`r`nbravo=0"

# Online/LAN host: embedded Python 3.11 (stdlib only) + our Blaze server.
$py=Join-Path $root 'tools/python'
Get-ChildItem -LiteralPath $py -File | Where-Object {$_.Name -notmatch '\._pth$|^python\.cat$|^pythonw\.exe$|^sqlite3\.dll$|^_sqlite3\.pyd$'} |
 Copy-Item -Destination (Join-Path $server 'python')
Set-Content -LiteralPath (Join-Path $server 'python/python311._pth') -Value "python311.zip`r`n." -Encoding ASCII
foreach($f in 'ao4-blaze-server.py','blaze_tdf.py'){Copy-Item -LiteralPath (Join-Path $root "analysis/native-tools/$f") -Destination $server}
# Optional firewall rules (only on request of the player; asks for admin rights). The script's
# root ('..' from server\) is the package folder.
Copy-Item -LiteralPath (Join-Path $root 'native/LAN-Koop-Firewall.ps1') -Destination $server
Set-Content -LiteralPath (Join-Path $out 'LAN-Koop-Firewall.cmd') -Encoding ASCII -Value @'
@echo off
rem Optional: Windows firewall rules for LAN/VPN co-op (asks for admin rights). Undo: LAN-Koop-Firewall.cmd -Remove
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0server\LAN-Koop-Firewall.ps1" %*
'@

# License notices (BSD-3-Clause of the ReXGlue SDK requires its notice with the binaries).
$licenses=Join-Path $out 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
$sdkSrc=Join-Path $root 'tools/rexglue-sdk-source-0.10.0'
Copy-Item -LiteralPath (Join-Path $sdkSrc 'LICENSE') -Destination (Join-Path $licenses 'ReXGlue-SDK-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $sdkSrc 'thirdparty/imgui/LICENSE.txt') -Destination (Join-Path $licenses 'Dear-ImGui-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'source-export-templates/LICENSE') -Destination (Join-Path $licenses 'AO4-Native-Port-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'source-export-templates/THIRD_PARTY.md') -Destination (Join-Path $licenses 'THIRD_PARTY.md')

# Readmes.
foreach($f in 'LIESMICH.txt','README.txt'){
 $src=Join-Path $root "native/release/$f"
 (Get-Content -LiteralPath $src -Raw -Encoding UTF8).Replace('{VERSION}',$Version) |
  ForEach-Object {[IO.File]::WriteAllText((Join-Path $out $f),$_,(New-Object Text.UTF8Encoding($true)))}
}

# Privacy: the compilers embed build paths (source file names in asserts/logs, PDB path in the
# debug directory) that contain the local user folder. Replace the build root with a neutral path of
# the same length (code is unchanged; only these strings), in ASCII and UTF-16.
$buildRoot=$root.TrimEnd('\')
$neutral='C:\build\ao4-native-port\source'
if($neutral.Length -lt $buildRoot.Length){$neutral=$neutral.PadRight($buildRoot.Length,'_')}
$neutral=$neutral.Substring(0,$buildRoot.Length)
$latin1=[Text.Encoding]::GetEncoding(28591)
$patterns=@()
foreach($pair in @(@($buildRoot,$neutral),@($buildRoot.Replace('\','/'),$neutral.Replace('\','/')))){
 $patterns+=,@([regex]::Escape($pair[0]),$pair[1])
 $patterns+=,@((($pair[0].ToCharArray()|ForEach-Object{[regex]::Escape([string]$_)}) -join '\x00'),(($pair[1].ToCharArray()) -join "`0"))
}
foreach($f in Get-ChildItem -LiteralPath $out -Recurse -File | Where-Object {$_.Extension -in '.exe','.dll'} | Where-Object {$_.DirectoryName -notmatch '\\server\\python'}){
 $text=$latin1.GetString([IO.File]::ReadAllBytes($f.FullName))
 $new=$text
 foreach($p in $patterns){$new=[regex]::Replace($new,$p[0],{param($m) $p[1]},'IgnoreCase')}
 if($new -ne $text){
  if($new.Length -ne $text.Length){throw "Path scrub changed the size of $($f.Name)"}
  [IO.File]::WriteAllBytes($f.FullName,$latin1.GetBytes($new))
 }
}
# Guard: no personal data (user folder, account names, addresses, profile ids) in any file.
$private=@('Users\'+$env:USERNAME+'\','Users/'+$env:USERNAME+'/',$buildRoot,$buildRoot.Replace('\','/'),'AppData\Local\Temp\claude')
foreach($f in Get-ChildItem -LiteralPath $out -Recurse -File){
 $bytes=[IO.File]::ReadAllBytes($f.FullName)
 foreach($enc in [Text.Encoding]::ASCII,[Text.Encoding]::Unicode){
  $text=$enc.GetString($bytes)
  foreach($t in $private){if($text.IndexOf($t,[StringComparison]::OrdinalIgnoreCase) -ge 0){throw "Private string '$t' in $($f.FullName)"}}
 }
}

# Guard: nothing of the game may be inside (disc/HD files, xex, big archives, profiles, logs).
$bad=Get-ChildItem -LiteralPath $out -Recurse -File | Where-Object {
 $_.Extension -in '.xex','.iso','.sb','.toc','.cas','.cat','.mft','.bik','.xma','.xpr','.xsh','.xpso','.pdb' -or
 $_.Name -match '^(default|ao4\.main|ao4\.splash)\.xex$' -or $_.FullName -match '\\(game|profiles|logs|user|extracted_disc|extracted_hd)\\'}
if($bad){$bad.FullName; throw 'Package contains game data or private files'}

$files=Get-ChildItem -LiteralPath $out -Recurse -File
$files | ForEach-Object {[pscustomobject]@{Path=$_.FullName.Substring($out.Length+1);Size=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash}} |
 ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root "analysis/release/$name-manifest.json") -Encoding UTF8
"{0} files, {1:N0} MB in {2}" -f $files.Count,(($files|Measure-Object Length -Sum).Sum/1MB),$out
if($Zip){
 $zipFile=Join-Path $root "analysis/release/$name.zip"
 if(Test-Path -LiteralPath $zipFile){Remove-Item -LiteralPath $zipFile}
 & "$env:SystemRoot\System32\tar.exe" -a -c -f $zipFile -C (Join-Path $root 'analysis/release') $name
 if($LASTEXITCODE){throw 'ZIP failed'}
 "ZIP: $zipFile ({0:N0} MB)" -f ((Get-Item $zipFile).Length/1MB)
}
