@echo off
echo Deploying PixelSort.dll to CSP plugin directory...
echo (Requires Administrator privileges)
echo.
copy /Y "%~dp0PixelSort\PixelSortPlugin\OutputWin\PixelSort\PixelSort\Release\x64\PixelSort.dll" "C:\Program Files\CELSYS\CLIP STUDIO 1.5\CLIP STUDIO PAINT\PlugIn\PAINT\PixelSort.dll"
if %ERRORLEVEL% NEQ 0 (
    echo DEPLOY FAILED - Try running as Administrator
    pause
    exit /b 1
)
echo.
echo Deployed! Restart Clip Studio Paint to load the plugin.
pause
