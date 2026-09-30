@echo off
rem Windows entrypoint (template; @BUILD_ID@ is filled in by stage_build.sh). Runs natively
rem and under Proton (Wine's cmd; no PowerShell there).
rem Launches The Dark Mod straight into a state where the arcade SDK can serve StartChallenge:
rem main menu, SDK listening on 127.0.0.1:6006 (ARCADE_SDK_PORT), no dialogs to click through.
rem fs_currentfm=arcade is the combined mission folder built by stage_build.sh (A New Job + Saint Lucia).
rem
rem Opens a plain (non-exclusive) window sized from SCREEN_WIDTH x SCREEN_HEIGHT (set by the
rem arcade machines; defaults to 2560x1440). Config, logs and saves are written inside this folder,
rem as on Linux: fs_savepath must stay the executable's folder, since fms\arcade is looked up under it.
rem Extra arguments are passed to the game.
setlocal
rem arcade-sdk may start us with a \\?\ (long path) name, which cmd cannot cd into
set "HERE=%~dp0"
if "%HERE:~0,4%"=="\\?\" set "HERE=%HERE:~4%"
cd /d "%HERE%"
set W=%SCREEN_WIDTH%
if "%W%"=="" set W=2560
set H=%SCREEN_HEIGHT%
if "%H%"=="" set H=1440
"%HERE%TheDarkModx64.exe" ^
    +set arcade_enable 1 +set arcade_buildId @BUILD_ID@ ^
    +set fs_currentfm arcade ^
    +set r_fullscreen 0 ^
    +set r_customWidth %W% +set r_customHeight %H% ^
    +set tdm_player_wait_until_ready 0 ^
    +set com_showFPS 0 ^
    %*
exit /b %ERRORLEVEL%
