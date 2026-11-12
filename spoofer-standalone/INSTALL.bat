@echo off
title CannonSpoofer :: install
color 0E

echo.
echo   CannonSpoofer — standalone HWID spoofer
echo   built by olivia  --  discord.gg/ojailbreaks
echo.
echo   This installs the 4 standalone EXEs + driver service.
echo.
pause >nul

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1"

echo.
echo   done. hit any key to close.
pause >nul
