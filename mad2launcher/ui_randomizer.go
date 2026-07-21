package main

import (
	"path/filepath"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

// buildRandomizerPage is the "Mad2 Randomizer" category: edit
// mad2rando5's flags (its "config", since it's a one-shot CLI generator
// with no config file of its own -- see RandomizerSettings) and run it,
// writing level_redirects.txt straight into the selected install so
// mad2levelredirectmod.dll picks it up on next launch.
func buildRandomizerPage(state *AppState) fyne.CanvasObject {
	header := widget.NewLabelWithStyle("Mad2 Randomizer", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})

	inst := state.SelectedInstall()
	if inst == nil {
		return container.NewVBox(header, widget.NewLabel("No install selected -- pick one under Game Launch first."))
	}
	instCopy := *inst
	repoRoot := filepath.Dir(instCopy.Dir)

	if _, err := ResolveMad2Rando5(repoRoot); err != nil {
		return container.NewVBox(header,
			widget.NewLabel("mad2rando5 "+err.Error()+"\n\nRun `just build-mad2rando5`, or install the mad2rando package via the Mod Installer."))
	}

	cfg := state.Settings.RandomizerFor(instCopy)

	persist := func() {
		state.Settings.SetRandomizer(instCopy, cfg)
		if err := state.Settings.Save(); err != nil {
			dialog.ShowError(err, state.Window)
		}
	}

	seedEntry := widget.NewEntry()
	seedEntry.SetText(cfg.Seed)
	seedEntry.OnChanged = func(v string) { cfg.Seed = v; persist() }

	coinsEntry := widget.NewEntry()
	coinsEntry.SetText(cfg.CoinsNeeded)
	coinsEntry.OnChanged = func(v string) { cfg.CoinsNeeded = v; persist() }

	maxCoinsCheck := widget.NewCheck("", nil)
	maxCoinsCheck.SetChecked(cfg.MaxCoins)
	maxCoinsCheck.OnChanged = func(v bool) { cfg.MaxCoins = v; persist() }

	separateCheck := widget.NewCheck("", nil)
	separateCheck.SetChecked(cfg.SeparatePools)
	separateCheck.OnChanged = func(v bool) { cfg.SeparatePools = v; persist() }

	monkeyCheck := widget.NewCheck("", nil)
	monkeyCheck.SetChecked(cfg.MonkeyMatch)
	monkeyCheck.OnChanged = func(v bool) { cfg.MonkeyMatch = v; persist() }

	coinMatchCheck := widget.NewCheck("", nil)
	coinMatchCheck.SetChecked(cfg.CoinMatch)
	coinMatchCheck.OnChanged = func(v bool) { cfg.CoinMatch = v; persist() }

	lockedEntry := widget.NewEntry()
	lockedEntry.SetText(cfg.LockedLevels)
	lockedEntry.OnChanged = func(v string) { cfg.LockedLevels = v; persist() }

	form := widget.NewForm(
		widget.NewFormItem("Seed (0 = random each run)", seedEntry),
		widget.NewFormItem("Coins needed (0 = no minimum)", coinsEntry),
		widget.NewFormItem("Guarantee 100%-completable (overrides coins needed)", maxCoinsCheck),
		widget.NewFormItem("Keep minigame/story pools separate", separateCheck),
		widget.NewFormItem("Require matching monkey counts", monkeyCheck),
		widget.NewFormItem("Require matching coin counts", coinMatchCheck),
		widget.NewFormItem("Locked levels (comma-separated)", lockedEntry),
	)

	// Deliberately no output display here -- see changes.md: what got
	// randomized is a spoiler, so the full mapping table only ever goes to
	// mad2rando5.log (in the install dir, alongside level_redirects.txt),
	// never to this UI. RunRandomizer itself writes that log.
	runButton := widget.NewButton("Run Randomizer", func() {
		if err := RunRandomizer(repoRoot, cfg, instCopy.Dir); err != nil {
			dialog.ShowError(err, state.Window)
			return
		}
		dialog.ShowInformation("Done", "Wrote "+instCopy.Dir+"/level_redirects.txt", state.Window)
	})
	runButton.Importance = widget.HighImportance

	body := container.NewVBox(
		header,
		widget.NewLabel("Writes level_redirects.txt into: "+instCopy.Dir),
		form,
		runButton,
	)
	return container.NewScroll(body)
}
