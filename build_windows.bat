@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo CMake was not found. Install it from https://cmake.org/download/
    echo and select "Add CMake to the PATH" during installation.
    pause
    exit /b 1
)

echo Configuring. The first run downloads JUCE and can take a few minutes...
cmake -B build
if errorlevel 1 goto failed

echo Building Release...
cmake --build build --config Release --parallel
if errorlevel 1 goto failed

echo.
echo Done.
echo VST3 plug-in:
echo   %~dp0build\ReverseSampler_artefacts\Release\VST3\Reverse Sampler.vst3
echo Copy it to C:\Program Files\Common Files\VST3\ and rescan plug-ins in your DAW.
echo.
echo Standalone app:
echo   %~dp0build\ReverseSampler_artefacts\Release\Standalone\Reverse Sampler.exe
echo.
pause
exit /b 0

:failed
echo.
echo Build failed. See the messages above.
pause
exit /b 1