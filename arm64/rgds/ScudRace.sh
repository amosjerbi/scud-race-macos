#!/bin/bash
# Scud Race for ROCKNIX / PortMaster (RG DS and other arm64 handhelds): the statically recompiled native arm64 build.
# Layout: ports/ScudRace.sh (this file), ports/scudrace/scudrace-arm64, ports/scudrace/game/{ui,Music}, ports/scudrace/roms/scud.zip

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if [ -d /opt/system/Tools/PortMaster ]; then
  controlfolder=/opt/system/Tools/PortMaster
elif [ -d /opt/tools/PortMaster ]; then
  controlfolder=/opt/tools/PortMaster
elif [ -d "$XDG_DATA_HOME/PortMaster" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder=/roms/ports/PortMaster
fi

source "$controlfolder/control.txt"
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/scudrace"
cd "$GAMEDIR" || exit 1
exec > >(tee "$GAMEDIR/log.txt") 2>&1

if [ ! -d "$GAMEDIR/game/ui" ] || [ ! -f "$GAMEDIR/roms/scud.zip" ]; then
  pm_message "Put ui/ and Music/ in ports/scudrace/game and your MAME scud.zip in ports/scudrace/roms."
  exit 1
fi

# GPU renderer (the game's Direct3D 11 renderer on GLES 3). Bilinear instead of trilinear texturing is cheaper on
# small Mali GPUs; SCUD_HOST_GL_SCALE=0.75 draws the picture smaller and scales it up; SCUD_HOST_GL=0 = software renderer.
export SCUD_HOST_NOGUI=1 SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-alsa} SCUD_HOST_GL_TEXQ=${SCUD_HOST_GL_TEXQ:-1}
"$GAMEDIR/scudrace-arm64"
pm_finish
