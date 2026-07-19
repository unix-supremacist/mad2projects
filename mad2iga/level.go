package iga

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
)

type LevelCoord struct {
	Offset uint32  `json:"offset"`
	X      float32 `json:"x"`
	Y      float32 `json:"y"`
	Z      float32 `json:"z"`
	Label  string  `json:"label,omitempty"`
}

type LevelMeta struct {
	Coordinates []LevelCoord `json:"coordinates"`
}

// loadNamesMap parses names.json in the target directory to load custom name-to-offset mappings.
func loadNamesMap(jsonPath string) (map[string]uint32, map[uint32]string) {
	nameToOffset := make(map[string]uint32)
	offsetToName := make(map[uint32]string)

	dir := filepath.Dir(jsonPath)
	namesPath := filepath.Join(dir, "names.json")

	data, err := os.ReadFile(namesPath)
	if err != nil {
		return nameToOffset, offsetToName
	}

	var rawMap map[string]interface{}
	if err := json.Unmarshal(data, &rawMap); err != nil {
		return nameToOffset, offsetToName
	}

	for k, v := range rawMap {
		var offsetVal uint32
		var nameVal string

		var parsedOffset uint64
		_, err := fmt.Sscan(k, &parsedOffset)
		if err == nil {
			offsetVal = uint32(parsedOffset)
			if nameStr, ok := v.(string); ok {
				nameVal = nameStr
			}
		} else {
			nameVal = k
			if offsetNum, ok := v.(float64); ok {
				offsetVal = uint32(offsetNum)
			}
		}

		if nameVal != "" && offsetVal != 0 {
			nameToOffset[nameVal] = offsetVal
			offsetToName[offsetVal] = nameVal
		}
	}

	return nameToOffset, offsetToName
}

// DumpLevelCoords scans level.bld for candidate float coordinates using 3D matrix signature.
func DumpLevelCoords(bldPath, jsonPath string) error {
	data, err := os.ReadFile(bldPath)
	if err != nil {
		return err
	}

	if len(data) < 0x30 {
		return fmt.Errorf("file too small")
	}

	magic := binary.LittleEndian.Uint32(data[0:4])
	if magic != 0x49475A01 {
		return fmt.Errorf("invalid magic: 0x%08X", magic)
	}

	sec0Off := binary.LittleEndian.Uint32(data[0x10:0x14])
	dirTableOff := binary.LittleEndian.Uint32(data[sec0Off+0x0C : sec0Off+0x10])
	strPoolOff := binary.LittleEndian.Uint32(data[sec0Off+0x2C : sec0Off+0x30])

	// Parse Class Names from Directory Table
	var classNames []string
	pos := sec0Off + dirTableOff
	end := sec0Off + strPoolOff

	for i := pos; i < end; {
		if data[i] != 0 {
			start := i
			for i < end && data[i] != 0 {
				i++
			}
			str := string(data[start:i])
			if len(str) > 2 && str != "t\n" {
				classNames = append(classNames, str)
			}
		} else {
			i++
		}
	}

	// Parse String Pool
	realStrStart := sec0Off + strPoolOff + 0xA840
	sec0Size := binary.LittleEndian.Uint32(data[0x14:0x18])
	strPoolEnd := sec0Off + sec0Size

	var stringsList []string
	if realStrStart < strPoolEnd && strPoolEnd <= uint32(len(data)) {
		poolData := data[realStrStart:strPoolEnd]
		stringsList = append(stringsList, "ttr", "OpMoveFrom", "igHandleList")
		for idx := 0; idx < len(poolData); {
			end := idx
			for end < len(poolData) && poolData[end] != 0 {
				end++
			}
			str := string(poolData[idx:end])
			if len(str) > 0 {
				stringsList = append(stringsList, str)
			}
			idx = end + 1
		}
	}

	// Find the class index for ActorInfo (used to resolve an instance's name below).
	actorInfoIdx := -1
	for idx, name := range classNames {
		if name == "ActorInfo" {
			actorInfoIdx = idx
			break
		}
	}

	// knownOffsets: byte offset of a class instance's coordinate-shaped field,
	// relative to the instance's own start (where classNames[classIdx] lives at +0).
	//
	// Verified against ../mad2assetextractor/wii_demo_ref/Mad2.elf (the unstripped
	// Wii demo build's PowerPC symbol table + decoded accessor offsets — see
	// docs/CLASS_SCHEMA.md). Two entries that were previously here based on pure
	// byte-pattern guessing turned out to be wrong once checked against real field
	// offsets and were removed:
	//   - "GeneratorGenerationFields": 32  -- that offset is `spawnRate` (a float,
	//     not a coordinate); none of GGF's 7 fields (spawnRate/maxAlive/
	//     currentlyAlive/lifespan/lifespanRandomness/maxDistance/midpointPercent,
	//     offsets 0x20-0x38) are positional.
	//   - "GeneratorMovementFields": 44   -- that offset is `initialSpeed` (also a
	//     lone float); GMF's ~24 fields are all particle-motion tuning scalars
	//     (drag/gravity/accel) plus a few direction *vectors* (e.g.
	//     startAccelDirection at +76) that are directions, not positions.
	// The real GeneratorInfo spawn position lives directly on GeneratorInfo itself
	// (Gap::TFBParticleInfo::GeneratorInfo::setVector writes a Vec3f at +0x60/96),
	// not indirectly through its GGF/GMF sub-objects as previously assumed.
	//
	// NOTE: even with the correct offset, the scan loop below still can't find
	// GeneratorInfo's position in practice — tested against a real level
	// (Waterhole) and got 0 hits. Cause: the scan only fires on a *homogeneous
	// matrix row* shape (3 floats + exact w=1.0 + zero padding on both sides),
	// which is how ActorInfo/ActorWaypoint/etc. store their transforms.
	// GeneratorInfo's position is a bare Vec3f with no such w=1.0 neighbor, so
	// it structurally never matches the trigger pattern — this entry is kept
	// as accurate reference data (and harmless), not because it currently finds
	// anything. Do NOT "fix" this by scanning for the class-index byte directly
	// instead of anchoring on a coordinate-shaped hit first: tried that as a
	// probe (not committed) and a bare classIdx==75 (GeneratorInfo in Waterhole)
	// byte match occurs 1524 times in that file for ~49 real instances — a raw
	// integer is just too common to use as a location anchor on its own. Finding
	// real GeneratorInfo instances needs actual Section 1 graph traversal
	// (following a genuine pointer from GeneratorInfoList/ParticleSystemInfo),
	// not byte scanning. That also means the old findGeneratorName mechanism's
	// reported "49 GeneratorInfo found" in research_notes.md was almost
	// certainly mislabeling other classes' real coordinate hits, not finding
	// genuine GeneratorInfo positions either — same false-positive-prone
	// approach, just one layer removed.
	knownOffsets := map[string]int{
		"GeneratorInfo":           96,
		"ActorWaypoint":           20,
		"ActorInfo":               80,
		"igSceneInfo":             76,
		"igGroup":                 76,
		"igNonRefCountedNodeList": 76,
		"igNodeList":              76,
		"igSimpleUserInfo":        76,
		"igMaterialGroup":         76,
		"igTechniqueInstance":     76,
		"igGeometry":              76,
		"igAABox":                 76,
	}

	_, offsetToName := loadNamesMap(jsonPath)

	var coords []LevelCoord
	labeledCount := 0

	// Scan the entire level file for 3D homogeneous matrix translation vectors
	for i := 32; i < len(data)-32; i += 4 {
		f1 := math.Float32frombits(binary.LittleEndian.Uint32(data[i : i+4]))
		f2 := math.Float32frombits(binary.LittleEndian.Uint32(data[i+4 : i+8]))
		f3 := math.Float32frombits(binary.LittleEndian.Uint32(data[i+8 : i+12]))

		wVal := binary.LittleEndian.Uint32(data[i+12 : i+16])
		postZero := binary.LittleEndian.Uint32(data[i+16 : i+20])
		preZero := binary.LittleEndian.Uint32(data[i-4 : i])

		if wVal == 0x3F800000 && postZero == 0 && preZero == 0 {
			if isTrueSpatialCoordinate(f1, f2, f3) {
				label := ""

				// Prioritize custom name map
				if name, exists := offsetToName[uint32(i)]; exists {
					label = name
				} else {
					// Identify class by scanning backwards
					className := ""
					objAddr := uint32(0)

					classPriority := []string{
						"ActorInfo",
						"GeneratorInfo",
						"ActorWaypoint",
						"igSceneInfo",
						"igGroup",
						"igNonRefCountedNodeList",
						"igNodeList",
						"igSimpleUserInfo",
						"igMaterialGroup",
						"igTechniqueInstance",
						"igGeometry",
						"igAABox",
					}
					for _, name := range classPriority {
						expectedOffset := knownOffsets[name]
						addr := int32(i) - int32(expectedOffset)
						if addr >= 0 && addr+4 <= int32(len(data)) {
							val := binary.LittleEndian.Uint32(data[addr : addr+4])
							if val > 1 && val < uint32(len(classNames)) && classNames[val] == name {
								className = name
								objAddr = uint32(addr)
								break
							}
						}
					}

					if className == "" {
						// Backwards fallback scan
						for bOffset := 4; bOffset <= 96; bOffset += 4 {
							addr := int32(i) - int32(bOffset)
							if addr >= 0 && addr+4 <= int32(len(data)) {
								val := binary.LittleEndian.Uint32(data[addr : addr+4])
								if val > 1 && val < uint32(len(classNames)) {
									className = classNames[val]
									objAddr = uint32(addr)
									break
								}
							}
						}
					}

					// Resolve name based on class
					if className == "ActorInfo" && actorInfoIdx != -1 && objAddr+12 <= uint32(len(data)) {
						nameVal := binary.LittleEndian.Uint32(data[objAddr+8 : objAddr+12])
						if int(nameVal) > 2 && int(nameVal)+1 < len(stringsList) {
							label = stringsList[nameVal+1]
						}
					}
					// TODO(GeneratorInfo naming): GeneratorInfo has no name field of
					// its own (verified via wii_demo_ref/wii_field_schema.json) — it's
					// wrapped by GeneratorInfoList -> ParticleSystemInfo per
					// research_notes.md's hierarchy, and the human-readable instance
					// name likely lives on that wrapper, not here. Until that's
					// reverse-engineered, GeneratorInfo hits fall back to the bare
					// class-name label below instead of a resolved instance name.

					// Fallback to class name if no custom instance name was resolved
					if label == "" && className != "" {
						label = className
					}
				}

				if label != "" {
					labeledCount++
				}

				coords = append(coords, LevelCoord{
					Offset: uint32(i),
					X:      f1,
					Y:      f2,
					Z:      f3,
					Label:  label,
				})
				i += 8
			}
		}
	}

	meta := LevelMeta{Coordinates: coords}
	jsonData, err := json.MarshalIndent(meta, "", "  ")
	if err != nil {
		return err
	}

	fmt.Printf("  Dumped %d precise coordinates, auto-labeled %d instances to JSON\n", len(coords), labeledCount)
	return os.WriteFile(jsonPath, jsonData, 0644)
}

// CompileLevelCoords applies JSON coordinates changes back into the binary level.bld file.
func CompileLevelCoords(jsonPath, origBldPath, outBldPath string) error {
	jsonData, err := os.ReadFile(jsonPath)
	if err != nil {
		return err
	}

	var meta LevelMeta
	if err := json.Unmarshal(jsonData, &meta); err != nil {
		return err
	}

	data, err := os.ReadFile(origBldPath)
	if err != nil {
		return err
	}

	nameToOffset, _ := loadNamesMap(jsonPath)

	for _, c := range meta.Coordinates {
		offset := c.Offset
		if offset == 0 && c.Label != "" {
			if resolvedOffset, exists := nameToOffset[c.Label]; exists {
				offset = resolvedOffset
			} else {
				return fmt.Errorf("could not resolve offset for label %q", c.Label)
			}
		}

		if offset+12 > uint32(len(data)) {
			return fmt.Errorf("offset 0x%X (label %q) is out of bounds", offset, c.Label)
		}
		binary.LittleEndian.PutUint32(data[offset:offset+4], math.Float32bits(c.X))
		binary.LittleEndian.PutUint32(data[offset+4:offset+8], math.Float32bits(c.Y))
		binary.LittleEndian.PutUint32(data[offset+8:offset+12], math.Float32bits(c.Z))
	}

	return os.WriteFile(outBldPath, data, 0644)
}

func isTrueSpatialCoordinate(f1, f2, f3 float32) bool {
	if math.IsNaN(float64(f1)) || math.IsNaN(float64(f2)) || math.IsNaN(float64(f3)) ||
		math.IsInf(float64(f1), 0) || math.IsInf(float64(f2), 0) || math.IsInf(float64(f3), 0) {
		return false
	}
	if f1 == f2 && f2 == f3 {
		return false
	}

	abs1 := math.Abs(float64(f1))
	abs2 := math.Abs(float64(f2))
	abs3 := math.Abs(float64(f3))

	if abs1 < 12000.0 && abs2 < 12000.0 && abs3 < 12000.0 {
		maxAbs := math.Max(abs1, math.Max(abs2, abs3))
		if maxAbs > 5.0 {
			return true
		}
	}
	return false
}
