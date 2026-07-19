package main

// Level holds all the metadata for a single game level/slot.
type Level struct {
	Name                     string
	IsMinigame               bool
	IsStory                  bool // true if it has story-mode gameplay (transitions, coins, monkeys, etc.)
	Coins                    int
	Monkeys                  int
	LockedCoins              int // coins behind a gate (climb, monkey count, duty free purchase, etc.)
	LockedMonkeys            int
	Transitions              []string // levels this one transitions to
	UnbeatableInMinigameMode bool
	UnlocksAbility           string   // e.g. "climb", "monkeys_everywhere", "volcano_rave"
	Requirements             []string // abilities/conditions required to enter or fully complete
	IsSkipped                bool     // levels we never randomize
	RequireStorySlot         bool     // true if this SLOT must never hold minigame content, even if it's otherwise a safe dead end (e.g. DutyFree, which gates access to the pause-menu map)
	// Chains: if this level transitions into another, that downstream level
	// inherits minigame-mode from whatever slot this level is placed into.
}

// AllLevels returns the full catalogue of levels parsed from files.txt.
func AllLevels() []Level {
	return []Level{
		// ── Pure Minigames ────────────────────────────────────────
		{
			Name:       "animal_chess",
			IsMinigame: true,
		},
		{
			Name:       "Card_Match_Game",
			IsMinigame: true,
		},
		{
			Name:       "DivingLocation_IslandFever",
			IsMinigame: true,
		},
		{
			Name:       "DivingLocation_PrepareToLaunch",
			IsMinigame: true,
		},
		{
			Name:       "DivingLocation_RitesOfPassage",
			IsMinigame: true,
		},
		{
			Name:       "DivingLocation_Waterhole",
			IsMinigame: true,
		},
		{
			Name:        "Golf_minigame",
			IsMinigame:  true,
			Transitions: []string{"golf_3holes"},
		},
		{
			Name:        "golf_3holes",
			IsMinigame:  true,
			Transitions: []string{"golf_baggagecheck"},
		},
		{
			Name:        "golf_baggagecheck",
			IsMinigame:  true,
			Transitions: []string{"golf_cratercrossing"},
		},
		{
			Name:        "golf_cratercrossing",
			IsMinigame:  true,
			Transitions: []string{"golf_crossedpaths"},
		},
		{
			Name:        "golf_crossedpaths",
			IsMinigame:  true,
			Transitions: []string{"golf_foosaBall"},
		},
		{
			Name:        "golf_foosaBall",
			IsMinigame:  true,
			Transitions: []string{"golf_ravenousRhinos"},
		},
		{
			Name:        "golf_ravenousRhinos",
			IsMinigame:  true,
			Transitions: []string{"golf_junkyard"},
		},
		{
			Name:        "golf_junkyard",
			IsMinigame:  true,
			Transitions: []string{"golf_maze"},
		},
		{
			Name:         "golf_maze",
			IsMinigame:   true,
			Transitions:  []string{"golf_lovelylumps"}, // requires duty free purchase (20 coins)
			Requirements: []string{"duty_free_bonus_holes"},
		},
		{
			Name:         "golf_lovelylumps",
			IsMinigame:   true,
			Transitions:  []string{"golf_cake"}, // requires duty free purchase (20 coins)
			Requirements: []string{"duty_free_bonus_holes"},
		},
		{
			Name:        "golf_cake",
			IsMinigame:  true,
			Transitions: []string{"golf_targetTree"},
		},
		{
			Name:       "golf_targetTree",
			IsMinigame: true,
		},
		{
			Name:       "HungryHippo",
			IsMinigame: true,
		},
		{
			Name:       "Minigame_Diving_Location_Menu",
			IsMinigame: true,
			Transitions: []string{
				"DivingLocation_IslandFever",
				"DivingLocation_PrepareToLaunch",
				"DivingLocation_RitesOfPassage",
				"DivingLocation_Waterhole",
			},
		},
		{
			Name:       "Minigame_HotDurian",
			IsMinigame: true,
		},
		{
			Name:       "RoP_MusicalChairs",
			IsMinigame: true,
		},
		{
			Name:       "Soccer",
			IsMinigame: true,
		},

		// ── Both Minigame and Story ──────────────────────────────
		{
			Name:                     "DrMelman",
			IsMinigame:               true,
			IsStory:                  true,
			Coins:                    52,
			Monkeys:                  2,
			UnbeatableInMinigameMode: true,
		},

		// ── Story Levels ─────────────────────────────────────────
		{
			Name:          "BraveNewWild",
			IsStory:       true,
			Coins:         100,
			LockedCoins:   11, // locked behind climb
			Monkeys:       15,
			LockedMonkeys: 15, // all locked behind climb
			Transitions:   []string{"Waterhole"},
		},
		{
			Name:        "ConvoyChase",
			IsStory:     true,
			Coins:       100,
			Monkeys:     10,
			Transitions: []string{"Waterhole"},
		},
		{
			Name:                     "Dam_Busters",
			IsStory:                  true,
			Coins:                    100,
			Transitions:              []string{"Credits"},
			UnbeatableInMinigameMode: true,
		},
		{
			Name:                     "FixThePlane",
			IsStory:                  true,
			Coins:                    300,
			LockedCoins:              6, // locked behind 10 monkeys
			UnbeatableInMinigameMode: true,
			UnlocksAbility:           "monkeys_everywhere",
			Transitions:              []string{"Dam_Busters", "Waterhole"}, // Dam_Busters requires 60 monkeys; can also return to the hub
			Requirements:             []string{"10_monkeys_for_coins", "60_monkeys_for_dam_busters"},
		},
		{
			Name:                     "IslandFever",
			IsStory:                  true,
			Coins:                    100,
			Transitions:              []string{"Prepare2Launch"},
			UnbeatableInMinigameMode: true,
		},
		{
			Name:        "Morts_Adventure",
			IsStory:     true,
			Coins:       100,
			Transitions: []string{"Waterhole"},
		},
		{
			Name:        "penguins",
			IsStory:     true,
			Coins:       85,
			Transitions: []string{"penguins2"},
		},
		{
			Name:        "penguins2",
			IsStory:     true,
			Coins:       15,
			Transitions: []string{"ConvoyChase"},
		},
		{
			Name:        "Prepare2Launch",
			IsStory:     true,
			Coins:       15,
			Transitions: []string{"Prepare2Launch_Plane"},
		},
		{
			Name:        "Prepare2Launch_Plane",
			IsStory:     true,
			Coins:       85,
			Transitions: []string{"BraveNewWild"},
		},
		{
			Name:                     "RitesOfPassage",
			IsStory:                  true,
			Coins:                    100,
			Monkeys:                  15,
			UnbeatableInMinigameMode: true,
			UnlocksAbility:           "climb",
			Transitions:              []string{"Waterhole"}, // can return to the hub after
		},
		{
			Name:          "VolcanoRave",
			IsStory:       true,
			Coins:         100,
			Monkeys:       5,
			LockedMonkeys: 3, // locked behind duty free songs (10 coins each)
			// Not "unbeatable" in the sense of being unplayable, but its 5
			// monkeys are only collectible in story mode — in minigame mode
			// they're inaccessible, so it's treated the same way.
			UnbeatableInMinigameMode: true,
			Transitions:              []string{"map"},
			Requirements:             []string{"wooing_gloria_complete"},
		},
		{
			Name:         "Watercaves",
			IsStory:      true,
			Coins:        100,
			Monkeys:      10,
			Transitions:  []string{"Waterhole", "Wooing_Gloria"},
			Requirements: []string{"rites_entered"},
		},
		{
			Name:          "Waterhole",
			IsStory:       true,
			Coins:         120,
			LockedCoins:   60, // 50% behind climb (temporary)
			Monkeys:       25,
			LockedMonkeys: 12, // 50% behind climb (temporary)
			Transitions: []string{
				"BraveNewWild",
				"FixThePlane",
				"penguins",    // only if haven't entered FixThePlane
				"ConvoyChase", // only if completed or been to FixThePlane
				"MartyRace",
				"DrMelman",
				"RitesOfPassage",
				"Watercaves",  // requires rites entered
				"VolcanoRave", // requires WooingGloria complete
				"Morts_Adventure",
			},
		},
		{
			Name:           "Wooing_Gloria",
			IsStory:        true,
			Coins:          100,
			Monkeys:        15,
			Transitions:    []string{"Waterhole"},
			UnlocksAbility: "volcano_rave",
		},

		// ── Menu / Utility (randomizable, but see notes below) ───
		{
			Name:    "Credits",
			IsStory: true,
			// Dead end: rolls after Dam_Busters, nothing further.
		},
		{
			Name:             "DutyFree",
			IsStory:          true,
			RequireStorySlot: true,
			// Dead-end shop reached from title. Forced to always be a story
			// slot: if it and title both ended up holding minigame content,
			// there would be no way to reach the pause-menu map.
		},
		{
			Name:    "map",
			IsStory: true,
			// Accessed from the pause menu outside minigame mode; a dead
			// end in this transition graph. Locked by default (see
			// Config.LockedLevels / -locked) since it's the primary way
			// to navigate mid-run.
		},
		{
			Name:        "MartyRace",
			IsStory:     true,
			Coins:       28,
			Monkeys:     3,
			Transitions: []string{"Waterhole"}, // can return to the hub after
		},
		{
			Name:    "title",
			IsStory: true,
			// Also "continues" to whichever story slot was last visited,
			// which isn't a fixed slot name so isn't modeled here. Locked
			// by default (see Config.LockedLevels / -locked).
			Transitions: []string{"DutyFree"},
		},
	}
}
