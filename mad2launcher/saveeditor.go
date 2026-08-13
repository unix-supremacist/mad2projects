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

// Offsets confirmed this session by diffing two real PC save files (same
// autosave point, one with every mad2cheatapply-known cheat toggled via
// mad2climbprobe's F5 test hotkey) -- the previous table here was a guessed
// reassignment of the right offset *range* to the wrong cheat at each slot
// (off by one for most of the block, a phantom "Buddy Pointer" at 0x13c
// that's actually an unused/reserved slot, and "Invis New York" missing
// from the table entirely). Cross-checked independently against the live
// memory offsets mad2cheatapply.cpp's own kCheats table already uses
// (AlchemyCommonLib.dll's cheat struct): sorting those by offset reproduces
// the exact same cheat order seen here, including the doubled gap between
// Unlock Levels and Buddy Pointer that corresponds to the unused 0x13c slot.
var cheatsList = []Cheat{
	{Name: "Super Mangos", PCOffset: 0x128, Key: "superMangos", Desc: "Increases standard health limits"},
	{Name: "Head of the Game", PCOffset: 0x12c, Key: "headOfTheGame", Desc: "Enables bobble head scaling"},
	{Name: "Infinite Sprint", PCOffset: 0x130, Key: "infiniteSprint", Desc: "Sprint without depleting energy"},
	{Name: "Penguin Projectile", PCOffset: 0x134, Key: "penguinProjectile", Desc: "Allows throwing penguin projectiles"},
	{Name: "Unlock Levels", PCOffset: 0x138, Key: "unlockLevels", Desc: "Unlocks stage select levels"},
	// 0x13c is an unused/reserved slot -- confirmed both empirically (it
	// didn't change between the toggled-off and toggled-on save) and
	// structurally (mad2cheatapply.cpp's live-memory offsets have a gap
	// here exactly twice the size of every other slot's gap, i.e. one
	// slot's worth of space with no named cheat assigned to it).
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

// ---------------------------------------------------------------------
// Story-progress ability flags -- climb (unlocked by completing Rites of
// Passage) and monkey collectibles (unlocked by leaving Fix the Plane).
// Found this session by hooking the game's own save-file writes
// (mad2climbprobe's NtCreateFile/WriteFile logger) across a real
// playthrough and diffing consecutive writes, not by design like
// cheatsList's dedicated per-cheat uint32 slots -- so the shapes don't
// match: climb is a single byte, monkeys is a bundle of fields (one
// uint32, the rest bytes) that all flip together the instant Fix the
// Plane is left. Both live entirely below pcGapOffset (0x120), so unlike
// cheatsList they need no PC/Wii offset shift -- confirmed identical on
// both platforms up to that point.
// ---------------------------------------------------------------------

const climbAbilityOffset = 0x70

// ReadClimbAbility reports whether the climb ability is unlocked.
func ReadClimbAbility(path string) (bool, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return false, err
	}
	if len(data) < saveFileSize {
		return false, fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	return data[climbAbilityOffset] != 0, nil
}

// WriteClimbAbility sets the climb ability flag and recalculates/rewrites
// the CRC32 footer.
func WriteClimbAbility(path string, unlocked bool) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if len(data) < saveFileSize {
		return fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)

	if unlocked {
		data[climbAbilityOffset] = 1
	} else {
		data[climbAbilityOffset] = 0
	}
	writeU32(data, len(data)-4, saveChecksum(data), isWii)
	return os.WriteFile(path, data, 0644)
}

// monkeyCollectibleByteFields is the full bundle of single-byte fields
// confirmed (via the write-log diff) to flip 0->1 together the instant Fix
// the Plane is left -- 0x68 goes to 2 instead, for reasons not otherwise
// investigated. monkeyCollectibleUint32Offset (0x18) flips 0xFFFFFFFF -> 0
// in the same instant, apparently a sentinel-vs-real-counter transition
// rather than a plain bool. Which single field (if any one alone) actually
// gates the mechanic hasn't been isolated -- verified live that writing
// this exact bundle onto a save that never visited Fix the Plane made
// monkeys collectible, so the write path always touches all of it
// together to match the only configuration confirmed to work.
var monkeyCollectibleByteFields = map[int]byte{
	0x30: 1, 0x48: 1, 0x58: 1, 0x68: 2, 0x80: 1, 0xA8: 1, 0x3A0: 1, 0x3A4: 1,
}

const monkeyCollectibleUint32Offset = 0x18

// ReadMonkeyCollectible reports whether monkeys are collectible, using the
// uint32 field alone (0 unlocked, 0xFFFFFFFF locked) as the read-back
// signal -- see monkeyCollectibleByteFields' comment for why the write
// path always touches the whole bundle regardless.
func ReadMonkeyCollectible(path string) (bool, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return false, err
	}
	if len(data) < saveFileSize {
		return false, fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)
	return readU32(data, monkeyCollectibleUint32Offset, isWii) == 0, nil
}

// WriteMonkeyCollectible sets (or clears) the whole monkeyCollectibleByteFields
// bundle plus the uint32 field, and recalculates/rewrites the CRC32 footer.
func WriteMonkeyCollectible(path string, unlocked bool) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if len(data) < saveFileSize {
		return fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)

	if unlocked {
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
	writeU32(data, len(data)-4, saveChecksum(data), isWii)
	return os.WriteFile(path, data, 0644)
}

// ---------------------------------------------------------------------
// Coin economy -- found by hooking the game's own save-file writes
// (mad2climbprobe's SaveWriteLog) across a from-scratch playthrough where
// every level was given a distinct, partial coin count specifically so
// its offset would be unambiguous (a maxed-out "100" would collide across
// the many levels that happen to cap at 100 -- see levelCoinMax below).
// There is no single persistent "current coin balance" field: the shop
// total is computed live as (sum of every level's own coin count) -
// (amountSpentOffset), and only the two operands are ever actually
// persisted.
//
// Every level's own coin count turned out to live in one contiguous table
// of 18 uint32 slots, 0x16C-0x1B0 inclusive -- confirmed bounded, since
// 0x168 and 0x1B4 never changed across any level in the reference
// playthrough. VolcanoRave (finished with 0 coins that session, so
// invisible to a value search) was resolved separately: a distinct test
// value was poked into each of the two remaining unidentified slots and
// cross-checked against which one showed up as VolcanoRave's own in-game
// coin count after reloading.
//
// The other slot, 0x19C, was initially assumed to be unused padding (only
// 17 of the 18 slots matched a named level in ../mad2mod's own ported
// level list) -- that assumption was WRONG, caught live: a leftover test
// value poked into 0x19C during the VolcanoRave hunt was never cleared
// before writing every level's designed max into the rest of the table,
// and the game's own displayed coin total came back exactly
// (sum of the 17 known maxes) + (that leftover value), proving 0x19C is
// genuinely summed into whatever running total the game computes even
// though no in-game screen was ever observed to show it as its own named
// level. Kept here as "Unknown0x19C" -- real, read/write-able, and
// included in every total, just not tied to a level name we've
// identified yet.
// ---------------------------------------------------------------------

const amountSpentOffset = 0x0

// levelCoinOffsets maps every level with a discovered coin-count slot to
// its byte offset in the save file (PC layout; same offsets used
// regardless of isWii since this whole table sits below pcGapOffset,
// like climbAbilityOffset/monkeyCollectible* above).
var levelCoinOffsets = map[string]int{
	"IslandFever":          0x16C,
	"Prepare2Launch":       0x170,
	"BraveNewWild":         0x174,
	"Waterhole":            0x178,
	"penguins":             0x17C,
	"FixThePlane":          0x180,
	"RitesOfPassage":       0x184,
	"ConvoyChase":          0x188,
	"Wooing_Gloria":        0x18C,
	"VolcanoRave":          0x190,
	"Morts_Adventure":      0x194,
	"Dam_Busters":          0x198,
	"Unknown0x19C":         0x19C,
	"Watercaves":           0x1A0,
	"DrMelman":             0x1A4,
	"MartyRace":            0x1A8,
	"penguins2":            0x1AC,
	"Prepare2Launch_Plane": 0x1B0,
}

// LevelCoinLevels lists every level with a discovered coin-count slot, in
// save-file offset order (matches the table's own physical layout, not
// alphabetical) -- the order the Cheats UI displays them in.
var LevelCoinLevels = []string{
	"IslandFever", "Prepare2Launch", "BraveNewWild", "Waterhole", "penguins",
	"FixThePlane", "RitesOfPassage", "ConvoyChase", "Wooing_Gloria",
	"VolcanoRave", "Morts_Adventure", "Dam_Busters", "Unknown0x19C",
	"Watercaves", "DrMelman", "MartyRace", "penguins2", "Prepare2Launch_Plane",
}

// LevelCoinMax is each level's own designed coin cap, taken from
// mad2rando5/levels.go's own `AllLevels` table (the in-repo randomizer's
// coin/monkey metadata) -- not derived from the save format itself, just
// carried along here since it's the natural "max" value for that field.
// This already includes any coins gated behind an ability (e.g.
// Waterhole's 120 includes the 60 locked behind climb) since that's how
// mad2rando5 itself represents Coins/LockedCoins.
//
// NOT sourced from ../mad2mod/mad2rando.go's older, since-superseded copy
// of this same table -- that one has penguins=75/penguins2=15 (sums to
// 90), which turned out to be stale: mad2rando5 already carries the fix,
// penguins=85/penguins2=15 (sums to 100), confirmed against the in-game
// "Penguin Caper" combined total.
var LevelCoinMax = map[string]uint32{
	"IslandFever":          100,
	"Prepare2Launch":       15,
	"BraveNewWild":         100,
	"Waterhole":            120,
	"penguins":             85,
	"FixThePlane":          300,
	"RitesOfPassage":       100,
	"ConvoyChase":          100,
	"Wooing_Gloria":        100,
	"VolcanoRave":          100,
	"Morts_Adventure":      100,
	"Dam_Busters":          100,
	"Watercaves":           100,
	"DrMelman":             52,
	"MartyRace":            28,
	"penguins2":            15,
	"Prepare2Launch_Plane": 85,
}

// ReadAmountSpent returns the persistent total-coins-spent-in-shops value
// (subtracted live from the level-coin-count sum to produce the shop's
// displayed total).
func ReadAmountSpent(path string) (uint32, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	if len(data) < saveFileSize {
		return 0, fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)
	return readU32(data, amountSpentOffset, isWii), nil
}

// WriteAmountSpent sets the persistent total-coins-spent-in-shops value
// and recalculates/rewrites the CRC32 footer.
func WriteAmountSpent(path string, value uint32) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if len(data) < saveFileSize {
		return fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)
	writeU32(data, amountSpentOffset, value, isWii)
	writeU32(data, len(data)-4, saveChecksum(data), isWii)
	return os.WriteFile(path, data, 0644)
}

// ReadLevelCoins returns the given level's own persisted coin count (level
// must be a key of levelCoinOffsets, e.g. one of LevelCoinLevels).
func ReadLevelCoins(path, level string) (uint32, error) {
	off, ok := levelCoinOffsets[level]
	if !ok {
		return 0, fmt.Errorf("no known coin offset for level %q", level)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	if len(data) < saveFileSize {
		return 0, fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)
	return readU32(data, off, isWii), nil
}

// WriteLevelCoins sets the given level's own persisted coin count and
// recalculates/rewrites the CRC32 footer.
func WriteLevelCoins(path, level string, value uint32) error {
	off, ok := levelCoinOffsets[level]
	if !ok {
		return fmt.Errorf("no known coin offset for level %q", level)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	if len(data) < saveFileSize {
		return fmt.Errorf("%s is %d bytes, expected %d -- not a Madagascar 2 save file", filepath.Base(path), len(data), saveFileSize)
	}
	isWii := detectWiiSave(data)
	writeU32(data, off, value, isWii)
	writeU32(data, len(data)-4, saveChecksum(data), isWii)
	return os.WriteFile(path, data, 0644)
}
