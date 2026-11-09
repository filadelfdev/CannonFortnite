# launch.ps1 — one-shot session starter.
#
# Does the pre-flight: makes sure drivers are running, waits for the
# game, then launches svchost_helper.exe as admin.
#
# Attribution: Built by Olivia — https://discord.gg/ojailbreaks

$ErrorActionPreference = "SilentlyContinue"
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "build\bin\Release\svchost_helper.exe"

function Say($msg, $colour = "Cyan"){ Write-Host ("[cf] " + $msg) -ForegroundColor $colour }

if(-not (Test-Path $exe)){
    Say "not built yet — run scripts\install.ps1 first." "Red"
    pause; exit 1
}

# elevate
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if(-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    Start-Process powershell -Verb RunAs -ArgumentList @(
        "-NoProfile","-ExecutionPolicy","Bypass","-File",$PSCommandPath)
    exit
}

# ensure services
foreach($svc in @("CfRead","CfSpoof")){
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if(-not $s){ Say "  $svc not installed (optional)" "DarkYellow"; continue }
    if($s.Status -ne "Running"){ Start-Service $svc; Say "  $svc started" "Green" }
    else { Say "  $svc already running" "DarkGray" }
}

# wait for fortnite if not running
$proc = $null
for($i=0; $i -lt 60; ++$i){
    $proc = Get-Process "FortniteClient-Win64-Shipping" -ErrorAction SilentlyContinue
    if($proc){ break }
    if($i -eq 0){ Say "waiting for Fortnite..." "Yellow" }
    Start-Sleep 2
}
if(-not $proc){
    Say "Fortnite didn't start within 120s — launching the app anyway." "DarkYellow"
}

Say "launching ripper..." "Green"
Start-Process $exe -Verb RunAs
