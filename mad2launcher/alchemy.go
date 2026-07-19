package main

import (
	"fmt"
	"os"
	"regexp"
	"sort"
)

// LevelNames is the full set of valid alchemy.xml startLevel identifiers,
// ported from ../mad2mod/mad2rando.go's own `levels` table (its randomizer
// needs the same list to know what it's allowed to shuffle). Kept as a
// plain name list here since the launcher only needs to offer a level
// picker, not the coin/monkey/minigame metadata the randomizer tracks.
var LevelNames = func() []string {
	names := []string{
		"animal_chess", "BraveNewWild", "Card_Match_Game", "ConvoyChase", "Credits",
		"Dam_Busters", "DivingLocation_IslandFever", "DivingLocation_PrepareToLaunch",
		"DivingLocation_RitesOfPassage", "DivingLocation_Waterhole", "DrMelman", "DutyFree",
		"FixThePlane", "golf_3holes", "golf_baggagecheck", "golf_cake", "golf_cratercrossing",
		"golf_crossedpaths", "golf_foosaBall", "golf_junkyard", "golf_lovelylumps", "golf_maze",
		"Golf_minigame", "golf_ravenousRhinos", "golf_targetTree", "HungryHippo", "IslandFever",
		"map", "MartyRace", "Minigame_Diving_Location_Menu", "Minigame_HotDurian",
		"Morts_Adventure", "penguins", "penguins2", "Prepare2Launch_Plane", "Prepare2Launch",
		"RitesOfPassage", "RoP_MusicalChairs", "Soccer", "title", "VolcanoRave", "Watercaves",
		"Waterhole", "Wooing_Gloria",
	}
	sort.Strings(names)
	return names
}()

var startLevelRe = regexp.MustCompile(`startLevel\s*=\s*"[^"]*"`)
var presentationIntervalRe = regexp.MustCompile(`presentationInterval\s*=\s*"[^"]*"`)

// AlchemySettings is the subset of alchemy.xml this launcher reads/edits.
type AlchemySettings struct {
	StartLevel           string
	PresentationInterval string
}

// ReadAlchemyXML extracts startLevel (Tfb) and presentationInterval (Gfx)
// from path. A missing file returns (nil, nil) since not every discovered
// install is guaranteed to have been launched by the game yet.
func ReadAlchemyXML(path string) (*AlchemySettings, error) {
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	content := string(data)
	s := &AlchemySettings{}
	if m := regexp.MustCompile(`startLevel\s*=\s*"([^"]*)"`).FindStringSubmatch(content); m != nil {
		s.StartLevel = m[1]
	}
	if m := regexp.MustCompile(`presentationInterval\s*=\s*"([^"]*)"`).FindStringSubmatch(content); m != nil {
		s.PresentationInterval = m[1]
	}
	return s, nil
}

// WriteAlchemyStartLevel rewrites just the startLevel="..." attribute,
// same surgical regex-replace ../mad2mod's setStartLevelInXml uses --
// alchemy.xml's shape is simple and stable enough that a full XML parser
// isn't worth pulling in here either.
func WriteAlchemyStartLevel(path, level string) error {
	return replaceAlchemyAttr(path, startLevelRe, fmt.Sprintf(`startLevel = "%s"`, level))
}

// WriteAlchemyPresentationInterval rewrites presentationInterval="...".
func WriteAlchemyPresentationInterval(path, value string) error {
	return replaceAlchemyAttr(path, presentationIntervalRe, fmt.Sprintf(`presentationInterval = "%s"`, value))
}

func replaceAlchemyAttr(path string, re *regexp.Regexp, replacement string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	content := re.ReplaceAllString(string(data), replacement)
	return os.WriteFile(path, []byte(content), 0644)
}
