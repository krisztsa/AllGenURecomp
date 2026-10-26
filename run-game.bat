@echo off
rem Starts AllGenURecomp from the build folder (its DLLs are beside it, the game files in build\data\exefs and
rem build\data\romfs; saves go to %APPDATA%\AllGenURecomp\save\). Settings, caches, mods\ and the log are kept in run\
rem (MHGU_HOME), so build\ stays exactly what a game folder needs: copy its contents wherever you play.
set MHGU_HOME=%~dp0run
if not exist "%MHGU_HOME%" mkdir "%MHGU_HOME%"
"%~dp0build\allgenurecomp.exe" 2> "%MHGU_HOME%\log.txt"
if errorlevel 1 (
    echo The game exited with an error. See run\log.txt
    pause
)
