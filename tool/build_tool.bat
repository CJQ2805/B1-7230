@echo off
setlocal

set CSC_DLL=C:\Program Files\dotnet\sdk\9.0.300\Roslyn\bincore\csc.dll
set REF_DIR=C:\Program Files (x86)\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8

dotnet "%CSC_DLL%" ^
  /noconfig ^
  /nostdlib+ ^
  /target:exe ^
  /platform:anycpu ^
  /optimize+ ^
  /out:"%~dp0tool.exe" ^
  /reference:"%REF_DIR%\mscorlib.dll" ^
  /reference:"%REF_DIR%\System.dll" ^
  /reference:"%REF_DIR%\System.Core.dll" ^
  /reference:"%REF_DIR%\System.Web.Extensions.dll" ^
  "%~dp0tool.cs"
