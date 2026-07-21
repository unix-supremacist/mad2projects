package main

import (
	"fmt"
	"path/filepath"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

// buildModInstallerPage is the "Mod Installer" category: lists every mod
// package discovered under ResolvePackagesRoot (see modinstaller.go) and
// copies whichever ones the user checks into the currently-selected
// install's own directory, merging into its mods/ folder the same way
// `just deploy-mad2` does today. Meant to eventually replace that dev-only
// deploy path as the end-user-facing install route -- see deploy-plan.md
// §3.1. Uses the same install selection every other page reads
// (state.SelectedInstall()) rather than a separate picker.
func buildModInstallerPage(state *AppState) fyne.CanvasObject {
	header := widget.NewLabelWithStyle("Mod Installer", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})

	inst := state.SelectedInstall()
	if inst == nil {
		return container.NewVBox(header, widget.NewLabel("No install selected -- pick one under Game Launch first."))
	}
	instCopy := *inst
	repoRoot := filepath.Dir(instCopy.Dir)

	statusLabel := widget.NewLabel("")
	statusLabel.Wrapping = fyne.TextWrapWord

	checks := map[string]*widget.Check{}
	listContainer := container.NewVBox()

	// refreshList re-runs discovery (the packages root or its contents may
	// have changed, e.g. after editing the override below) and rebuilds the
	// checkbox rows; checks is reset each time so installButton always reads
	// against the currently-displayed set.
	refreshList := func() {
		checks = map[string]*widget.Check{}
		root := ResolvePackagesRoot(repoRoot, state.Settings.PackagesDir)
		pkgs := DiscoverPackages(root)

		var rows []fyne.CanvasObject
		switch {
		case root == "":
			rows = append(rows, widget.NewLabel(
				"No packages directory found. Bundle one at packages/ next to this launcher's "+
					"executable, run the packaging scripts into <repoRoot>/dist/<platform>/, or set "+
					"a custom directory below."))
		case len(pkgs) == 0:
			rows = append(rows, widget.NewLabel("No packages found under "+root))
		default:
			rows = append(rows, widget.NewLabel("Packages found under "+root+":"))
			for _, pkg := range pkgs {
				chk := widget.NewCheck(pkg.Name, nil)
				checks[pkg.Name] = chk
				rows = append(rows, chk)
			}
		}

		listContainer.Objects = rows
		listContainer.Refresh()
	}
	refreshList()

	// Custom packages directory override, persisted immediately like every
	// other auto-saving control in this launcher (see ui_category.go's
	// autosave) -- blank falls back to auto-detection in ResolvePackagesRoot.
	packagesDirEntry := widget.NewEntry()
	packagesDirEntry.SetText(state.Settings.PackagesDir)
	packagesDirEntry.SetPlaceHolder("(auto-detect)")
	packagesDirEntry.OnChanged = func(v string) {
		state.Settings.PackagesDir = v
		_ = state.Settings.Save()
		refreshList()
	}
	browseButton := widget.NewButton("Browse...", func() {
		dialog.ShowFolderOpen(func(uri fyne.ListableURI, err error) {
			if err != nil || uri == nil {
				return
			}
			packagesDirEntry.SetText(uri.Path()) // triggers OnChanged above
		}, state.Window)
	})
	packagesDirCard := widget.NewCard("Packages directory (blank = auto-detect)", "",
		container.NewBorder(nil, nil, nil, browseButton, packagesDirEntry))

	installButton := widget.NewButton("Install Selected", nil)
	installButton.Importance = widget.HighImportance
	installButton.OnTapped = func() {
		root := ResolvePackagesRoot(repoRoot, state.Settings.PackagesDir)
		var selected []ModPackage
		for _, pkg := range DiscoverPackages(root) {
			if chk, ok := checks[pkg.Name]; ok && chk.Checked {
				selected = append(selected, pkg)
			}
		}
		if len(selected) == 0 {
			statusLabel.SetText("Nothing selected.")
			return
		}

		installButton.Disable()
		statusLabel.SetText("Installing...")
		go func() {
			results := InstallPackages(selected, instCopy.Dir)
			fyne.Do(func() {
				installButton.Enable()
				statusLabel.SetText(formatInstallResults(results))
			})
		}()
	}

	listCard := widget.NewCard("Available packages", "", listContainer)

	body := container.NewVBox(
		header,
		widget.NewLabel("Installing into: "+instCopy.DisplayName()+" ("+instCopy.Dir+")"),
		packagesDirCard,
		listCard,
		installButton,
		statusLabel,
	)
	return container.NewScroll(body)
}

// formatInstallResults renders one line per package (OK, or the error) plus
// a trailing summary count -- shown directly in statusLabel rather than a
// log file, since InstallPackage's work is fast synchronous file copying
// with nothing worth streaming incrementally.
func formatInstallResults(results []InstallResult) string {
	var b strings.Builder
	ok := 0
	for _, r := range results {
		if r.Err != nil {
			fmt.Fprintf(&b, "%s: FAILED (%v)\n", r.Package, r.Err)
			continue
		}
		ok++
		fmt.Fprintf(&b, "%s: OK\n", r.Package)
	}
	fmt.Fprintf(&b, "\n%d/%d package(s) installed.", ok, len(results))
	return b.String()
}
