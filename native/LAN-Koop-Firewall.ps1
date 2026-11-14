# Optionale Windows-Firewall-Freigabe fuer LAN/VPN-Koop (Adminrechte noetig, fragt per UAC).
# Legt zwei Eingangsregeln der Gruppe "AO4 LAN-Koop" an:
#   UDP 1000  (Spielverkehr, Host und Gast)   TCP 42124 (Mini-Blaze-Server, nur Host noetig)
# jeweils nur fuer Gegenstellen aus dem eigenen Subnetz und den virtuellen LANs
# Tailscale (100.64.0.0/10) und Hamachi (25.0.0.0/8). Gilt fuer alle Netzwerkprofile, weil
# Heimnetze oft als "Oeffentlich" eingestuft sind.
# Entfernt ausserdem Sperrregeln, die Windows nach "Abbrechen" im Firewall-Dialog fuer Programme
# AUS DIESEM PROJEKTORDNER angelegt hat (z.B. ao4_launcher.exe, python.exe), sonst nichts.
# Rueckgaengig: LAN-Koop-Firewall.cmd -Remove
param([switch]$Remove,[switch]$Elevated)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$principal=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
 if($Elevated){throw 'Adminrechte wurden nicht gewaehrt.'}
 $argsList=@('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"",'-Elevated')
 if($Remove){$argsList+='-Remove'}
 Start-Process powershell.exe -Verb RunAs -ArgumentList $argsList -Wait
 return
}
$group='AO4 LAN-Koop'
Get-NetFirewallRule -Group $group -ErrorAction SilentlyContinue | Remove-NetFirewallRule
if($Remove){Write-Host "Regeln der Gruppe '$group' entfernt."; Read-Host 'Enter zum Schliessen'; return}
$remote=@('LocalSubnet','100.64.0.0/10','25.0.0.0/8')
New-NetFirewallRule -DisplayName 'AO4 LAN-Koop Spiel (UDP 1000)' -Group $group -Direction Inbound -Protocol UDP -LocalPort 1000 -RemoteAddress $remote -Profile Any -Action Allow | Out-Null
New-NetFirewallRule -DisplayName 'AO4 LAN-Koop Server (TCP 42124)' -Group $group -Direction Inbound -Protocol TCP -LocalPort 42124 -RemoteAddress $remote -Profile Any -Action Allow | Out-Null
Write-Host "Angelegt: UDP 1000 und TCP 42124 eingehend, nur von $($remote -join ', ')."
$prefix=$root.TrimEnd('\')+'\'
$blocks=Get-NetFirewallRule -Direction Inbound -Action Block -ErrorAction SilentlyContinue | Where-Object {
 $app=($_ | Get-NetFirewallApplicationFilter).Program
 $app -and $app.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)
}
foreach($rule in @($blocks)){
 $app=($rule | Get-NetFirewallApplicationFilter).Program
 Write-Host "Entferne Sperrregel '$($rule.DisplayName)' fuer $app"
 $rule | Remove-NetFirewallRule
}
if(-not $blocks){Write-Host 'Keine Sperrregeln fuer Programme aus diesem Ordner gefunden.'}
Read-Host 'Fertig. Enter zum Schliessen'
