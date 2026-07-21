package main

import (
	"path/filepath"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/widget"
)

// AppState holds everything the UI pages need to read/rebuild themselves --
// the discovered installs, the launcher's own persisted settings, the
// currently-selected install's parsed config.cfg, and the widgets the top
// bar owns so category/install changes can refresh them in place.
type AppState struct {
	App      fyne.App
	Window   fyne.Window
	Settings *LauncherSettings
	Installs []Install
	Selected int // index into Installs, -1 if none discovered
	Cfg      *ConfigFile

	CategorySelect *widget.Select
	Content        *fyne.Container
}

func (a *AppState) SelectedInstall() *Install {
	if a.Selected < 0 || a.Selected >= len(a.Installs) {
		return nil
	}
	return &a.Installs[a.Selected]
}

func (a *AppState) RefreshInstalls() {
	a.Installs = DiscoverInstalls(defaultScanRoots())
	if len(a.Settings.ExtraRoots) > 0 {
		seen := map[string]bool{}
		for _, inst := range a.Installs {
			seen[inst.Dir] = true
		}
		for _, inst := range DiscoverInstallsDeep(a.Settings.ExtraRoots) {
			if !seen[inst.Dir] {
				seen[inst.Dir] = true
				a.Installs = append(a.Installs, inst)
			}
		}
	}
	if a.Selected >= len(a.Installs) {
		a.Selected = -1
	}
	if a.Selected == -1 && len(a.Installs) > 0 {
		a.Selected = 0
	}
}

// LoadConfigForSelected re-parses the selected install's config.cfg -- the
// registry that mad2config.dll owns and every other mod reads its settings
// from (see CLAUDE.md's "Shared-API mods" section). Every section header
// in that file becomes a category in the launcher's dropdown.
func (a *AppState) LoadConfigForSelected() {
	a.Cfg = nil
	inst := a.SelectedInstall()
	if inst == nil {
		return
	}
	cfg, err := ParseConfigFile(filepath.Join(inst.Dir, "config.cfg"))
	if err == nil {
		a.Cfg = cfg
	}
}

func (a *AppState) CategoryNames() []string {
	names := []string{"Game Launch", "Cheats", "Mad2 Randomizer", "Music", "Build Games", "Mod Installer"}
	if a.Cfg != nil {
		// "Music" is a hardcoded category above (it owns seeding config.cfg's
		// own [Music] section -- see ui_music.go), so skip it here to avoid
		// listing it twice once that section actually exists on disk.
		for _, s := range a.Cfg.Sections {
			if s == "Music" {
				continue
			}
			names = append(names, s)
		}
	}
	return names
}

func (a *AppState) ShowCategory(name string) {
	var page fyne.CanvasObject
	switch name {
	case "", "Game Launch":
		page = buildGameLaunchPage(a)
	case "Cheats":
		page = buildCheatsPage(a)
	case "Mad2 Randomizer":
		page = buildRandomizerPage(a)
	case "Music":
		page = buildMusicPage(a)
	case "Build Games":
		page = buildBuildGamesPage(a)
	case "Mod Installer":
		page = buildModInstallerPage(a)
	default:
		page = buildConfigCategoryPage(a, name)
	}
	a.Content.Objects = []fyne.CanvasObject{page}
	a.Content.Refresh()
}

// RebuildAll re-derives the category list (install/config.cfg may have
// changed) and jumps back to the Game Launch page -- the one category
// that's always valid regardless of which install is selected.
func (a *AppState) RebuildAll() {
	a.CategorySelect.Options = a.CategoryNames()
	a.CategorySelect.Refresh()
	a.CategorySelect.SetSelected("Game Launch")
}

func main() {
	fyneApp := app.New()
	window := fyneApp.NewWindow("Mad2 Launcher")

	state := &AppState{App: fyneApp, Window: window, Settings: LoadSettings(), Selected: -1}
	state.RefreshInstalls()
	state.LoadConfigForSelected()

	state.Content = container.NewStack(widget.NewLabel(""))
	state.CategorySelect = widget.NewSelect(state.CategoryNames(), func(name string) {
		state.ShowCategory(name)
	})

	topBar := container.NewBorder(nil, nil, widget.NewLabel("Category:"), nil, state.CategorySelect)
	root := container.NewBorder(topBar, nil, nil, nil, state.Content)

	window.SetContent(root)
	window.Resize(fyne.NewSize(860, 720))

	state.CategorySelect.SetSelected("Game Launch")

	window.ShowAndRun()
}
