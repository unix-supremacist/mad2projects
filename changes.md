in game launch change video quality 0/1/2 to Low/Medium/High
in cheats remove the cheat descriptions and make each single line, also remove the (NUM) from the selection boxes, and the detected platform
only log the output for mad2randomizer, dont have a thing in the fyneui for it so people arent spoiled
removing building mad2music from the mad2launcher, it should be shipped in binary form, but add music downloading, also fixed the blank in game music description because it makes the ui super wide and broken, the new lines arent working
in saveloader why is the comment describing how mad2launcher reads the config in mad2launcher, i feel like thats just unnecessary to have as a comment at all
make the links in timer clickable
why are the randomization chances/weight duplicated to chaosmode? having double the config for that seems insane, each mods config section should handle their weights
inputs effect is weird, why is it invert left stick only by default and it seems to imply that it cant pick and anxis to invert it inverts all that are selected, it should randomly pick 1-2 to invert, and right stick should be enabled by default
the reward and effect sections are duplicated in twitch controls ui
the sm64mod should be renamed now that it contains jak, and sm64 and jak should be 1 config category
cheat apply comments are very weird, we dont need all this info, also we can take the clean names from the other cheats menu
jaK1 fails to render in gamescope
sm64 fails to read controller inputs
we need to implement a system in music mods display to properly name downloaded songs like openbsds songXX songs
make sure the music downloader is not trying to redownload music
make all input modifiers also do equivalent keyboard input modifications
if i close gamescope instead of it actually properly killing everything it freezes up and music still plays, and if i exit the game it doesn't restart it
mario legacy and gordon and daxter(if they fixed controller) things
remove any unnecessary just recipes for things that can just be done through mad2launcher
remove any tools building any other tools, the only thing that should not be built by just is sm64 and jak
