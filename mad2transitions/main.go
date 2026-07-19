package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io/ioutil"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"unicode/utf16"
)

// Master list of normalized internal level names
var masterLevelNames = map[string]string{
	"bravenewwild":                  "BraveNewWild",
	"dambusters":                    "DamBusters",
	"drmelman":                      "DrMelman",
	"dutyfree":                      "DutyFree",
	"fixtheplane":                   "FixThePlane",
	"islandfever":                   "IslandFever",
	"convoychase":                   "ConvoyChase",
	"jeepchase":                     "ConvoyChase", // Normalized to ConvoyChase
	"map":                           "Map",
	"martyrace":                     "MartysRace",
	"mortsadventure":                "MortsAdventure",
	"penguins2":                     "penguins2",
	"penguins":                      "penguins",
	"prepare2launch":                "Prepare2launch",
	"prepare2launchplane":           "Prepare2Launch_plane",
	"ritesofpassage":                "RitesOfPassage",
	"volcanorave":                   "VolcanoRave",
	"watercaves":                    "Watercaves",
	"waterhole":                     "Waterhole",
	"wooinggloria":                  "WooingGloria",
	"cardmatchgame":                 "Card_Match_Game",
	"divinglocationislandfever":     "DivingLocation_IslandFever",
	"divinglocationpreparetolaunch": "DivingLocation_PrepareToLaunch",
	"divinglocationritesofpassage":  "DivingLocation_RitesOfPassage",
	"divinglocationwaterhole":       "DivingLocation_Waterhole",
	"golfminigame":                  "Golf_minigame",
	"hungryhippo":                   "HungryHippo",
	"minigamedivinglocationmenu":    "Minigame_Diving_Location_Menu",
	"minigamehotdurian":             "Minigame_HotDurian",
	"ropmusicalchairs":              "RoP_MusicalChairs",
	"soccer":                        "Soccer",
	"animalchess":                   "animal_chess",
	"golf3holes":                    "golf_3holes",
	"golfbaggagecheck":              "golf_baggagecheck",
	"golfcake":                      "golf_cake",
	"golfcratercrossing":            "golf_cratercrossing",
	"golfcrossedpaths":              "golf_crossedpaths",
	"golffoosaball":                 "golf_foosaBall",
	"golfjunkyard":                  "golf_junkyard",
	"golflovelylumps":               "golf_lovelylumps",
	"golfmaze":                      "golf_maze",
	"golfravenousrhinos":            "golf_ravenousRhinos",
	"golftargettree":                "golf_targetTree",
	"title":                         "title",
	"credits":                       "Credits",
	"multiplayertourneyfinish":      "MultiplayerTourneyFinish",
}

type PakMetadata struct {
	StringPoolStart uint32   `json:"string_pool_start"`
	Strings         []string `json:"strings"`
}

type StringOffset struct {
	Value  string
	Offset uint32
}

type Transition struct {
	RawString string
	Target    string
	Verified  bool
}

type LevelResult struct {
	LevelName   string
	Transitions []Transition
}

func cleanLevelName(s string) string {
	s = strings.TrimSpace(s)
	s = strings.ReplaceAll(s, "\n", "")
	s = strings.ReplaceAll(s, "\r", "")
	s = strings.ToLower(s)
	s = strings.ReplaceAll(s, " ", "")
	s = strings.ReplaceAll(s, "_", "")
	return s
}

func getUtf16LESize(s string) int {
	runes := utf16.Encode([]rune(s))
	return len(runes)*2 + 2
}

func findPointerInBld(bldData []byte, segID byte, targetOffset uint32) bool {
	ptrVal := (uint32(segID) << 24) | targetOffset
	ptrBytes := make([]byte, 4)
	binary.LittleEndian.PutUint32(ptrBytes, ptrVal)
	return bytes.Contains(bldData, ptrBytes)
}

func parsePakStrings(pakJsonPath string) ([]StringOffset, error) {
	pakJsonData, err := ioutil.ReadFile(pakJsonPath)
	if err != nil {
		return nil, err
	}

	var pakMeta PakMetadata
	if err := json.Unmarshal(pakJsonData, &pakMeta); err != nil {
		return nil, err
	}

	var offsets []StringOffset
	currOffset := uint32(0)
	for _, s := range pakMeta.Strings {
		offsets = append(offsets, StringOffset{
			Value:  s,
			Offset: pakMeta.StringPoolStart + currOffset,
		})
		currOffset += uint32(getUtf16LESize(s))
	}
	return offsets, nil
}

func main() {
	inputDir := flag.String("input", "mad2raw", "Path to the unpacked mad2raw directory")
	outputFile := flag.String("output", "transitions_report.md", "Path to output Markdown file")
	flag.Parse()

	// Parse global string pool offsets
	globalPakJson := filepath.Join(*inputDir, "globalBLD", "ENGLISH.pak.json")
	globalStringOffsets, err := parsePakStrings(globalPakJson)
	if err != nil {
		fmt.Printf("Warning: Failed to load global string pool: %v\n", err)
	} else {
		fmt.Printf("Loaded %d global string offsets\n", len(globalStringOffsets))
	}

	files, err := ioutil.ReadDir(*inputDir)
	if err != nil {
		fmt.Printf("Error reading input directory: %v\n", err)
		os.Exit(1)
	}

	var results []LevelResult

	for _, f := range files {
		// Skip non-directories, non-level folders, and the master 'globalBLD' folder
		if !f.IsDir() || !strings.HasSuffix(f.Name(), "BLD") || f.Name() == "globalBLD" {
			continue
		}

		levelName := strings.TrimSuffix(f.Name(), "BLD")
		dirPath := filepath.Join(*inputDir, f.Name())

		pakJsonPath := filepath.Join(dirPath, "ENGLISH.pak.json")
		bldPath := filepath.Join(dirPath, "level.bld.orig")
		if _, err := os.Stat(bldPath); os.IsNotExist(err) {
			bldPath = filepath.Join(dirPath, "level.bld")
		}

		// Skip if essential files do not exist
		if _, err := os.Stat(pakJsonPath); os.IsNotExist(err) {
			continue
		}
		if _, err := os.Stat(bldPath); os.IsNotExist(err) {
			continue
		}

		// Read local strings
		localStringOffsets, err := parsePakStrings(pakJsonPath)
		if err != nil {
			fmt.Printf("Warning: Failed to parse %s: %v\n", pakJsonPath, err)
			continue
		}

		// Read BLD binary
		bldData, err := ioutil.ReadFile(bldPath)
		if err != nil {
			fmt.Printf("Warning: Failed to read %s: %v\n", bldPath, err)
			continue
		}

		// Collect level transitions (deduplicated by display target)
		transMap := make(map[string]Transition)

		// 1. Scan local strings (Segment 2 pointers)
		for _, s := range localStringOffsets {
			cleaned := cleanLevelName(s.Value)

			for k, displayName := range masterLevelNames {
				kClean := strings.ReplaceAll(k, "_", "")
				if cleaned == kClean {
					// Filter out self-references
					if cleanLevelName(levelName) == cleanLevelName(displayName) {
						break
					}
					isVerified := findPointerInBld(bldData, 2, s.Offset)

					existing, ok := transMap[displayName]
					if !ok {
						transMap[displayName] = Transition{
							RawString: strings.TrimSpace(s.Value),
							Target:    displayName,
							Verified:  isVerified,
						}
					} else if isVerified && !existing.Verified {
						existing.Verified = true
						existing.RawString = strings.TrimSpace(s.Value)
						transMap[displayName] = existing
					}
					break
				}
			}
		}

		// 2. Scan global strings (Segment 255 pointers)
		// Only check if global strings were parsed successfully
		if len(globalStringOffsets) > 0 {
			for _, s := range globalStringOffsets {
				cleaned := cleanLevelName(s.Value)

				for k, displayName := range masterLevelNames {
					kClean := strings.ReplaceAll(k, "_", "")
					if cleaned == kClean {
						// Filter out self-references
						if cleanLevelName(levelName) == cleanLevelName(displayName) {
							break
						}
						// Global strings must be explicitly referenced in the level database
						isVerified := findPointerInBld(bldData, 255, s.Offset)
						if isVerified {
							existing, ok := transMap[displayName]
							if !ok {
								transMap[displayName] = Transition{
									RawString: strings.TrimSpace(s.Value),
									Target:    displayName,
									Verified:  true,
								}
							} else if !existing.Verified {
								existing.Verified = true
								existing.RawString = strings.TrimSpace(s.Value)
								transMap[displayName] = existing
							}
						}
						break
					}
				}
			}
		}

		if len(transMap) > 0 {
			var transitions []Transition
			for _, t := range transMap {
				transitions = append(transitions, t)
			}
			// Sort transitions by target name
			sort.Slice(transitions, func(i, j int) bool {
				return transitions[i].Target < transitions[j].Target
			})

			results = append(results, LevelResult{
				LevelName:   levelName,
				Transitions: transitions,
			})
		}
	}

	// Sort level results alphabetically
	sort.Slice(results, func(i, j int) bool {
		return results[i].LevelName < results[j].LevelName
	})

	// Generate Markdown Report
	var md bytes.Buffer
	md.WriteString("# Madagascar 2 Level Transitions Report\n\n")
	md.WriteString("This report lists all level transitions extracted from the game's compiled `.bld` databases and resolved via local `.pak` (Segment 2) and global `.pak` (Segment 255) localization packages.\n\n")

	// Table Summary
	md.WriteString("## Summary of Transitions\n\n")
	md.WriteString("| Source Level | Target Level | Connection Type | Raw String Entry |\n")
	md.WriteString("|--------------|--------------|-----------------|------------------|\n")
	for _, res := range results {
		for _, t := range res.Transitions {
			connType := "Probable (String only)"
			if t.Verified {
				connType = "**Verified (Pointer)**"
			}
			md.WriteString(fmt.Sprintf("| %s | %s | %s | `%s` |\n", res.LevelName, t.Target, connType, t.RawString))
		}
	}
	md.WriteString("\n---\n\n")

	// Detailed breakdown
	md.WriteString("## Detailed Level Breakdown\n\n")
	for _, res := range results {
		md.WriteString(fmt.Sprintf("### [%s]\n\n", res.LevelName))

		var verified []Transition
		var probable []Transition
		for _, t := range res.Transitions {
			if t.Verified {
				verified = append(verified, t)
			} else {
				probable = append(probable, t)
			}
		}

		if len(verified) > 0 {
			md.WriteString("#### Verified Transitions\n")
			md.WriteString("> [!NOTE]\n")
			md.WriteString("> Explicitly referenced in the level database via Segment 2 or Segment 255 pointers.\n\n")
			for _, t := range verified {
				md.WriteString(fmt.Sprintf("- **%s** (String: `%s`)\n", t.Target, t.RawString))
			}
			md.WriteString("\n")
		}

		if len(probable) > 0 {
			md.WriteString("#### Probable Transitions\n")
			md.WriteString("> [!TIP]\n")
			md.WriteString("> Present in local string pool but not directly pointed to by a Segment 2 reference.\n\n")
			for _, t := range probable {
				md.WriteString(fmt.Sprintf("- **%s** (String: `%s`)\n", t.Target, t.RawString))
			}
			md.WriteString("\n")
		}
	}

	// Write to output file
	err = ioutil.WriteFile(*outputFile, md.Bytes(), 0644)
	if err != nil {
		fmt.Printf("Error writing report: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Successfully generated Markdown report: %s\n", *outputFile)
}
