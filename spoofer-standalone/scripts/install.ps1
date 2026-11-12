# install.ps1 — CannonSpoofer standalone installer.

$ErrorActionPreference = "Stop"

function Say($msg, $colour="Cyan"){ Write-Host ("[spoofer] " + $msg) -ForegroundColor $colour }

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$pr = New-Object Security.Principal.WindowsPrincipal($id)
if(-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
    Say "elevating..." "Yellow"
    Start-Process powershell -Verb RunAs -ArgumentList @(
        "-NoProfile","-ExecutionPolicy","Bypass","-File",$PSCommandPath)
    exit
}

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

# ---- build the standalone EXEs ----
Say "configuring cmake..."
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -Wno-dev | Out-Null
Say "building..."
cmake --build build --config Release --parallel | Out-Null

$exe = Join-Path $root "build\bin\Release\spoofer.exe"
if(Test-Path $exe){ Say "  built: spoofer.exe" "Green" }
else { Say "  MISSING: spoofer.exe" "Red"; exit 1 }

# ---- driver service (ShellHWAgent disguise name) ----
$drv = Join-Path $root "driver\x64\Release\spoof.sys"
if(Test-Path $drv){
    Say "enabling test-signing..."
    bcdedit /set testsigning on | Out-Null
    sc.exe query ShellHWAgent 2>$null | Out-Null
    if($LASTEXITCODE -eq 0){
        sc.exe stop   ShellHWAgent | Out-Null
        sc.exe delete ShellHWAgent | Out-Null
    }
    sc.exe create ShellHWAgent type= kernel start= demand binPath= $drv | Out-Null
    sc.exe start  ShellHWAgent | Out-Null
    Say "driver loaded as service ShellHWAgent" "Green"
} else {
    Say "driver not built (skip — spoofer still runs user-mode only)" "Yellow"
}

# ---- desktop shortcut (admin, for the interactive menu) ----
$lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "CannonSpoofer.lnk"
$ws  = New-Object -ComObject WScript.Shell
$sh  = $ws.CreateShortcut($lnk)
$sh.TargetPath       = $exe
$sh.WorkingDirectory = (Split-Path $exe)
$sh.Save()

Pop-Location
Say "installed. reboot if test-signing was just turned on." "Green"
Say "then double-click 'CannonSpoofer' on the desktop (as admin)." "Green"
