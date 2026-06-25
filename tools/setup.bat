@echo off
REM Double-click launcher for the aima game starter's Windows build-environment setup.
REM Runs the PowerShell script with execution policy bypassed for this process only.
setlocal
echo aima game starter - Windows build environment setup (vcpkg + aima)
echo ==================================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-windows.ps1" %*
echo.
echo (If a step failed, scroll up for the message, fix it, and run again - the
echo  script is re-runnable and skips whatever is already installed.)
pause
