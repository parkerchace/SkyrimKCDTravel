#!/usr/bin/env bash
# Build Wayfarer and deploy the DLL + INI to the game Data folder and the Vortex
# staging folder. Usage: bash redeploy.sh   (close Skyrim first — a running game
# locks the DLL).
set -u
cd "H:/coding/SkyrimKCDTravel"

echo "=== build ==="
cmake --build build/release --config Release 2>&1 \
  | grep -iE "error C[0-9]|: error|Wayfarer\.vcxproj ->" | head

SRCDLL="build/release/Release/Wayfarer.dll"
SRCINI="data/SKSE/Plugins/Wayfarer.ini"
GAME="H:/SteamLibrary/steamapps/common/Skyrim Special Edition/Data"
VORTEX="H:/Vortex Mods/skyrimse/Wayfarer"

echo "=== deploy ==="
# Copy the whole data/ tree so DLL, INI, ESP, compiled scripts, and MCM config
# all land together. Both the game Data folder and the Vortex staging folder.
deploy_tree() {
  local dst="$1"
  mkdir -p "$dst/SKSE/Plugins" "$dst/Scripts" "$dst/Source/Scripts" \
           "$dst/MCM/Config/Wayfarer" "$dst/MCM/Settings" 2>/dev/null
  cp -f "$SRCDLL" "$dst/SKSE/Plugins/Wayfarer.dll" || return 1
  cp -f "$SRCINI" "$dst/SKSE/Plugins/Wayfarer.ini"
  cp -f "data/Wayfarer.esp" "$dst/Wayfarer.esp" 2>/dev/null
  cp -f data/Scripts/*.pex "$dst/Scripts/" 2>/dev/null
  cp -f data/Source/Scripts/*.psc "$dst/Source/Scripts/" 2>/dev/null
  cp -f data/MCM/Config/Wayfarer/* "$dst/MCM/Config/Wayfarer/" 2>/dev/null
  cp -f data/MCM/Settings/* "$dst/MCM/Settings/" 2>/dev/null
  return 0
}
if deploy_tree "$GAME"; then
  deploy_tree "$VORTEX"
  echo "DEPLOYED $(ls -la "$GAME/SKSE/Plugins/Wayfarer.dll" | awk '{print $6,$7,$8}') + esp/scripts/mcm"
else
  echo "LOCKED — Skyrim is running; close it and rerun redeploy.sh"
fi
