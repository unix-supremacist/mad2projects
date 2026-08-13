cmake_minimum_required(VERSION 3.16)

# NES chaos effect (mad2/mods/mad2nesmod.dll) -- directly dlopens a
# libretro core (fceumm, vendored as a prebuilt 32-bit Windows DLL, see
# extern/.libretro-cores/ and `just fetch-libretro-fceumm`) inside the
# Mad2 process and drives it from this mod's own D3D9 EndScene hook,
# rather than spawning a separate emulator process the way
# mad2companionmod's SM64Challenge/JakChallenge/MarioLegacyChallenge do.
# See src/nesmod.cpp for the full picture. winmm is linked for the
# best-effort waveOut audio playback; opengl32 for N64Sm64Challenge's
# hardware-rendering path (a hidden-window WGL context GLideN64 renders
# into, read back via glReadPixels -- see EnsureN64GlContext/EnsureN64Fbo).
add_mad2_mod(mad2nesmod
    SOURCES src/nesmod.cpp
    LIBS winmm opengl32
)
