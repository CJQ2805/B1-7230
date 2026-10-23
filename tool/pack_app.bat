@echo off
setlocal

set PACK_EXE=%~dp0tool.exe
set INPUT=%~1
set PART=%~2
set OUTPUT=%~3

if "%INPUT%"=="" (
    echo 用法1: pack_app.bat ^<raw_app.bin^> ^<10字节品号^> [output_iap.bin]
    echo 用法2: tool.exe -p n32
    exit /b 1
)

if "%PART%"=="" (
    echo 用法1: pack_app.bat ^<raw_app.bin^> ^<10字节品号^> [output_iap.bin]
    echo 用法2: tool.exe -p n32
    exit /b 1
)

if "%OUTPUT%"=="" (
    "%PACK_EXE%" --input "%INPUT%" --part "%PART%"
) else (
    "%PACK_EXE%" --input "%INPUT%" --part "%PART%" --output "%OUTPUT%"
)
