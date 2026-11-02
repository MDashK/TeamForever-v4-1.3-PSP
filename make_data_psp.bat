@echo off
setlocal
rem Sonic 1 Forever / Sonic 2 Absolute for the PSP: makes the game's data (Data.psp) from your own Data.rsdk.
rem
rem Drag your Data.rsdk onto this file (or: make_data_psp.bat "C:\path\to\Data.rsdk").
rem The game is told by the file itself (Sonic 1 -> Sonic 1 Forever, Sonic 2 -> Sonic 2 Absolute); Data.psp goes to
rem PSP\GAME\<game>\ next to this file (with the EBOOT.PBP): copy that PSP folder to the root of the Memory Stick.
rem
rem Needs: Python 3, ffmpeg (tools\ffmpeg\ffmpeg.exe or on the PATH), atracdenc (tools\atracdenc\atracdenc.exe).

set "RSDK=%~1"
set "HERE=%~dp0"

if "%RSDK%"=="" (
    echo Drag your Data.rsdk onto make_data_psp.bat
    goto fail
)
if not exist "%RSDK%" (
    echo Data.rsdk not found: %RSDK%
    goto fail
)

set "PYTHON="
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYTHON set "PYTHON=%%P"
if not defined PYTHON for /f "delims=" %%P in ('where py 2^>nul') do if not defined PYTHON set "PYTHON=%%P"
if not defined PYTHON (
    echo Python 3 not found: install it from https://www.python.org ^(tick "Add python.exe to PATH"^)
    goto fail
)

set "FFMPEG=%HERE%tools\ffmpeg\ffmpeg.exe"
if not exist "%FFMPEG%" (
    for /f "delims=" %%F in ('where ffmpeg 2^>nul') do set "FFMPEG=%%F"
)
if not exist "%FFMPEG%" (
    echo ffmpeg not found: put ffmpeg.exe in tools\ffmpeg\ or on the PATH ^(https://ffmpeg.org^)
    goto fail
)

set "ATRACDENC=%HERE%tools\atracdenc\atracdenc.exe"
if not exist "%ATRACDENC%" (
    echo atracdenc not found: put atracdenc.exe in tools\atracdenc\
    echo ^(https://github.com/dcherednik/atracdenc/releases^)
    goto fail
)

"%PYTHON%" "%HERE%tools\build_psp_data.py" "%RSDK%" --out "%HERE%." --ffmpeg "%FFMPEG%" --atracdenc "%ATRACDENC%" --cache "%HERE%cache"
if errorlevel 1 goto fail

echo.
echo Done: copy the PSP folder next to make_data_psp.bat to the root of the Memory Stick.
echo (the "cache" folder only makes building again quicker: it can be deleted)
pause
exit /b 0

:fail
pause
exit /b 1
