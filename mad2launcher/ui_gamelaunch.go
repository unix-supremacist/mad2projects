package main

import (
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

const addFolderOption = "+ Add folder..."

// buildGameLaunchPage is the "Game Launch" category: pick a discovered
// install, tune how it's launched (gamescope/Proton on Linux, nothing extra
// on Windows), edit the game's own graphics settings (from the save
// directory's Config.xml) and start level (alchemy.xml). Every field
// autosaves itself the moment it changes -- there is no separate Save
// button, only Launch, which is an action rather than a save.
func buildGameLaunchPage(state *AppState) fyne.CanvasObject {
	installSelect := buildInstallSelect(state)

	inst := state.SelectedInstall()
	if inst == nil {
		return container.NewVBox(
			widget.NewLabelWithStyle("Game Launch", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			widget.NewLabel("No Mad2.exe / Mad2Demo.exe found under the current directory."),
			widget.NewLabel("Use the dropdown below to add a folder to search."),
			installSelect,
		)
	}
	instCopy := *inst
	settings := state.Settings.For(instCopy)
	alchemyPath := filepath.Join(instCopy.Dir, "alchemy.xml")

	persistSettings := func() {
		state.Settings.Set(instCopy, settings)
		if err := state.Settings.Save(); err != nil {
			dialog.ShowError(err, state.Window)
		}
	}

	launchCard := buildLaunchOptionsCard(&settings, persistSettings, alchemyPath)
	saveXmlCard := buildSaveGraphicsCard(state, instCopy)
	startLevelCard := buildStartLevelCard(&settings, persistSettings, alchemyPath)

	launchButton := widget.NewButton("Launch Game", func() {
		warnings, err := LaunchGame(instCopy, settings)
		if err != nil {
			dialog.ShowError(err, state.Window)
			return
		}
		if len(warnings) > 0 {
			dialog.ShowInformation("Launched with warnings", strings.Join(warnings, "\n\n"), state.Window)
		}
	})
	launchButton.Importance = widget.HighImportance

	body := container.NewVBox(
		widget.NewLabelWithStyle("Game Launch", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
		installSelect,
		launchCard,
		saveXmlCard,
		startLevelCard,
		launchButton,
	)
	return container.NewVScroll(body)
}

func buildInstallSelect(state *AppState) *widget.Select {
	var options []string
	for _, inst := range state.Installs {
		options = append(options, inst.DisplayName())
	}
	options = append(options, addFolderOption)

	sel := widget.NewSelect(options, nil)
	if inst := state.SelectedInstall(); inst != nil {
		sel.SetSelected(inst.DisplayName())
	}

	sel.OnChanged = func(name string) {
		if name == addFolderOption {
			dialog.ShowFolderOpen(func(uri fyne.ListableURI, err error) {
				if err != nil || uri == nil {
					if inst := state.SelectedInstall(); inst != nil {
						sel.SetSelected(inst.DisplayName())
					}
					return
				}
				path := uri.Path()
				state.Settings.ExtraRoots = append(state.Settings.ExtraRoots, path)
				_ = state.Settings.Save()
				state.RefreshInstalls()
				for i, inst := range state.Installs {
					if len(inst.Dir) >= len(path) && inst.Dir[:len(path)] == path {
						state.Selected = i
						break
					}
				}
				state.LoadConfigForSelected()
				state.RebuildAll()
			}, state.Window)
			return
		}
		for i, inst := range state.Installs {
			if inst.DisplayName() == name {
				state.Selected = i
				break
			}
		}
		state.LoadConfigForSelected()
		state.RebuildAll()
	}
	return sel
}

// buildLaunchOptionsCard shows the gamescope/Proton/wineprefix options on
// Linux (see CLAUDE.md's WINEDLLOVERRIDES note and the justfile's _play
// recipe -- this reproduces the same knobs ../mad2mod's launcher exposes:
// input/output resolution, scaling filter, FSR sharpness, FPS cap). On
// Windows none of this exists -- the exe just runs directly. Every field
// autosaves into settings (a launcher-owned setting) on change; the
// Presentation field additionally writes straight through to alchemy.xml,
// since that's the game's own file, not something this launcher owns.
func buildLaunchOptionsCard(settings *InstallSettings, persist func(), alchemyPath string) fyne.CanvasObject {
	if runtime.GOOS == "windows" {
		return widget.NewCard("Launch", "", widget.NewLabel(
			"Running on Windows: Mad2.exe launches directly -- no gamescope/Proton/wineprefix needed."))
	}

	inputEntry := widget.NewEntry()
	inputEntry.SetText(settings.InputRes)
	inputEntry.OnChanged = func(v string) { settings.InputRes = v; persist() }

	outputEntry := widget.NewEntry()
	outputEntry.SetText(settings.OutputRes)
	outputEntry.OnChanged = func(v string) { settings.OutputRes = v; persist() }

	scalingSelect := widget.NewSelect([]string{"Bilinear", "FSR", "NIS", "Nearest"}, nil)
	scalingSelect.SetSelected(settings.Scaling)
	scalingSelect.OnChanged = func(v string) { settings.Scaling = v; persist() }

	sharpnessEntry := widget.NewEntry()
	sharpnessEntry.SetText(settings.FsrSharpness)
	sharpnessEntry.OnChanged = func(v string) { settings.FsrSharpness = v; persist() }

	fpsEntry := widget.NewEntry()
	fpsEntry.SetText(settings.FpsCap)
	fpsEntry.OnChanged = func(v string) { settings.FpsCap = v; persist() }

	presentEntry := widget.NewEntry()
	presentEntry.SetText(settings.Presentation)
	presentEntry.OnChanged = func(v string) {
		settings.Presentation = v
		persist()
		_ = WriteAlchemyPresentationInterval(alchemyPath, v)
	}

	prefixEntry := widget.NewEntry()
	prefixEntry.SetText(settings.WinePrefix)
	prefixEntry.OnChanged = func(v string) { settings.WinePrefix = v; persist() }

	protonEntry := widget.NewEntry()
	protonEntry.SetText(settings.ProtonPath)
	protonEntry.OnChanged = func(v string) { settings.ProtonPath = v; persist() }

	gameIdEntry := widget.NewEntry()
	gameIdEntry.SetText(settings.GameID)
	gameIdEntry.OnChanged = func(v string) { settings.GameID = v; persist() }

	form := widget.NewForm(
		widget.NewFormItem("Input resolution", inputEntry),
		widget.NewFormItem("Output resolution", outputEntry),
		widget.NewFormItem("Scaling filter", scalingSelect),
		widget.NewFormItem("FSR sharpness", sharpnessEntry),
		widget.NewFormItem("FPS cap (0 = unlimited)", fpsEntry),
		widget.NewFormItem("Presentation interval", presentEntry),
		widget.NewFormItem("Wine prefix", prefixEntry),
		widget.NewFormItem("Proton path", protonEntry),
		widget.NewFormItem("GAMEID", gameIdEntry),
	)
	return widget.NewCard("Gamescope / Proton (Linux)", "", form)
}

// videoQualityLabels maps Config.xml's raw VideoQuality integer (0/1/2,
// exactly what the game itself reads -- the wire value is untouched) to a
// human label for the dropdown. Order matters: index == stored value.
var videoQualityLabels = []string{"Low", "Medium", "High"}

func videoQualityValueToLabel(v string) string {
	if i, err := strconv.Atoi(strings.TrimSpace(v)); err == nil && i >= 0 && i < len(videoQualityLabels) {
		return videoQualityLabels[i]
	}
	return v // unrecognized value -- show it verbatim rather than guessing
}

func videoQualityLabelToValue(label string) string {
	for i, l := range videoQualityLabels {
		if l == label {
			return strconv.Itoa(i)
		}
	}
	return label
}

// buildSaveGraphicsCard reads [SaveLoader] SaveDir from config.cfg (default
// "saves", relative to the install dir -- see mad2saveloader/src/save_redirect.cpp)
// and, if a Config.xml already exists there, exposes its graphics/resolution
// keys as auto-saving fields, writing straight back to that one key on change.
// VideoQuality is special-cased as a Low/Medium/High dropdown instead of a
// raw integer entry -- the stored value (0/1/2) is exactly what the game's
// own Config.xml expects, only the display changes.
func buildSaveGraphicsCard(state *AppState, inst Install) fyne.CanvasObject {
	xmlPath := filepath.Join(inst.Dir, ResolveSaveDir(state.Cfg), "Config.xml")

	values, _ := ReadSaveConfigXML(xmlPath)
	if values == nil {
		return widget.NewCard("Save Graphics Settings", xmlPath,
			widget.NewLabel("No Config.xml found yet in the save directory -- run the game once, then reopen this install."))
	}

	var items []*widget.FormItem
	for _, key := range SaveGraphicsKeys {
		v, ok := values[key]
		if !ok {
			continue
		}
		key := key

		var control fyne.CanvasObject
		if key == "VideoQuality" {
			sel := widget.NewSelect(videoQualityLabels, nil)
			sel.SetSelected(videoQualityValueToLabel(v))
			sel.OnChanged = func(label string) {
				if err := WriteSaveConfigXML(xmlPath, map[string]string{key: videoQualityLabelToValue(label)}); err != nil {
					dialog.ShowError(err, state.Window)
				}
			}
			control = sel
		} else {
			e := widget.NewEntry()
			e.SetText(v)
			e.OnChanged = func(newVal string) {
				if err := WriteSaveConfigXML(xmlPath, map[string]string{key: newVal}); err != nil {
					dialog.ShowError(err, state.Window)
				}
			}
			control = e
		}
		items = append(items, widget.NewFormItem(key, control))
	}
	form := widget.NewForm(items...)
	return widget.NewCard("Save Graphics Settings", xmlPath, form)
}

// buildStartLevelCard exposes alchemy.xml's Tfb startLevel attribute as a
// dropdown over every level name ../mad2mod's randomizer knows about,
// writing straight through to alchemy.xml (and caching the choice in
// settings) the moment it's changed.
func buildStartLevelCard(settings *InstallSettings, persist func(), alchemyPath string) fyne.CanvasObject {
	current := settings.StartLevel
	if alchemy, _ := ReadAlchemyXML(alchemyPath); alchemy != nil && alchemy.StartLevel != "" {
		current = alchemy.StartLevel
	}
	sel := widget.NewSelect(LevelNames, nil)
	sel.OnChanged = func(v string) {
		settings.StartLevel = v
		persist()
		_ = WriteAlchemyStartLevel(alchemyPath, v)
	}
	if current != "" {
		sel.SetSelected(current)
	} else {
		sel.SetSelected("title")
	}
	return widget.NewCard("Start Level", "written straight to alchemy.xml", sel)
}
