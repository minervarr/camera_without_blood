@echo off
setlocal enabledelayedexpansion

:: Get the first device ID from 'adb devices' (skipping the header)
for /f "skip=1 tokens=1" %%i in ('adb devices') do (
    set DEVICE_ID=%%i
    goto :found
)

:found
if "%DEVICE_ID%"=="" (
    echo No device found! Make sure your phone is connected and USB debugging is on.
    pause
    exit /b
)

echo Targeting device: %DEVICE_ID%

:: Take, pull, and remove using the detected device ID
adb -s %DEVICE_ID% shell screencap -p /sdcard/screenshot.png
adb -s %DEVICE_ID% pull /sdcard/screenshot.png
adb -s %DEVICE_ID% shell rm /sdcard/screenshot.png

echo.
echo Screenshot captured and saved to the current directory.
pause
