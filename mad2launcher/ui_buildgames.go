package main

import (
	"path/filepath"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

// buildGameCard renders one target's status (ROM/ISO present, in either the
// shared roms/ drop folder or the per-project location? already built?)
// plus a "Build" button that runs RunBuildGames in the background,
// disabling itself while the build is in flight. finalAssetPath is the
// per-project location RunBuildGames ultimately builds against; romName is
// the file mad2buildgames will symlink from gamesRoot/roms/ into that
// location if it's not already there (see romAvailable/ensureRomLinked).
func buildGameCard(state *AppState, title, requiredAssetLabel, gamesRoot, romName, finalAssetPath, builtPath, target string) fyne.CanvasObject {
	statusLabel := widget.NewLabel("")
	statusLabel.Wrapping = fyne.TextWrapWord
	refreshStatus := func() {
		switch {
		case !romAvailable(gamesRoot, romName, finalAssetPath):
			statusLabel.SetText("Missing " + requiredAssetLabel + " -- drop " + romName + " in " +
				filepath.Join(gamesRoot, "roms") + " (or place it directly at " + finalAssetPath + ")")
		case fileExists(builtPath):
			statusLabel.SetText("Built: " + builtPath)
		default:
			statusLabel.SetText("Not built yet.")
		}
	}
	refreshStatus()

	buildButton := widget.NewButton("Build", nil)
	if !romAvailable(gamesRoot, romName, finalAssetPath) {
		buildButton.Disable()
	}
	buildButton.OnTapped = func() {
		buildButton.Disable()
		statusLabel.SetText("Building... (see buildgames.log)")
		repoRoot := filepath.Dir(state.SelectedInstall().Dir)
		go func() {
			err := RunBuildGames(repoRoot, target)
			fyne.Do(func() {
				refreshStatus()
				if !romAvailable(gamesRoot, romName, finalAssetPath) {
					buildButton.Disable()
				} else {
					buildButton.Enable()
				}
				if err != nil {
					dialog.ShowError(err, state.Window)
				} else {
					dialog.ShowInformation("Build finished", title+" built successfully.", state.Window)
				}
			})
		}()
	}

	return widget.NewCard(title, "", container.NewVBox(statusLabel, buildButton))
}

// buildBuildGamesPage is the "Build Games" category: compiles the two
// companion-process chaos-effect games (SM64, Jak & Daxter) from a
// user-supplied ROM/ISO via the prebuilt mad2buildgames tool (see
// buildgames.go/ResolveMad2BuildGames) -- this is the one place in the
// launcher that's *supposed* to trigger a real build, since it's compiling
// a game from copyrighted assets the user provides, not this repo's own
// tooling (see build_native.go's file header). Mario Legacy is
// deliberately not included here -- see CLAUDE.md/deploy-plan.md.
func buildBuildGamesPage(state *AppState) fyne.CanvasObject {
	header := widget.NewLabelWithStyle("Build Games", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})

	inst := state.SelectedInstall()
	if inst == nil {
		return container.NewVBox(header, widget.NewLabel("No install selected -- pick one under Game Launch first."))
	}
	repoRoot := filepath.Dir(inst.Dir)
	gamesRoot := GamesRoot(repoRoot)

	intro := widget.NewLabel(
		"SM64Challenge and JakChallenge (mad2companionmod.dll's chaos effects) need these built " +
			"from your own legally-owned ROM/ISO first. Drop baserom.us.z64 and jak1.iso in " +
			filepath.Join(gamesRoot, "roms") + " (one shared folder for both games), then build. " +
			"This can take a while (especially Jak1's first build) -- progress streams to buildgames.log " +
			"in " + gamesRoot + ".")
	intro.Wrapping = fyne.TextWrapWord

	sm64Card := buildGameCard(state, "Super Mario 64 (Linux)", "ROM",
		gamesRoot, "baserom.us.z64", sm64RomPath(gamesRoot), sm64LinuxBinPath(gamesRoot), "sm64-linux")
	sm64WinCard := buildGameCard(state, "Super Mario 64 (Windows, best-effort cross-build)", "ROM",
		gamesRoot, "baserom.us.z64", sm64RomPath(gamesRoot), sm64WindowsBinPath(gamesRoot), "sm64-windows")
	jak1Card := buildGameCard(state, "Jak & Daxter (Linux)", "ISO",
		gamesRoot, "jak1.iso", jak1IsoPath(gamesRoot), jak1LinuxBinPath(gamesRoot), "jak1-linux")

	if _, err := ResolveMad2BuildGames(repoRoot); err != nil {
		warn := widget.NewLabel("mad2buildgames " + err.Error() + "\n\nRun `just build-buildgames` first.")
		warn.Wrapping = fyne.TextWrapWord
		return container.NewScroll(container.NewVBox(header, intro, warn))
	}

	body := container.NewVBox(header, intro, sm64Card, sm64WinCard, jak1Card)
	return container.NewScroll(body)
}
