# install.ps1 — one-click bootstrap for CannonFortnite.
#
# Does everything a human would do by hand, in order:
#   1. elevate (self-relaunch as admin if not already)
#   2. verify / install prerequisites (vs buildtools, cmake, git, wdk)
#   3. clone imgui into third_party/
#   4. compute the target image-hash, patch src/main.cpp
#   5. build user-mode EXE + both drivers
#   6. enable test signing + load drivers as services
#   7. create a start-menu shortcut to scripts/launch.ps1
#
# One command:
#     powershell -ep bypass -f scripts\install.ps1
#
# Attribution: Built by Olivia — https://discord.gg/ojailbreaks

$ErrorActionPreference = "Stop"
$Host.UI.RawUI.WindowTitle = "CF :: installer"

function Say($msg, $colour = "Cyan"){
    Write-Host ("[cf] " + $msg) -ForegroundColor $colour
}

function Need-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $pr = New-Object Security.Principal.WindowsPrincipal($id)
    if(-not $pr.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){
        Say "elevating..." "Yellow"
        Start-Process powershell -Verb RunAs -ArgumentList @(
            "-NoProfile","-ExecutionPolicy","Bypass","-File",$PSCommandPath)
        exit
    }
}

function Have-Cmd($name){
    return [bool](Get-Command $name -ErrorAction SilentlyContinue)
}

function Ensure-Winget {
    if(Have-Cmd winget){ return }
    Say "winget missing — install the App Installer from the Microsoft Store, then re-run." "Red"
    Start-Process "ms-windows-store://pdp/?productid=9NBLGGH4NNS1"
    exit 2
}

function Install-Prereq($id, $display){
    Say "ensuring $display..."
    try {
        winget list --id $id --accept-source-agreements 2>$null | Out-Null
        if($LASTEXITCODE -eq 0){
            Say "  already installed" "DarkGray"
            return
        }
    } catch {}
    winget install --id $id --silent --accept-source-agreements --accept-package-agreements | Out-Null
    Say "  installed" "Green"
}

function Fnv1a-Lower($s){
    $h = [uint64]0xcbf29ce484222325
    foreach($c in $s.ToLower().ToCharArray()){
        $h = $h -bxor [byte][char]$c
        $h = ($h * 0x100000001b3) -band 0xFFFFFFFFFFFFFFFF
    }
    return ("{0:x}" -f $h)
}

function Find-VsDev {
    $ws = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if(-not (Test-Path $ws)){ return $null }
    $path = & $ws -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if(-not $path){ return $null }
    return Join-Path $path "VC\Auxiliary\Build\vcvars64.bat"
}

# ---- run ----

Need-Admin
Say "CannonFortnite installer" "Green"

Ensure-Winget
Install-Prereq "Git.Git"                             "git"
Install-Prereq "Kitware.CMake"                       "cmake"
Install-Prereq "Microsoft.VisualStudio.2022.BuildTools" "visual studio build tools"
# the wdk is optional — only needed if you're rebuilding drivers.
# comment the next line out if you only want user-mode.
Install-Prereq "Microsoft.WindowsWDK.10.0.22621"     "windows driver kit"

$root  = Split-Path -Parent $PSScriptRoot
Push-Location $root

# ---- imgui ----
if(-not (Test-Path "third_party\imgui\imgui.h")){
    Say "vendoring imgui..."
    if(-not (Test-Path "third_party")){ New-Item -ItemType Directory -Path "third_party" | Out-Null }
    git clone --depth=1 --branch v1.91.0 https://github.com/ocornut/imgui third_party\imgui | Out-Null
    Say "  imgui vendored" "Green"
} else {
    Say "imgui already vendored" "DarkGray"
}

# ---- target image hash ----
$hash = Fnv1a-Lower "FortniteClient-Win64-Shipping.exe"
Say "target image hash: $hash"
$main = Get-Content "src\main.cpp" -Raw
$new  = $main -replace 'const wchar_t\* targetHash = L"[0-9a-fA-F]+";',
                       ('const wchar_t* targetHash = L"' + $hash + '";')
if($new -ne $main){
    [IO.File]::WriteAllText("$root\src\main.cpp", $new)
    Say "  patched src/main.cpp" "Green"
}

# ---- build user-mode ----
Say "configuring cmake..."
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -Wno-dev | Out-Null
Say "building..."
cmake --build build --config Release --parallel | Out-Null
$exe = Join-Path $root "build\bin\Release\MicrosoftEdgeUpdateBroker.exe"
if(-not (Test-Path $exe)){ Say "build failed." "Red"; exit 3 }
Say "  built: $exe" "Green"

# ---- drivers ----
# This repo ships driver/*.c + .inf + cf_ioctl.h — no Visual Studio
# .vcxproj files. If you want the drivers, open the WDK "KMDF Driver"
# template, add our .c files + driver/cf_ioctl.h, build x64 Release.
# The installer no longer pretends to build them.
$cfRead  = Join-Path $root "driver\x64\Release\CfRead.sys"
$cfSpoof = Join-Path $root "driver\x64\Release\CfSpoof.sys"
if((Test-Path $cfRead) -and (Test-Path $cfSpoof)){
    Say "pre-built drivers found" "Green"
} else {
    Say "drivers NOT present — kernel path disabled; user-mode fallback only." "Yellow"
}

# ---- test signing + driver services ----
$kread = $cfRead
$spoof = $cfSpoof
if((Test-Path $kread) -and (Test-Path $spoof)){
    Say "enabling test-signing (reboot required to take effect if currently off)..."
    bcdedit /set testsigning on | Out-Null
    # v5: service names mirror real Windows services so a registry
    # enumeration doesn't flag. The driver binary itself still exports
    # the same device object names internally (env_lie hides the ones
    # the AC would enumerate).
    foreach($svc in @(
        @{Name="RpcEptCache";  Path=$kread},
        @{Name="ShellHWAgent"; Path=$spoof})){
        sc.exe query $svc.Name 2>$null | Out-Null
        if($LASTEXITCODE -eq 0){
            sc.exe stop   $svc.Name | Out-Null
            sc.exe delete $svc.Name | Out-Null
        }
        sc.exe create $svc.Name type= kernel start= demand binPath= $svc.Path | Out-Null
        sc.exe start  $svc.Name | Out-Null
        Say "  service $($svc.Name) armed" "Green"
    }
}

# ---- shortcut ----
$lnk = Join-Path ([Environment]::GetFolderPath("Desktop")) "cannon ripper.lnk"
$ws  = New-Object -ComObject WScript.Shell
$sh  = $ws.CreateShortcut($lnk)
$sh.TargetPath       = "powershell.exe"
$sh.Arguments        = "-NoProfile -ExecutionPolicy Bypass -File `"$root\scripts\launch.ps1`""
$sh.WorkingDirectory = $root
$sh.IconLocation     = "$exe,0"
$sh.Save()
Say "desktop shortcut: $lnk" "Green"

Pop-Location

Say "done. launch from the desktop or run scripts\launch.ps1" "Green"
Say "if you just enabled test-signing you MUST reboot once." "Yellow"
