@echo off
REM One-click build for a clean clone: submodules + ONNX Runtime (.so from source) + APK.
REM Forwards any args to build.ps1, e.g.:  build.bat -Release
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
