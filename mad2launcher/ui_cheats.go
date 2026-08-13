package main

import (
	"fmt"
	"path/filepath"
	"strconv"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

var cheatSelectOptions = []string{"Lock", "Unlock", "Enable"}

func cheatValueToOption(v uint32) string {
	switch v {
	case 1:
		return "Unlock"
	case 2:
		return "Enable"
	default:
		return "Lock"
	}
}

func cheatOptionToValue(opt string) uint32 {
	switch opt {
	case "Unlock":
		return 1
	case "Enable":
		return 2
	default:
		return 0
	}
}

// numericFieldRow builds a labeled integer entry that reads the current
// value via read() and writes back via write() on every keystroke (not
// just Enter/submit -- a plain fyne Entry doesn't fire OnSubmitted on
// focus loss either, so requiring Enter made every edit here need an
// extra keypress) -- used for the coin-economy fields, which are plain
// uint32 values, not cheat-style enums or booleans. Parse errors from
// transient/incomplete input (e.g. the field momentarily empty while
// backspacing to type a new number) are swallowed rather than surfaced --
// only a genuine write failure pops the error dialog.
func numericFieldRow(state *AppState, label string, read func() (uint32, error), write func(uint32) error) fyne.CanvasObject {
	value, err := read()
	if err != nil {
		return widget.NewLabel("Error reading " + label + ": " + err.Error())
	}
	entry := widget.NewEntry()
	entry.SetText(strconv.FormatUint(uint64(value), 10))
	entry.OnChanged = func(text string) {
		v, err := strconv.ParseUint(text, 10, 32)
		if err != nil {
			return
		}
		if err := write(uint32(v)); err != nil {
			dialog.ShowError(err, state.Window)
		}
	}
	nameLabel := widget.NewLabelWithStyle(label, fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	return container.NewBorder(nil, nil, nameLabel, nil, entry)
}

// buildCheatsPage is the "Cheats" category: edit the cheat toggles baked
// into a save_game<N> file, ported from
// ../mad2assetextractor/cmd/mad2save_gui's cheatsList and read/write/CRC32
// logic (see saveeditor.go). Deliberately narrow in scope, per the brief --
// progress/playtime editing and PC<->Wii conversion, which that tool also
// does, are left alone here.
func buildCheatsPage(state *AppState) fyne.CanvasObject {
	header := widget.NewLabelWithStyle("Cheats (Save Editor)", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})

	inst := state.SelectedInstall()
	if inst == nil {
		return container.NewVBox(header, widget.NewLabel("No install selected -- pick one under Game Launch first."))
	}

	saveDir := filepath.Join(inst.Dir, ResolveSaveDir(state.Cfg))
	slots := ListSaveSlots(saveDir)
	if len(slots) == 0 {
		return container.NewVBox(header,
			widget.NewLabel("No save_game<N> files found in "+saveDir+" yet -- create an in-game save, then reopen this install."))
	}

	body := container.NewVBox()

	renderSlot := func(slotName string) {
		body.RemoveAll()
		savePath := filepath.Join(saveDir, slotName)

		// isWii is auto-detected purely to pick the correct read/write byte
		// offsets/order -- see saveeditor.go -- and no longer surfaced here.
		values, _, err := ReadCheats(savePath)
		if err != nil {
			body.Add(widget.NewLabel("Error reading " + slotName + ": " + err.Error()))
			body.Refresh()
			return
		}

		// Story-progress ability flags -- separate from cheatsList's
		// dedicated per-cheat uint32 slots (see saveeditor.go's own
		// comment on ReadClimbAbility/ReadMonkeyCollectible for why).
		climbUnlocked, err := ReadClimbAbility(savePath)
		if err != nil {
			body.Add(widget.NewLabel("Error reading climb ability: " + err.Error()))
		} else {
			climbCheck := widget.NewCheck("Climb Ability (unlocked by completing Rites of Passage)", func(checked bool) {
				if err := WriteClimbAbility(savePath, checked); err != nil {
					dialog.ShowError(err, state.Window)
				}
			})
			climbCheck.SetChecked(climbUnlocked)
			body.Add(climbCheck)
		}

		monkeysUnlocked, err := ReadMonkeyCollectible(savePath)
		if err != nil {
			body.Add(widget.NewLabel("Error reading monkey collectibles: " + err.Error()))
		} else {
			monkeysCheck := widget.NewCheck("Monkey Collectibles (unlocked by leaving Fix the Plane)", func(checked bool) {
				if err := WriteMonkeyCollectible(savePath, checked); err != nil {
					dialog.ShowError(err, state.Window)
				}
			})
			monkeysCheck.SetChecked(monkeysUnlocked)
			body.Add(monkeysCheck)
		}

		// Coin economy -- see saveeditor.go's own comment on
		// ReadAmountSpent/levelCoinOffsets for how these two combine
		// (shop total = sum of every level's own coin count - amount
		// spent) and why there's no single "current balance" field. One
		// row per LevelCoinLevels entry (includes "Unknown0x19C", a real,
		// summed-into-the-total slot that's never been observed to
		// belong to any specific named level -- see saveeditor.go).
		body.Add(numericFieldRow(state, "Amount Spent (shops, persists across all levels)",
			func() (uint32, error) { return ReadAmountSpent(savePath) },
			func(v uint32) error { return WriteAmountSpent(savePath, v) }))
		for _, lvl := range LevelCoinLevels {
			lvl := lvl
			label := lvl + " Coins"
			if max, ok := LevelCoinMax[lvl]; ok {
				label = fmt.Sprintf("%s Coins (max %d)", lvl, max)
			}
			body.Add(numericFieldRow(state, label,
				func() (uint32, error) { return ReadLevelCoins(savePath, lvl) },
				func(v uint32) error { return WriteLevelCoins(savePath, lvl, v) }))
		}

		body.Add(widget.NewSeparator())

		for _, c := range cheatsList {
			c := c
			sel := widget.NewSelect(cheatSelectOptions, nil)
			sel.SetSelected(cheatValueToOption(values[c.Key]))
			sel.OnChanged = func(opt string) {
				if err := WriteCheat(savePath, c.Key, cheatOptionToValue(opt)); err != nil {
					dialog.ShowError(err, state.Window)
				}
			}

			nameLabel := widget.NewLabelWithStyle(c.Name, fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
			body.Add(container.NewBorder(nil, nil, nameLabel, nil, sel))
		}
		body.Refresh()
	}

	slotSelect := widget.NewSelect(slots, renderSlot)
	slotSelect.SetSelected(slots[0])
	renderSlot(slots[0])

	top := container.NewVBox(header, widget.NewLabel("Save slot:"), slotSelect)
	scroll := container.NewScroll(body)
	return container.NewBorder(top, nil, nil, nil, scroll)
}
