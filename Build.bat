@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dev\build.ps1" -Quick %*
exit /b %errorlevel%
