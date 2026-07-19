cmake_minimum_required(VERSION 3.16)

# Joystick-reversal chaos effect (mad2/mods/mad2inputeffectsmod.dll).
# Registers a "JoystickReversal" effect with mad2effects that inverts
# stick axes via mad2xinput's Mad2XInputOverride.invertLX/LY/RX/RY --
# mad2inputdisplay needs no changes, since it already reads pre-override
# raw state from mad2xinput. See src/inputeffects.cpp for the rest.
add_mad2_mod(mad2inputeffectsmod
    SOURCES src/inputeffects.cpp
)
