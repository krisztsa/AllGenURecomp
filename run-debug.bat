@echo off
rem Like run-game.bat, with renderer debugging on: F10 in the game window dumps every render pass of the next 2 frames
rem to run\shots\ (takes a few seconds), shaders are saved to run\build\glsl\ and the log is run\log_debug.txt.
set MHGU_DUMP_VERBOSE=1
set MHGU_SAVE_GLSL=1
set MHGU_SAMPLER_LOG=1
set MHGU_HOME=%~dp0run
if not exist "%MHGU_HOME%" mkdir "%MHGU_HOME%"
"%~dp0build\allgenurecomp.exe" 2> "%MHGU_HOME%\log_debug.txt"
if errorlevel 1 (
    echo The game exited with an error. See run\log_debug.txt
    pause
)
