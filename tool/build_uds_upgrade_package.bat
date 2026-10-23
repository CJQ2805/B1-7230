@echo off
setlocal

set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "TOOL_DIR=%~dp0"
if not defined UVISION set "UVISION=E:\Keil_v5\UV4\uVision.com"
if not defined PYTHON set "PYTHON=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"

if not exist "%UVISION%" (
    echo Keil command line tool not found: %UVISION%
    exit /b 1
)
if not exist "%PYTHON%" (
    echo Python 3 not found: %PYTHON%
    echo Set PYTHON to a Python 3 executable and retry.
    exit /b 1
)
if not exist "%TOOL_DIR%SeednKey.dll" (
    echo ZCANPRO SeedKey DLL is missing: %TOOL_DIR%SeednKey.dll
    exit /b 1
)
if not exist "%TOOL_DIR%BMS-DUCAR_FlashDriver.s19" (
    echo ZCANPRO FlashDriver placeholder is missing: %TOOL_DIR%BMS-DUCAR_FlashDriver.s19
    exit /b 1
)

rem Remove the previous package output before building so failed builds cannot leave a stale S19 importable.
if exist "%TOOL_DIR%A800000002_7230B1_V01_T12_APP1.s19" del /q "%TOOL_DIR%A800000002_7230B1_V01_T12_APP1.s19"
if exist "%TOOL_DIR%A800000002_7230B1_V01_T12_APP1.s19" (
    echo Failed to remove stale APP1 S19 output.
    exit /b 1
)

"%UVISION%" -b "%ROOT%\boot\Projects\MDK\boot.uvprojx" -t boot -o "%ROOT%\boot\Projects\MDK\Objects\build_uds.log" -j0
if errorlevel 1 exit /b 1
if not exist "%ROOT%\boot\Projects\MDK\Objects\boot.hex" (
    echo BOOT build did not produce boot.hex.
    exit /b 1
)
copy /y "%ROOT%\boot\Projects\MDK\Objects\boot.hex" "%TOOL_DIR%boot.hex" >nul
if errorlevel 1 exit /b 1

"%UVISION%" -b "%ROOT%\app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx" -t N32G45x -o "%ROOT%\app\N32G45x_KEIL\MDK-ARM\Objects\build_uds.log" -j0
if errorlevel 1 exit /b 1

"%TOOL_DIR%tool.exe" -p n32
if errorlevel 1 exit /b 1

"%PYTHON%" "%TOOL_DIR%generate_uds_app_s19.py"
if errorlevel 1 exit /b 1

echo UDS upgrade artifacts are ready:
echo   %TOOL_DIR%ZS_ECU_Flash_20250401.zflash
echo   %TOOL_DIR%A800000002_7230B1_V01_T12_APP1.s19
echo   %TOOL_DIR%BMS-DUCAR_FlashDriver.s19
echo   %TOOL_DIR%SeednKey.dll
