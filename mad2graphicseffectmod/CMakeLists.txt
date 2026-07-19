cmake_minimum_required(VERSION 3.16)

# Graphics chaos effects (deployed as mad2/mods/zz_mad2graphicseffectmod.dll
# -- the "zz_" prefix is deliberate, not cosmetic: mad2modloader's
# mod_loader.cpp now loads mad2/mods/*.dll in sorted-filename order, and
# this mod's effects pipeline destructively captures and redraws the whole
# backbuffer every frame, so it needs to be the LAST-loaded D3D9-hooking
# mod (see mod_loader.cpp's comment on why "last loaded" = "outermost
# hook" = "captures before any other mod's HUD has drawn") or it silently
# clobbers every other overlay mod's HUD. Confirmed by observation: before
# this rename, mods that happened to sort alphabetically after
# "mad2graphicseffectmod" (e.g. playercoords, twitchchat) had their HUDs
# captured and transformed along with the game's own frame.
#
# Registers effects (InvertColors, Rotate, Zoom, ...) with mad2effects,
# each implemented as a D3D9 EndScene post-process pass -- see
# src/graphicseffect.cpp for the full effect list and technique.
add_mad2_mod(mad2graphicseffectmod
    SOURCES src/graphicseffect.cpp
    OUTPUT_NAME zz_mad2graphicseffectmod
)
