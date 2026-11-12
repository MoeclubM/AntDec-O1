@echo off
setlocal
for %%I in ("%~dp0..") do set "ANTDEC_XMOS=%%~fI"
if not defined XMOS_TOOL_PATH set "XMOS_TOOL_PATH=%ProgramFiles%\XMOS\XTC\15.3.1"
call "%XMOS_TOOL_PATH%\SetEnv.bat" >nul
if errorlevel 1 exit /b 1
set "ANTDEC_CMAKE=cmake"
where cmake >nul 2>&1
if errorlevel 1 for /d %%I in ("%LOCALAPPDATA%\stm32cube\bundles\cmake\*") do if exist "%%~fI\bin\cmake.exe" set "ANTDEC_CMAKE=%%~fI\bin\cmake.exe"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0prepare_dependencies.ps1"
if errorlevel 1 exit /b 1
"%ANTDEC_CMAKE%" -S "%ANTDEC_XMOS%\app_antdec_o1" -B "%ANTDEC_XMOS%\app_antdec_o1\build" -G "Unix Makefiles"
if errorlevel 1 exit /b 1
xmake -C "%ANTDEC_XMOS%\app_antdec_o1\build" -j8
if errorlevel 1 exit /b 1
xflash --factory "%ANTDEC_XMOS%\app_antdec_o1\bin\app_antdec_o1.xe" --boot-partition-size 0x100000 -o "%ANTDEC_XMOS%\app_antdec_o1\bin\app_antdec_o1_flash.bin"
if errorlevel 1 exit /b 1
xflash --factory-version 15.3 --upgrade 1 "%ANTDEC_XMOS%\app_antdec_o1\bin\app_antdec_o1.xe" -o "%ANTDEC_XMOS%\app_antdec_o1\bin\app_antdec_o1_dfu.bin"
exit /b %errorlevel%

