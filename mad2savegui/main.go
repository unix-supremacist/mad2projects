// Package main implements a Fyne UI desktop save game editor for Madagascar 2.
// Supports both PC (little-endian) and Wii (big-endian) save files with
// cross-platform conversion.
package main

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"io"
	"path/filepath"
	"strconv"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/storage"
	"fyne.io/fyne/v2/widget"
)

const (
	SaveFileSize = 13568
	PCGapOffset  = 0x120
	PCGapSize    = 8
)

type Cheat struct {
	Name     string
	PCOffset int // Offset in PC save file
	Key      string
	Desc     string
}

// PC offsets. Wii offsets are PCOffset - PCGapSize for offsets >= PCGapOffset+PCGapSize.
//
// Confirmed this session by diffing two real PC save files (same autosave
// point, one with every cheat toggled via mad2cheatapply/mad2climbprobe) --
// the previous table here (and mad2launcher/saveeditor.go's identical
// copy) was off by one slot for most of the block, claimed a phantom
// "Buddy Pointer" at what's actually an unused/reserved slot (0x13c), and
// was missing "Invis New York" entirely. See mad2launcher/saveeditor.go's
// own comment for the full derivation and cross-check against
// mad2cheatapply.cpp's live-memory offsets.
var cheatsList = []Cheat{
	{Name: "Super Mangos", PCOffset: 0x128, Key: "superMangos", Desc: "Increases standard health limits"},
	{Name: "Head of the Game", PCOffset: 0x12c, Key: "headOfTheGame", Desc: "Enables bobble head scaling"},
	{Name: "Infinite Sprint", PCOffset: 0x130, Key: "infiniteSprint", Desc: "Sprint without depleting energy"},
	{Name: "Penguin Projectile", PCOffset: 0x134, Key: "penguinProjectile", Desc: "Allows throwing penguin projectiles"},
	{Name: "Unlock Levels", PCOffset: 0x138, Key: "unlockLevels", Desc: "Unlocks stage select levels"},
	// 0x13c is an unused/reserved slot -- see mad2launcher/saveeditor.go.
	{Name: "Buddy Pointer", PCOffset: 0x140, Key: "buddyPointer", Desc: "Shows compass/arrow helpers"},
	{Name: "Debug Mode", PCOffset: 0x144, Key: "debugMode", Desc: "Developer debug logs/mode"},
	{Name: "Fast Mango", PCOffset: 0x148, Key: "fastMango", Desc: "Increases gameplay speed"},
	{Name: "Giant Frogs", PCOffset: 0x14c, Key: "giantFrogs", Desc: "Scales frog targets larger"},
	{Name: "Super Butt Bounce", PCOffset: 0x150, Key: "superButtBounce", Desc: "Improves ground pound velocity"},
	{Name: "Pepper at Will", PCOffset: 0x154, Key: "pepperAtWill", Desc: "Marty gets infinite pepper sprints"},
	{Name: "Swim Backwards", PCOffset: 0x158, Key: "swimBackwards", Desc: "Reverses swimming speed"},
	{Name: "Invulnerable Alex", PCOffset: 0x15c, Key: "invulnAlex", Desc: "Invincibility for Alex"},
	{Name: "Invulnerable Gloria", PCOffset: 0x160, Key: "invulnGloria", Desc: "Invincibility for Gloria"},
	{Name: "Invulnerable Marty", PCOffset: 0x164, Key: "invulnMarty", Desc: "Invincibility for Marty"},
	{Name: "Invis New York", PCOffset: 0x168, Key: "invisNewYork", Desc: "Turns character invisible"},
	{Name: "Golf Bonus Hole", PCOffset: 0x28c, Key: "golfHole", Desc: "Easier golf physics"},
	{Name: "Stop Ball", PCOffset: 0x2a0, Key: "ballStop", Desc: "Allows stopping ball instantly"},
}

// Story-progress ability flags -- climb (unlocked by completing Rites of
// Passage) and monkey collectibles (unlocked by leaving Fix the Plane).
// Found by hooking the game's own save-file writes across a real
// playthrough and diffing consecutive writes, not by design like
// cheatsList's dedicated per-cheat uint32 slots -- see
// mad2launcher/saveeditor.go's identical fields for the full derivation.
// Both live below PCGapOffset (0x120), so unlike cheatsList they need no
// PC/Wii offset shift.
const climbAbilityOffset = 0x70
const monkeyCollectibleUint32Offset = 0x18

var monkeyCollectibleByteFields = map[int]byte{
	0x30: 1, 0x48: 1, 0x58: 1, 0x68: 2, 0x80: 1, 0xA8: 1, 0x3A0: 1, 0x3A4: 1,
}

// Coin economy -- found the same way (save-write hooking, cross-checked
// live via direct memory pokes and reloading the save to confirm each
// one). There is no single persistent "current coin balance" field: the
// shop total is computed live as (sum of every level's own coin count) -
// amountSpentOffset, and only the two operands are ever actually
// persisted. waterholeCoinsOffset is Waterhole's own per-level coin count
// specifically -- other levels almost certainly have their own slots
// elsewhere in the save (not located this session).
const amountSpentOffset = 0x0
const waterholeCoinsOffset = 0x178

func cheatOffset(c Cheat, isWii bool) int {
	if isWii && c.PCOffset >= PCGapOffset+PCGapSize {
		return c.PCOffset - PCGapSize
	}
	return c.PCOffset
}

func detectWii(data []byte) bool {
	if len(data) < 0x14 {
		return false
	}
	verBE := binary.BigEndian.Uint32(data[0x10:0x14])
	verLE := binary.LittleEndian.Uint32(data[0x10:0x14])
	return verBE == 17 && verLE != 17
}

func readU32(data []byte, off int, isWii bool) uint32 {
	if isWii {
		return binary.BigEndian.Uint32(data[off : off+4])
	}
	return binary.LittleEndian.Uint32(data[off : off+4])
}

func writeU32(data []byte, off int, val uint32, isWii bool) {
	if isWii {
		binary.BigEndian.PutUint32(data[off:off+4], val)
	} else {
		binary.LittleEndian.PutUint32(data[off:off+4], val)
	}
}

func calculateChecksum(data []byte) uint32 {
	return crc32.ChecksumIEEE(data[:len(data)-4]) ^ 0xFFFFFFFF
}

func swapEndianUint32(data []byte) {
	for i := 0; i+3 < len(data); i += 4 {
		data[i], data[i+3] = data[i+3], data[i]
		data[i+1], data[i+2] = data[i+2], data[i+1]
	}
}

// applyEdits writes the current UI values into the data buffer using native byte order.
func applyEdits(data []byte, isWii bool, inputProgress, inputPlaytime, inputAmountSpent, inputWaterholeCoins *widget.Entry, cheatSelectors map[string]*widget.Select, climbCheck, monkeysCheck *widget.Check) error {
	percentVal, err := strconv.ParseFloat(inputProgress.Text, 64)
	if err != nil {
		return fmt.Errorf("invalid progress value: %w", err)
	}
	writeU32(data, 12, uint32(percentVal*100.0), isWii)

	playtimeVal, err := strconv.ParseUint(inputPlaytime.Text, 10, 32)
	if err != nil {
		return fmt.Errorf("invalid playtime value: %w", err)
	}
	writeU32(data, 4, uint32(playtimeVal), isWii)

	amountSpentVal, err := strconv.ParseUint(inputAmountSpent.Text, 10, 32)
	if err != nil {
		return fmt.Errorf("invalid amount spent value: %w", err)
	}
	writeU32(data, amountSpentOffset, uint32(amountSpentVal), isWii)

	waterholeCoinsVal, err := strconv.ParseUint(inputWaterholeCoins.Text, 10, 32)
	if err != nil {
		return fmt.Errorf("invalid Waterhole coins value: %w", err)
	}
	writeU32(data, waterholeCoinsOffset, uint32(waterholeCoinsVal), isWii)

	if climbCheck.Checked {
		data[climbAbilityOffset] = 1
	} else {
		data[climbAbilityOffset] = 0
	}

	if monkeysCheck.Checked {
		writeU32(data, monkeyCollectibleUint32Offset, 0x00000000, isWii)
		for off, val := range monkeyCollectibleByteFields {
			data[off] = val
		}
	} else {
		writeU32(data, monkeyCollectibleUint32Offset, 0xFFFFFFFF, isWii)
		for off := range monkeyCollectibleByteFields {
			data[off] = 0
		}
	}

	for _, c := range cheatsList {
		sel := cheatSelectors[c.Key]
		var val uint32
		switch sel.Selected {
		case "Lock (0)":
			val = 0
		case "Unlock (1)":
			val = 1
		case "Enable (2)":
			val = 2
		}
		off := cheatOffset(c, isWii)
		writeU32(data, off, val, isWii)
	}

	crc := calculateChecksum(data)
	writeU32(data, len(data)-4, crc, isWii)
	return nil
}

// convertToPC takes any save (PC or Wii) with edits applied and returns a PC-format buffer.
func convertToPC(data []byte, isWii bool) []byte {
	if !isWii {
		// Already PC — just return a copy
		out := make([]byte, len(data))
		copy(out, data)
		return out
	}
	// Wii → PC: byte-swap BE→LE, shift cheats from [0x120-0x15F] to [0x128-0x167]
	pcData := make([]byte, len(data))
	copy(pcData, data)
	swapEndianUint32(pcData)

	// Shift cheats
	copy(pcData[0x128:0x168], pcData[0x120:0x160])
	binary.LittleEndian.PutUint32(pcData[0x120:0x124], 0)
	binary.LittleEndian.PutUint32(pcData[0x124:0x128], 0)

	// Shift golf cheats
	copy(pcData[0x28C:0x290], pcData[0x284:0x288])
	binary.LittleEndian.PutUint32(pcData[0x284:0x288], 0)
	copy(pcData[0x2A0:0x2A4], pcData[0x298:0x29C])
	binary.LittleEndian.PutUint32(pcData[0x298:0x29C], 0)

	// Recalculate checksum
	crc := calculateChecksum(pcData)
	binary.LittleEndian.PutUint32(pcData[len(pcData)-4:], crc)
	return pcData
}

// convertToWii takes any save (PC or Wii) with edits applied and returns a Wii-format buffer.
func convertToWii(data []byte, isWii bool) []byte {
	if isWii {
		// Already Wii — just return a copy
		out := make([]byte, len(data))
		copy(out, data)
		return out
	}
	// PC → Wii: byte-swap LE→BE, shift cheats from [0x128-0x167] to [0x120-0x15F]
	wiiData := make([]byte, len(data))
	copy(wiiData, data)
	swapEndianUint32(wiiData)

	// Shift cheats
	copy(wiiData[0x120:0x160], wiiData[0x128:0x168])
	binary.BigEndian.PutUint32(wiiData[0x160:0x164], 0)
	binary.BigEndian.PutUint32(wiiData[0x164:0x168], 0)

	// Shift golf cheats
	copy(wiiData[0x284:0x288], wiiData[0x28C:0x290])
	binary.BigEndian.PutUint32(wiiData[0x28C:0x290], 0)
	copy(wiiData[0x298:0x29C], wiiData[0x2A0:0x2A4])
	binary.BigEndian.PutUint32(wiiData[0x2A0:0x2A4], 0)

	// Recalculate checksum
	crc := calculateChecksum(wiiData)
	binary.BigEndian.PutUint32(wiiData[len(wiiData)-4:], crc)
	return wiiData
}

func main() {
	a := app.New()
	w := a.NewWindow("Madagascar 2 Save Editor")
	w.Resize(fyne.NewSize(750, 650))

	var currentFilePath string
	var currentData []byte
	var currentIsWii bool

	// GUI elements
	lblStatus := widget.NewLabel("No file loaded. Please open a Madagascar 2 save file.")
	lblStatus.Alignment = fyne.TextAlignCenter

	lblPlatform := widget.NewLabelWithStyle("", fyne.TextAlignCenter, fyne.TextStyle{Bold: true})

	btnOpen := widget.NewButton("Load Save File", nil)
	btnSave := widget.NewButton("Save (Native Format)", nil)
	btnSave.Disable()
	btnExportPC := widget.NewButton("Export as PC", nil)
	btnExportPC.Disable()
	btnExportWii := widget.NewButton("Export as Wii", nil)
	btnExportWii.Disable()

	inputProgress := widget.NewEntry()
	inputProgress.SetPlaceHolder("e.g. 62.83")
	inputPlaytime := widget.NewEntry()
	inputPlaytime.SetPlaceHolder("Playtime tick count")
	inputAmountSpent := widget.NewEntry()
	inputAmountSpent.SetPlaceHolder("Total coins spent in shops (persists across all levels)")
	inputWaterholeCoins := widget.NewEntry()
	inputWaterholeCoins.SetPlaceHolder("Waterhole's own coin count")

	climbCheck := widget.NewCheck("Climb Ability (unlocked by completing Rites of Passage)", nil)
	monkeysCheck := widget.NewCheck("Monkey Collectibles (unlocked by leaving Fix the Plane)", nil)

	formGeneral := widget.NewForm(
		widget.NewFormItem("Progress (%)", inputProgress),
		widget.NewFormItem("Playtime Ticks", inputPlaytime),
		widget.NewFormItem("Amount Spent", inputAmountSpent),
		widget.NewFormItem("Waterhole Coins", inputWaterholeCoins),
	)

	// Cheats scroll container layout
	cheatsContainer := container.NewVBox()
	scrollCheats := container.NewVScroll(cheatsContainer)
	scrollCheats.SetMinSize(fyne.NewSize(0, 280))

	cheatSelectors := make(map[string]*widget.Select)

	for _, c := range cheatsList {
		sel := widget.NewSelect([]string{"Lock (0)", "Unlock (1)", "Enable (2)"}, nil)
		cheatSelectors[c.Key] = sel

		labelBlock := container.NewVBox(
			widget.NewLabelWithStyle(c.Name, fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			widget.NewLabelWithStyle(c.Desc, fyne.TextAlignLeading, fyne.TextStyle{Italic: true}),
		)

		row := container.NewBorder(nil, nil, labelBlock, sel)
		cheatsContainer.Add(row)
		cheatsContainer.Add(widget.NewSeparator())
	}

	// Disable editing widgets until loaded
	inputProgress.Disable()
	inputPlaytime.Disable()
	inputAmountSpent.Disable()
	inputWaterholeCoins.Disable()
	climbCheck.Disable()
	monkeysCheck.Disable()
	cheatsContainer.Hide()

	// Load Save Logic
	btnOpen.OnTapped = func() {
		fileDialog := dialog.NewFileOpen(func(reader fyne.URIReadCloser, err error) {
			if err != nil {
				dialog.ShowError(err, w)
				return
			}
			if reader == nil {
				return
			}
			defer reader.Close()

			currentFilePath = reader.URI().Path()
			data, err := io.ReadAll(reader)
			if err != nil {
				dialog.ShowError(err, w)
				return
			}

			if len(data) < 0x14 {
				dialog.ShowError(fmt.Errorf("file too small (%d bytes); expected 13568", len(data)), w)
				return
			}

			currentData = data
			currentIsWii = detectWii(data)

			platform := "PC (Little-Endian)"
			if currentIsWii {
				platform = "Wii (Big-Endian)"
			}

			// Parse values using detected byte order
			progressVal := readU32(data, 12, currentIsWii)
			inputProgress.SetText(fmt.Sprintf("%.2f", float64(progressVal)/100.0))

			playtimeVal := readU32(data, 4, currentIsWii)
			inputPlaytime.SetText(strconv.FormatUint(uint64(playtimeVal), 10))

			amountSpentVal := readU32(data, amountSpentOffset, currentIsWii)
			inputAmountSpent.SetText(strconv.FormatUint(uint64(amountSpentVal), 10))

			waterholeCoinsVal := readU32(data, waterholeCoinsOffset, currentIsWii)
			inputWaterholeCoins.SetText(strconv.FormatUint(uint64(waterholeCoinsVal), 10))

			climbCheck.SetChecked(data[climbAbilityOffset] != 0)
			monkeysCheck.SetChecked(readU32(data, monkeyCollectibleUint32Offset, currentIsWii) == 0)

			// Parse cheats at platform-correct offsets
			for _, c := range cheatsList {
				off := cheatOffset(c, currentIsWii)
				cheatVal := readU32(data, off, currentIsWii)
				sel := cheatSelectors[c.Key]
				switch cheatVal {
				case 0:
					sel.SetSelected("Lock (0)")
				case 1:
					sel.SetSelected("Unlock (1)")
				case 2:
					sel.SetSelected("Enable (2)")
				default:
					sel.ClearSelected()
				}
			}

			// Enable interface
			lblStatus.SetText(fmt.Sprintf("Loaded: %s (%d bytes)", filepath.Base(currentFilePath), len(data)))
			lblPlatform.SetText(fmt.Sprintf("Detected Platform: %s", platform))
			inputProgress.Enable()
			inputPlaytime.Enable()
			inputAmountSpent.Enable()
			inputWaterholeCoins.Enable()
			climbCheck.Enable()
			monkeysCheck.Enable()
			cheatsContainer.Show()
			btnSave.Enable()
			btnExportPC.Enable()
			btnExportWii.Enable()

		}, w)

		fileDialog.SetFilter(storage.NewExtensionFileFilter([]string{"", "sav", "bin"}))
		fileDialog.Show()
	}

	// Helper: write edited data via save dialog
	doExport := func(exportData []byte, defaultName string) {
		fileDialog := dialog.NewFileSave(func(writer fyne.URIWriteCloser, err error) {
			if err != nil {
				dialog.ShowError(err, w)
				return
			}
			if writer == nil {
				return
			}
			defer writer.Close()

			if _, err = writer.Write(exportData); err != nil {
				dialog.ShowError(err, w)
				return
			}
			dialog.ShowInformation("Success", "Save game exported with valid checksum!", w)
		}, w)
		fileDialog.SetFileName(defaultName)
		fileDialog.Show()
	}

	// Save in native format
	btnSave.OnTapped = func() {
		if len(currentData) < 16 {
			return
		}
		if err := applyEdits(currentData, currentIsWii, inputProgress, inputPlaytime, inputAmountSpent, inputWaterholeCoins, cheatSelectors, climbCheck, monkeysCheck); err != nil {
			dialog.ShowError(err, w)
			return
		}
		doExport(currentData, filepath.Base(currentFilePath))
	}

	// Export as PC
	btnExportPC.OnTapped = func() {
		if len(currentData) < 16 {
			return
		}
		// Apply edits in native format first
		if err := applyEdits(currentData, currentIsWii, inputProgress, inputPlaytime, inputAmountSpent, inputWaterholeCoins, cheatSelectors, climbCheck, monkeysCheck); err != nil {
			dialog.ShowError(err, w)
			return
		}
		pcData := convertToPC(currentData, currentIsWii)
		baseName := filepath.Base(currentFilePath)
		doExport(pcData, baseName+"_pc")
	}

	// Export as Wii
	btnExportWii.OnTapped = func() {
		if len(currentData) < 16 {
			return
		}
		// Apply edits in native format first
		if err := applyEdits(currentData, currentIsWii, inputProgress, inputPlaytime, inputAmountSpent, inputWaterholeCoins, cheatSelectors, climbCheck, monkeysCheck); err != nil {
			dialog.ShowError(err, w)
			return
		}
		wiiData := convertToWii(currentData, currentIsWii)
		baseName := filepath.Base(currentFilePath)
		doExport(wiiData, baseName+"_wii")
	}

	// Layout
	title := widget.NewLabelWithStyle("Madagascar 2 Save Dashboard", fyne.TextAlignCenter, fyne.TextStyle{Bold: true})
	topSection := container.NewVBox(title, btnOpen, lblStatus, lblPlatform)

	exportRow := container.NewGridWithColumns(3, btnSave, btnExportPC, btnExportWii)
	bottomSection := container.NewVBox(exportRow)

	content := container.NewBorder(topSection, bottomSection, nil, nil, container.NewVBox(
		formGeneral,
		widget.NewLabelWithStyle("Story-Progress Ability Flags", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
		climbCheck,
		monkeysCheck,
		widget.NewLabelWithStyle("Cheat Options", fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
		scrollCheats,
	))

	w.SetContent(content)
	w.ShowAndRun()
}
