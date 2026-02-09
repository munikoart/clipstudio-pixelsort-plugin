@echo off
echo Building PixelSort plugin (Release x64)...
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" "%~dp0PixelSort\PixelSortPlugin\ProjectWin\PixelSort.sln" /p:Configuration=Release /p:Platform=x64
if %ERRORLEVEL% NEQ 0 (
    echo BUILD FAILED
    pause
    exit /b 1
)
echo.
echo Build succeeded!
echo Output: %~dp0PixelSort\PixelSortPlugin\OutputWin\PixelSort\PixelSort\Release\x64\PixelSort.dll
echo.
echo To deploy, run deploy.bat as Administrator
pause
