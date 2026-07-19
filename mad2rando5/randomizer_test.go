package main

import (
	"testing"
)

// TestTotalMonkeyCount verifies the level data still sums to the game's
// total of 100 monkeys (a fact previously verified by hand and recorded
// in current.md — this test guards against future data edits breaking it).
func TestTotalMonkeyCount(t *testing.T) {
	total := 0
	for _, l := range AllLevels() {
		total += l.Monkeys
	}
	if total != 100 {
		t.Errorf("expected 100 total monkeys across all levels, got %d", total)
	}
}

// TestDrMelmanSlotIsNotTreatedAsPureMinigame is a regression test for the
// bug recorded in current.md: DrMelman's slot is both IsMinigame and
// IsStory, and dual slots must not trigger minigame-contamination rejection
// or force minigame-mode during simulation, since they're reached via the
// story hub (Waterhole), not the minigame menu.
func TestDrMelmanSlotIsNotTreatedAsPureMinigame(t *testing.T) {
	r := NewRandomizer(Config{Seed: 1})

	// Build an identity mapping (every slot holds its own vanilla level),
	// which is always a legal arrangement and should never be rejected.
	slotToLevel := make(map[string]string)
	var mappings []Mapping
	for _, l := range AllLevels() {
		if l.IsSkipped {
			continue
		}
		slotToLevel[l.Name] = l.Name
		mappings = append(mappings, Mapping{Slot: l.Name, Level: l.Name})
	}

	if !r.validate(mappings) {
		t.Fatal("identity (vanilla) mapping was rejected by validate(), but it must always be legal")
	}

	// Placing an UnbeatableInMinigameMode story level into DrMelman's slot
	// must be allowed, since DrMelman's slot is a dual (story) slot. Uses
	// RitesOfPassage rather than IslandFever: IslandFever is the fixed
	// simulation start, and DrMelman has no transitions of its own, so
	// swapping it in as the START content would correctly break
	// completability now that transitions follow the placed CONTENT
	// rather than the slot — that's a different (valid) rejection, not
	// the dual-slot contamination bug this test targets.
	slotToLevel["DrMelman"] = "RitesOfPassage"
	slotToLevel["RitesOfPassage"] = "DrMelman"
	var swapped []Mapping
	for slot, level := range slotToLevel {
		swapped = append(swapped, Mapping{Slot: slot, Level: level})
	}
	if !r.validate(swapped) {
		t.Fatal("swapping an unbeatable-in-minigame-mode level into DrMelman's dual slot was wrongly rejected")
	}

	// Sanity: the same content placed into an actual pure-minigame slot
	// (no IsStory) must still be rejected.
	if !r.chainHasUnbeatableInMinigameMode("IslandFever", slotToLevel, make(map[string]bool)) {
		t.Fatal("expected IslandFever to be classified as unbeatable in minigame mode")
	}
}

// TestVanillaMappingIsCompletableAndCountsResources checks the simulator
// against known-good vanilla totals: the identity mapping must be able to
// reach Dam_Busters, and should count all 100 monkeys and all coins once
// climb is unlocked (locked coins/monkeys become available).
func TestVanillaMappingIsCompletableAndCountsResources(t *testing.T) {
	r := NewRandomizer(Config{Seed: 1})

	slotToLevel := make(map[string]string)
	for _, l := range AllLevels() {
		if l.IsSkipped {
			continue
		}
		slotToLevel[l.Name] = l.Name
	}

	if !r.isCompletable(slotToLevel) {
		t.Fatal("vanilla identity mapping must be completable (Dam_Busters reachable)")
	}

	state := r.simulate(slotToLevel)
	if !state.abilities["climb"] {
		t.Error("expected climb to be unlocked via RitesOfPassage in vanilla mapping")
	}
	if state.monkeys != 100 {
		t.Errorf("expected all 100 monkeys reachable in vanilla mapping, got %d", state.monkeys)
	}
}

// TestRandomizeSucceedsUnderAllFlagCombinations exercises every
// SeparatePools/MonkeyCountMustMatch/CoinCountMustMatch combination the CLI
// exposes, making sure the retry loop can always find a valid mapping.
func TestRandomizeSucceedsUnderAllFlagCombinations(t *testing.T) {
	bools := []bool{false, true}
	for _, separate := range bools {
		for _, monkeyMatch := range bools {
			for _, coinMatch := range bools {
				cfg := Config{
					Seed:                 7,
					SeparatePools:        separate,
					MonkeyCountMustMatch: monkeyMatch,
					CoinCountMustMatch:   coinMatch,
				}
				r := NewRandomizer(cfg)
				mappings, err := r.Randomize()
				if err != nil {
					t.Errorf("separate=%v monkeyMatch=%v coinMatch=%v: %v", separate, monkeyMatch, coinMatch, err)
					continue
				}
				if !r.validate(mappings) {
					t.Errorf("separate=%v monkeyMatch=%v coinMatch=%v: returned mapping failed validation", separate, monkeyMatch, coinMatch)
				}
			}
		}
	}
}

// TestMixedPoolsWithMonkeyMatchSucceedQuickly is a regression test: mixing
// pools (SeparatePools=false) together with MonkeyCountMustMatch used to
// have only a ~1-in-100,000 chance of finding a valid layout per attempt
// (contamination almost always occurred, since any pure minigame that
// landed in a load-bearing story slot like Waterhole could trap the
// required story levels in minigame mode), making the 5000-attempt retry
// budget fail most of the time. The mixedPoolAssignment partition in
// tryOnce should now make this succeed within a handful of attempts.
func TestMixedPoolsWithMonkeyMatchSucceedQuickly(t *testing.T) {
	cfg := Config{Seed: 3, SeparatePools: false, MonkeyCountMustMatch: true}
	r := NewRandomizer(cfg)
	const attempts = 200
	for i := 0; i < attempts; i++ {
		if _, ok := r.tryOnce(); ok {
			return
		}
	}
	t.Errorf("expected at least one success within %d attempts", attempts)
}

// TestCoinsNeededIsEnforced checks that a CoinsNeeded threshold is actually
// respected by the returned mapping. Forced coin placement guarantees the
// coins themselves are reachable, but completability (Rule 3) depends on
// the exact arrangement of gating content (e.g. what ends up at "penguins2"
// can trip FixThePlane early) and isn't guaranteed by coin-forcing alone,
// so this goes through the full Randomize() retry loop rather than
// asserting first-attempt success.
func TestCoinsNeededIsEnforced(t *testing.T) {
	cfg := Config{Seed: 99, SeparatePools: true, CoinsNeeded: 800}
	r := NewRandomizer(cfg)
	mappings, err := r.Randomize()
	if err != nil {
		t.Fatalf("Randomize with CoinsNeeded=800 failed: %v", err)
	}
	slotToLevel := make(map[string]string)
	for _, m := range mappings {
		slotToLevel[m.Slot] = m.Level
	}
	state := r.simulate(slotToLevel)
	if state.coins < 800 {
		t.Errorf("expected at least 800 accessible coins, got %d", state.coins)
	}
}

// TestMaxCoinsGuaranteesFullCoinTotal checks that asking for the true
// 100%-completion coin total (TotalCoins()) succeeds — this is the "-maxcoins"
// CLI use case, forcing every coin-bearing level into a coin-bearing slot.
func TestMaxCoinsGuaranteesFullCoinTotal(t *testing.T) {
	max := TotalCoins()
	if max != 1600 {
		t.Fatalf("sanity check failed: expected TotalCoins()=1600, got %d (level data changed?)", max)
	}

	cfg := Config{Seed: 5, SeparatePools: true, CoinsNeeded: max}
	r := NewRandomizer(cfg)
	mappings, err := r.Randomize()
	if err != nil {
		t.Fatalf("Randomize with CoinsNeeded=max failed: %v", err)
	}
	slotToLevel := make(map[string]string)
	for _, m := range mappings {
		slotToLevel[m.Slot] = m.Level
	}
	state := r.simulate(slotToLevel)
	if !state.abilities["climb"] {
		t.Fatal("expected climb to be unlocked so all locked coins count toward the max total")
	}
	if state.coins != max {
		t.Errorf("expected all %d coins accessible, got %d", max, state.coins)
	}
}

// TestSeparateFalseMixesStoryAndMinigamePools is a regression test: with
// SeparatePools=false, story levels and minigames used to never actually mix
// because the (now-removed) hasCoins bucket key recreated the exact same
// story/minigame split by accident (every story level has coins>0, every
// pure minigame has 0). This checks that turning -separate off actually
// produces cross-pool placements.
func TestSeparateFalseMixesStoryAndMinigamePools(t *testing.T) {
	levels := make(map[string]Level)
	for _, l := range AllLevels() {
		levels[l.Name] = l
	}

	mixed := false
	for seed := int64(1); seed <= 20 && !mixed; seed++ {
		cfg := Config{Seed: seed, SeparatePools: false}
		r := NewRandomizer(cfg)
		mappings, err := r.Randomize()
		if err != nil {
			t.Fatalf("seed=%d: Randomize failed: %v", seed, err)
		}
		for _, m := range mappings {
			slot := levels[m.Slot]
			placed := levels[m.Level]
			if slot.IsMinigame && !slot.IsStory && placed.IsStory {
				mixed = true
				break
			}
			if slot.IsStory && placed.IsMinigame && !placed.IsStory {
				mixed = true
				break
			}
		}
	}
	if !mixed {
		t.Error("expected -separate=false to place at least one story level in a minigame slot (or vice versa) within 20 seeds, but pools never mixed")
	}
}

// TestCoinInNonCoinSlotContributesZeroCoins checks the soft coin semantics:
// a coin-bearing level dropped into a coinless slot doesn't invalidate the
// mapping, it just contributes 0 coins (the coins are physically
// uncollectable there), per the corrected understanding in the prompt.
func TestCoinInNonCoinSlotContributesZeroCoins(t *testing.T) {
	r := NewRandomizer(Config{Seed: 1})

	slotToLevel := make(map[string]string)
	for _, l := range AllLevels() {
		if l.IsSkipped {
			continue
		}
		slotToLevel[l.Name] = l.Name
	}
	// Move IslandFever's content (100 coins) into a coinless minigame slot,
	// and put that slot's own content (no coins) into IslandFever's slot.
	slotToLevel["IslandFever"] = "Soccer"
	slotToLevel["Soccer"] = "IslandFever"

	state := r.simulate(slotToLevel)

	// IslandFever's 100 coins must not appear anywhere in the total, since
	// its content now sits in the coinless Soccer slot.
	if state.coins > TotalCoins()-100 {
		t.Errorf("expected IslandFever's 100 coins to be dropped once moved into a coinless slot, got total coins=%d", state.coins)
	}
}

// TestLockedLevelsStayPinned checks that Config.LockedLevels forces a slot
// to always keep its own vanilla content across many seeds.
func TestLockedLevelsStayPinned(t *testing.T) {
	for seed := int64(1); seed <= 10; seed++ {
		cfg := Config{Seed: seed, LockedLevels: []string{"Dam_Busters"}}
		r := NewRandomizer(cfg)
		mappings, err := r.Randomize()
		if err != nil {
			t.Fatalf("seed=%d: Randomize failed: %v", seed, err)
		}
		found := false
		for _, m := range mappings {
			if m.Slot == "Dam_Busters" {
				found = true
				if m.Level != "Dam_Busters" || !m.Locked {
					t.Errorf("seed=%d: expected Dam_Busters slot to stay pinned and marked Locked, got Level=%s Locked=%v", seed, m.Level, m.Locked)
				}
			}
		}
		if !found {
			t.Fatalf("seed=%d: Dam_Busters slot missing from mapping", seed)
		}
	}
}

// TestUnlockedDamBustersCanBypassMonkeyGate demonstrates exactly why
// Dam_Busters is locked by default: isCompletable() is content-based, so if
// Dam_Busters' content is free to move into a slot with no monkey
// requirement (like MartyRace, reachable directly from Waterhole), the
// 60-monkey gate on FixThePlane->Dam_Busters becomes bypassable.
func TestUnlockedDamBustersCanBypassMonkeyGate(t *testing.T) {
	r := NewRandomizer(Config{Seed: 1})

	slotToLevel := make(map[string]string)
	for _, l := range AllLevels() {
		if l.IsSkipped {
			continue
		}
		slotToLevel[l.Name] = l.Name
	}
	// Swap Dam_Busters' content into MartyRace's slot (no monkey gate) and
	// vice versa.
	slotToLevel["Dam_Busters"] = "MartyRace"
	slotToLevel["MartyRace"] = "Dam_Busters"

	if !r.isCompletable(slotToLevel) {
		t.Fatal("expected isCompletable to find Dam_Busters' content reachable via MartyRace's ungated slot")
	}
}

// TestVolcanoRaveMonkeysRequireStoryMode is a regression test: VolcanoRave
// has no real "completion" to speak of (it's a dead end), but its 5 monkeys
// are only collectible in story mode — forced into a genuine minigame slot,
// they must not count, the same as any other UnbeatableInMinigameMode level.
func TestVolcanoRaveMonkeysRequireStoryMode(t *testing.T) {
	r := NewRandomizer(Config{Seed: 1})
	slotToLevel := make(map[string]string)
	for _, l := range AllLevels() {
		if l.IsSkipped {
			continue
		}
		slotToLevel[l.Name] = l.Name
	}
	slotToLevel["VolcanoRave"] = "Soccer"
	slotToLevel["Soccer"] = "VolcanoRave"

	state := r.simulate(slotToLevel)
	if state.monkeys != 95 {
		t.Errorf("expected VolcanoRave's 5 monkeys to be lost when forced into a minigame slot, got %d total monkeys (want 95)", state.monkeys)
	}
}

// TestDutyFreeNeverGetsMinigameContent is a regression test for the
// title→DutyFree→pause-menu-map path: if DutyFree's slot held minigame
// content, there would be no way to reach the map while both title and
// DutyFree are stuck in minigame mode. DutyFree.RequireStorySlot forces its
// slot to always stay story-classified, across every SeparatePools/mixed
// configuration.
func TestDutyFreeNeverGetsMinigameContent(t *testing.T) {
	for seed := int64(1); seed <= 200; seed++ {
		cfg := Config{Seed: seed, SeparatePools: false, LockedLevels: []string{"Dam_Busters", "map", "title"}}
		r := NewRandomizer(cfg)
		mappings, err := r.Randomize()
		if err != nil {
			t.Fatalf("seed=%d: Randomize failed: %v", seed, err)
		}
		for _, m := range mappings {
			if m.Slot != "DutyFree" {
				continue
			}
			placed := r.levels[m.Level]
			if placed.IsMinigame && !placed.IsStory {
				t.Fatalf("seed=%d: DutyFree slot got pure minigame content %s", seed, m.Level)
			}
		}
	}
}

// TestDefaultLocksIncludeMapAndTitle checks that main.go's -locked default
// ("Dam_Busters,map,title") corresponds to valid, non-skipped level names —
// a typo here would silently do nothing since main.go only validates
// whatever string is actually passed in.
func TestDefaultLocksIncludeMapAndTitle(t *testing.T) {
	levels := make(map[string]Level)
	for _, l := range AllLevels() {
		levels[l.Name] = l
	}
	for _, name := range []string{"Dam_Busters", "map", "title"} {
		l, ok := levels[name]
		if !ok {
			t.Errorf("default-locked level %q does not exist", name)
			continue
		}
		if l.IsSkipped {
			t.Errorf("default-locked level %q is marked IsSkipped, so it could never be a real slot to lock", name)
		}
	}
}

// TestTransitionsFollowPlacedContentNotSlot is a regression test for a
// reported bug: walkSlot used to follow the SLOT's own vanilla transitions
// instead of the transitions of whatever LEVEL was actually placed there.
// In the real game, a level's exit script is baked into that level file and
// travels with it wherever it's shuffled — e.g. if "penguins" content is
// placed in the IslandFever slot, finishing it sends the player to whatever
// occupies the "penguins2" slot (penguins' own hardcoded target), not to
// "Prepare2Launch" (IslandFever slot's own vanilla target).
//
// This also reproduces the exact scenario reported: penguins2 holds
// FixThePlane, which permanently blocks Waterhole → penguins (per
// files.txt) once played, stranding whatever's in the "penguins" slot
// (here, Morts_Adventure's 100 coins) — the simulator must reflect that
// loss, and validate() must reject this arrangement under -maxcoins.
func TestTransitionsFollowPlacedContentNotSlot(t *testing.T) {
	r := NewRandomizer(Config{Seed: 1, LockedLevels: []string{"Dam_Busters", "map", "title"}})

	slotToLevel := make(map[string]string)
	for _, l := range AllLevels() {
		if l.IsSkipped {
			continue
		}
		slotToLevel[l.Name] = l.Name
	}
	slotToLevel["IslandFever"] = "penguins"
	slotToLevel["penguins"] = "Morts_Adventure"
	slotToLevel["penguins2"] = "FixThePlane"
	slotToLevel["FixThePlane"] = "IslandFever"
	slotToLevel["Morts_Adventure"] = "penguins2"

	state := r.simulate(slotToLevel)
	if state.visited["penguins"] {
		t.Error("expected the 'penguins' slot to be stranded (unreachable) once FixThePlane is played via penguins2")
	}
	if want := TotalCoins() - 100; state.coins != want {
		t.Errorf("expected Morts_Adventure's 100 coins to be lost (want %d), got %d", want, state.coins)
	}

	var mappings []Mapping
	for slot, level := range slotToLevel {
		mappings = append(mappings, Mapping{Slot: slot, Level: level})
	}
	strict := NewRandomizer(Config{Seed: 1, CoinsNeeded: TotalCoins(), LockedLevels: []string{"Dam_Busters", "map", "title"}})
	if strict.validate(mappings) {
		t.Error("expected validate() to reject this arrangement under -maxcoins, since not all coins are reachable")
	}
}

// TestWaterholeToConvoyChaseGate is a regression test for a missing rule
// from files.txt: Waterhole → ConvoyChase should only be available once
// ConvoyChase has already been reached via its own chain
// (penguins → penguins2 → ConvoyChase), or once FixThePlane has been
// played. Previously this transition was unconditionally allowed.
func TestWaterholeToConvoyChaseGate(t *testing.T) {
	r := NewRandomizer(Config{Seed: 1})
	state := &progressState{abilities: make(map[string]bool), visited: make(map[string]bool)}
	reachedContent := make(map[string]bool)

	if r.canFollowTransition("Waterhole", "ConvoyChase", state, reachedContent) {
		t.Error("expected Waterhole -> ConvoyChase to be blocked before ConvoyChase or FixThePlane has been reached")
	}
	reachedContent["FixThePlane"] = true
	if !r.canFollowTransition("Waterhole", "ConvoyChase", state, reachedContent) {
		t.Error("expected Waterhole -> ConvoyChase to open up once FixThePlane has been played")
	}
}
