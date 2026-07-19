cmake_minimum_required(VERSION 3.16)

# Game-state chaos effects (mad2/mods/mad2gameeffectsmod.dll). Registers
# three effects with mad2effects, ported from ../mad2mod's external overlay
# tool (its F4/F5/"I" keys and chaos-event director loop):
#   - ObjectShuffle: shuffles the positions of every Havok physics object
#     found on the heap (player excluded).
#   - MangoMutate: randomizes the player's mango count, gated to a handful
#     of story levels that actually display it.
#   - PlayerTeleport: teleports the player to a random previously-logged
#     coordinate (data/coordinates.yaml, deployed alongside the mod -- see
#     root CMakeLists.txt's add_deploy_target) for the current level.
# See src/gameeffects.cpp for the in-process port of ../mad2mod's
# process_vm_readv/writev-based memory access.
add_mad2_mod(mad2gameeffectsmod
    SOURCES src/gameeffects.cpp
)
