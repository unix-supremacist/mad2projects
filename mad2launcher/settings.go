package main

import (
	"encoding/json"
	"os"
	"path/filepath"
)

// settingsFileName is written next to the launcher's own working directory,
// same convention as every mod's own log/config files (relative to cwd, not
// a hardcoded absolute path).
const settingsFileName = "mad2launcher_settings.json"

// InstallSettings holds everything the launcher itself owns per-install --
// i.e. settings that live nowhere in the game/mods' own config.cfg because
// they're about how *this launcher* runs the game (gamescope/Proton/wine),
// not about the mods themselves.
type InstallSettings struct {
	WinePrefix   string `json:"winePrefix"`
	ProtonPath   string `json:"protonPath"`
	GameID       string `json:"gameId"`
	InputRes     string `json:"inputRes"`     // "1280x720"
	OutputRes    string `json:"outputRes"`    // "1280x720"
	Scaling      string `json:"scaling"`      // Bilinear/FSR/NIS/Nearest
	FsrSharpness string `json:"fsrSharpness"` // "0".."20"
	FpsCap       string `json:"fpsCap"`       // "" or "0" = unlimited
	Presentation string `json:"presentation"` // written into alchemy.xml Gfx presentationInterval
	StartLevel   string `json:"startLevel"`   // last-selected alchemy.xml startLevel

	// MusicDisabled and MusicAudioDir configure mad2relauncher's music/podcast
	// supervision (see music.go in mad2relauncher) when launching via this
	// launcher. MusicDisabled defaults false (music plays) so existing
	// settings.json files with this key absent -- unmarshaled as the zero
	// value -- don't silently lose background music after upgrading.
	MusicDisabled bool   `json:"musicDisabled"`
	MusicAudioDir string `json:"musicAudioDir"` // "" = <repoRoot>/mad2music/audio
}

// LauncherSettings is the whole persisted state of mad2launcher itself.
type LauncherSettings struct {
	ExtraRoots           []string                      `json:"extraRoots"`
	PerInstall           map[string]InstallSettings    `json:"perInstall"`           // keyed by absolute install dir
	PerInstallRandomizer map[string]RandomizerSettings `json:"perInstallRandomizer"` // keyed by absolute install dir

	// PackagesDir overrides the Mod Installer's auto-discovered packages/
	// root (see ResolvePackagesRoot in modinstaller.go) -- "" means fall
	// back to <exeDir>/packages, then <repoRoot>/dist/<platform>.
	PackagesDir string `json:"packagesDir"`
}

// RandomizerSettings mirrors ../mad2rando5's own CLI flags (main.go) --
// this launcher has no config file to edit for it (it's a one-shot CLI
// generator, not a daemon with its own config), so these are the
// launcher's own persisted stand-in for "modifying its configs".
type RandomizerSettings struct {
	Seed          string `json:"seed"`        // int64; "0" = random each run, matching the CLI's own default
	CoinsNeeded   string `json:"coinsNeeded"` // int; "0" = no minimum
	MaxCoins      bool   `json:"maxCoins"`
	SeparatePools bool   `json:"separatePools"`
	MonkeyMatch   bool   `json:"monkeyMatch"`
	CoinMatch     bool   `json:"coinMatch"`
	LockedLevels  string `json:"lockedLevels"` // comma-separated, same format as the CLI's -locked flag
}

func defaultRandomizerSettings() RandomizerSettings {
	return RandomizerSettings{
		Seed:          "0",
		CoinsNeeded:   "0",
		MaxCoins:      false,
		SeparatePools: true,
		MonkeyMatch:   false,
		CoinMatch:     false,
		LockedLevels:  "Dam_Busters,map,title",
	}
}

func defaultInstallSettings(inst Install) InstallSettings {
	return InstallSettings{
		WinePrefix:   defaultWinePrefix(inst),
		ProtonPath:   defaultProtonPathSetting(),
		GameID:       "0",
		InputRes:     "1280x720",
		OutputRes:    "1280x720",
		Scaling:      "FSR",
		FsrSharpness: "2",
		FpsCap:       "60",
		Presentation: "1",
		StartLevel:   "title",
	}
}

// defaultWinePrefix mirrors the justfile: mad2/mad2russia/mad2demo (this
// repo's own three installs) all share ./wineprefixes/mad2. Any other
// discovered install gets its own prefix named after itself, sibling to
// that same wineprefixes/ directory.
func defaultWinePrefix(inst Install) string {
	repoRoot := filepath.Dir(inst.Dir)
	base := filepath.Base(inst.Dir)
	if base == "mad2" || base == "mad2russia" || base == "mad2demo" {
		return filepath.Join(repoRoot, "wineprefixes", "mad2")
	}
	return filepath.Join(repoRoot, "wineprefixes", base)
}

// defaultProtonPathSetting mirrors the justfile's own protonpath default.
// defaultProtonPathSetting is a first-run UI pre-fill only -- ResolveProtonPath
// (launch.go) is what actually matters at launch time and does this same
// broad search itself regardless of what's stored here, falling through if
// this guess doesn't exist. Still worth getting right: showing an install
// found via steamRootCandidates (covers non-~/.local/share/Steam installs,
// e.g. Flatpak) beats always showing the same hardcoded guess that may not
// exist on this machine at all.
func defaultProtonPathSetting() string {
	home, err := os.UserHomeDir()
	if err != nil {
		return ""
	}
	if found, err := ResolveProtonPath(""); err == nil {
		return found
	}
	return filepath.Join(home, ".local/share/Steam/compatibilitytools.d/GE-Proton10-34")
}

func LoadSettings() *LauncherSettings {
	s := &LauncherSettings{PerInstall: map[string]InstallSettings{}, PerInstallRandomizer: map[string]RandomizerSettings{}}
	data, err := os.ReadFile(settingsFileName)
	if err != nil {
		return s
	}
	_ = json.Unmarshal(data, s)
	if s.PerInstall == nil {
		s.PerInstall = map[string]InstallSettings{}
	}
	if s.PerInstallRandomizer == nil {
		s.PerInstallRandomizer = map[string]RandomizerSettings{}
	}
	return s
}

func (s *LauncherSettings) Save() error {
	data, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(settingsFileName, data, 0644)
}

// For gets this install's settings, seeding sensible defaults on first use.
func (s *LauncherSettings) For(inst Install) InstallSettings {
	if v, ok := s.PerInstall[inst.Dir]; ok {
		return v
	}
	return defaultInstallSettings(inst)
}

func (s *LauncherSettings) Set(inst Install, v InstallSettings) {
	s.PerInstall[inst.Dir] = v
}

// RandomizerFor/SetRandomizer mirror For/Set above, for ../mad2rando5's
// flag settings scoped to whichever install its -out level_redirects.txt
// targets.
func (s *LauncherSettings) RandomizerFor(inst Install) RandomizerSettings {
	if v, ok := s.PerInstallRandomizer[inst.Dir]; ok {
		return v
	}
	return defaultRandomizerSettings()
}

func (s *LauncherSettings) SetRandomizer(inst Install, v RandomizerSettings) {
	s.PerInstallRandomizer[inst.Dir] = v
}
