@echo off
setlocal

set "ROOT=%~dp0"
set "RELEASE=%ROOT%Emscripten\Release"
set "DOCS=%ROOT%docs"

if not exist "%RELEASE%\AirHockeyArena_Web.html" (
    echo Release build not found:
    echo   %RELEASE%
    exit /b 1
)

if not exist "%DOCS%" mkdir "%DOCS%"

rem Copy all Release assets except the HTML shell.
robocopy "%RELEASE%" "%DOCS%" /E /XF AirHockeyArena_Web.html /R:2 /W:1 /NFL /NDL /NJH /NJS /NP
if errorlevel 8 (
    echo Failed to copy Release assets.
    exit /b 1
)

copy /Y "%RELEASE%\AirHockeyArena_Web.html" "%DOCS%\index.html" >nul
if errorlevel 1 (
    echo Failed to create docs\index.html.
    exit /b 1
)

echo Release has been copied to docs\index.html.
exit /b 0
