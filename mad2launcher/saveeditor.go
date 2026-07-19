package main

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"regexp"
	"sort"
)

// Save-file format ported from ../mad2assetextractor/cmd/mad2save_gui and
// docs/SAVES_FORMAT.md: a fixed-size 13568-byte struct, PC (little-endian)
// or Wii (big-endian) with a shifted layout, terminated by a CRC32 footer.
// Only the cheat toggles are exposed here -- per the brief, everything else
// mad2save_gui can edit (progress, playtime, PC/Wii conversion) is out of
// scope for now.
const saveFileSize = 13568

// Cheat is one cheat toggle: a uint32 at PCOffset (native byte order),
// valued 0 (Lock) / 1 (Unlock) / 2 (Enable) exactly as mad2save_gui defines
// it -- these offsets and names are copied verbatim from there.
type Cheat struct {
	Name     string
	PCOffset int
	Key      string
	Desc     string
}

var cheatsList = []Cheat{
	{Name: "Infinite Sprint", PCOffset: 0x128, Key: "infiniteSprint", Desc: "Sprint without depleting energy"},
	{Name: "Super Mangos", PCOffset: 0x12c, Key: "superMangos", Desc: "Increases standard health limits"},
	{Name: "Head of the Game", PCOffset: 0x130, Key: "headOfTheGame", Desc: "Enables bobble head scaling"},
	{Name: "Penguin Projectile", PCOffset: 0x134, Key: "penguinProjectile", Desc: "Allows throwing penguin projectiles"},
	{Name: "Unlock Levels", PCOffset: 0x138, Key: "unlockLevels", Desc: "Unlocks stage select levels"},
	{Name: "Buddy Pointer", PCOffset: 0x13c, Key: "buddyPointer", Desc: "Shows compass/arrow helpers"},
	{Name: "Debug Mode", PCOffset: 0x140, Key: "debugMode", Desc: "Developer debug logs/mode"},
	{Name: "Fast Mango", PCOffset: 0x144, Key: "fastMango", Desc: "Increases gameplay speed"},
	{Name: "Giant Frogs", PCOffset: 0x148, Key: "giantFrogs", Desc: "Scales frog targets larger"},
	{Name: "Super Butt Bounce", PCOffset: 0x14c, Key: "superButtBounce", Desc: "Improves ground pound velocity"},
	{Name: "Pepper at Will", PCOffset: 0x150, Key: "pepperAtWill", Desc: "Marty gets infinite pepper sprints"},
	{Name: "Swim Backwards", PCOffset: 0x154, Key: "swimBackwards", Desc: "Reverses swimming speed"},
	{Name: "Invulnerable Alex", PCOffset: 0x158, Key: "invulnAlex", Desc: "Invincibility for Alex"},
	{Name: "Invulnerable Gloria", PCOffset: 0x15c, Key: "invulnGloria", Desc: "Invincibility for Gloria"},
	{Name: "Invulnerable Marty", PCOffset: 0x160, Key: "invulnMarty", Desc: "Invincibility for Marty"},
	{Name: "Invis New York", PCOffset: 0x164, Key: "invisNewYork", Desc: "Turns character invisible"},
	{Name: "Golf Hole in One", PCOffset: 0x28c, Key: "golfHole", Desc: "Easier golf physics"},
	{Name: "Stop Ball", PCOffset: 0x2a0, Key: "ballStop", Desc: "Allows stopping ball instantly"},
}

// PC has 8 extra zero bytes at 0x120 that Wii does not -- everything from
// there on is shifted +8 on PC relative to Wii (see mad2save_gui's own
// cheatOffset/convertToPC/convertToWii for the derivation).
const (
	pcGapOffset = 0x120
	pcGapSize   = 8
)

func cheatOffset(c Cheat, isWii bool) int {
	if isWii && c.PCOffset >= pcGapOffset+pcGapSize {
		return c.PCOffset - pcGapSize
	}
	return c.PCOffset
}

func detectWiiSave(data []byte) bool {
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

func saveChecksum(data []byte) uint32 {
	return crc32.ChecksumIEEE(data[:len(data)-4]) ^ 0xFFFFFFFF
}

// saveSlotRe matches this game's save-slot filenames (save_game0,
// save_game1, ...) -- see docs/SAVES_FORMAT.md -- while excluding the
// sibling Config.xml/registry.cfg files that live in the same directory.
var saveSlotRe = regexp.MustCompile(`^save_game\d+$`)

// ListSaveSlots returns every save_game<N> file in saveDir, sorted.
func ListSaveSlots(saveDir string) []string {
	entries, err := os.ReadDir(saveDir)
	if err != nil {
		return nil
	}
	var slots []string
	for _, e := range entries {
		if !e.IsDir() && saveSlotRe.MatchString(e.Name()) {
			slots = append(slots, e.Name())
		}
	}
	sort.Strings(slots)
	return slots
}

// ReadCheats reads every cheatsList value from the save file at path,
// keyed by Cheat.Key, along with whether it was detected as a Wii
// (big-endian) save.
func ReadCheats(path string) (map[string]uint32, bool, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, false, err
	}
	if len(data) < saveFileSize {
		return nil, false, fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)
	values := map[string]uint32{}
	for _, c := range cheatsList {
		values[c.Key] = readU32(data, cheatOffset(c, isWii), isWii)
	}
	return values, isWii, nil
}

// WriteCheat sets a single cheat's value in the save file at path and
// recalculates/rewrites the CRC32 footer, matching mad2save_gui's
// applyEdits + native-format save behavior. Re-reads and re-detects
// platform on every call rather than caching, since this is meant to be
// called straight from a UI control's autosave, not in a hot loop.
func WriteCheat(path string, key string, value uint32) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if len(data) < saveFileSize {
		return fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)

	var target *Cheat
	for i := range cheatsList {
		if cheatsList[i].Key == key {
			target = &cheatsList[i]
			break
		}
	}
	if target == nil {
		return fmt.Errorf("unknown cheat key %q", key)
	}

	writeU32(data, cheatOffset(*target, isWii), value, isWii)
	writeU32(data, len(data)-4, saveChecksum(data), isWii)

	return os.WriteFile(path, data, 0644)
}
