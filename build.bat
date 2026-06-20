@echo off
REM One-click build for a clean clone: submodules + ONNX Runtime (.so from source) + APK.
REM Forwards any args to build.ps1, e.g.:  build.bat -Release
setlocal
REM Prefer PowerShell 7 (pwsh) if installed; fall back to Windows PowerShell 5.1.
set "PS=powershell"
where pwsh >nul 2>nul && set "PS=pwsh"
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
