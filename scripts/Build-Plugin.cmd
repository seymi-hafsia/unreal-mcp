@echo off
setlocal
powershell -ExecutionPolicy Bypass -File "%~dp0Build-Plugin.ps1" %*
endlocal
