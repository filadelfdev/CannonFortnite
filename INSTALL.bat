@echo off
title cannon ripper :: one-click install
color 0B

echo.
echo   cannon chest ripper
echo   built by olivia  --  discord.gg/ojailbreaks
echo.
echo   this installs every prerequisite (git, cmake, vs build tools, wdk),
echo   vendors imgui, builds the app + drivers, loads kernel services,
echo   and drops a shortcut on your desktop.
echo.
echo   press any key to start, or close this window to cancel.
pause >nul

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\install.ps1"

echo.
echo   done. hit any key to close.
pause >nul
