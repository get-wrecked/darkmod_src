#!/bin/sh
# Launches The Dark Mod straight into a state where the arcade SDK can serve StartChallenge:
# main menu, SDK listening on 127.0.0.1:6006 (ARCADE_SDK_PORT), no dialogs to click through.
#
# Opens a plain (non-exclusive) window on $DISPLAY sized from SCREEN_WIDTH x SCREEN_HEIGHT
# (set by the arcade machines; defaults to 2560x1440). Extra arguments are passed to the game.
cd "$(dirname "$0")"
exec ./thedarkmod.x64 \
    +set arcade_enable 1 +set arcade_buildId @BUILD_ID@ \
    +set fs_currentfm training_mission \
    +set r_fullscreen 0 \
    +set r_customWidth "${SCREEN_WIDTH:-2560}" +set r_customHeight "${SCREEN_HEIGHT:-1440}" \
    +set tdm_player_wait_until_ready 0 \
    +set com_showFPS 0 \
    "$@"
