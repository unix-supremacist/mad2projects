package main

import (
	"path/filepath"

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
