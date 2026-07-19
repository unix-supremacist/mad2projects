package main

import (
	"fmt"
	"math/rand"
	"os"
	"strings"
	"time"
)

// Config holds the user-facing randomizer settings.
type Config struct {
	Seed                 int64
	CoinsNeeded          int  // just enough coin-bearing levels are forced into coin-bearing slots to reach this total; 0 = no minimum
	SeparatePools        bool // true = minigames swap with minigames, story with story
	MonkeyCountMustMatch bool
	CoinCountMustMatch   bool
	LockedLevels         []string // level names whose slot is pinned to its own vanilla content (never shuffled)
}

// Mapping represents a single slot→level assignment.
// Slot is the original level name (the "address" in the game).
// Level is the level content placed into that slot.
type Mapping struct {
	Slot   string
	Level  string
	Locked bool // true if this slot was pinned via Config.LockedLevels rather than shuffled
}

// TotalCoins returns the sum of every level's coins (including locked
// portions), i.e. the true 100%-completion coin total.
func TotalCoins() int {
	total := 0
	for _, l := range AllLevels() {
		total += l.Coins
	}
	return total
}

// Randomizer performs the level-shuffle logic.
type Randomizer struct {
	cfg    Config
	rng    *rand.Rand
	levels map[string]Level // name → Level
}

// NewRandomizer builds a ready-to-use randomizer.
func NewRandomizer(cfg Config) *Randomizer {
	if cfg.Seed == 0 {
		cfg.Seed = time.Now().UnixNano()
	}
	r := &Randomizer{
		cfg:    cfg,
		rng:    rand.New(rand.NewSource(cfg.Seed)),
		levels: make(map[string]Level),
	}
	for _, l := range AllLevels() {
		r.levels[l.Name] = l
	}
	return r
}

// Randomize produces a valid mapping or returns an error if no valid
// arrangement can be found within the retry budget.
func (r *Randomizer) Randomize() ([]Mapping, error) {
	const maxAttempts = 5000
	for attempt := 0; attempt < maxAttempts; attempt++ {
		mappings, ok := r.tryOnce()
		if ok {
			return mappings, nil
		}
	}
	return nil, fmt.Errorf("failed to find a valid randomization after %d attempts – try loosening constraints", maxAttempts)
}

// lockedSet returns the configured locked level names as a set.
func (r *Randomizer) lockedSet() map[string]bool {
	set := make(map[string]bool, len(r.cfg.LockedLevels))
	for _, name := range r.cfg.LockedLevels {
		set[name] = true
	}
	return set
}

// pickForcedCoinPlacements guarantees `target` accessible coins is
// structurally achievable instead of leaving it to chance: it greedily
// reserves the fewest possible coin-bearing levels (largest coin values
// first, so as many levels as possible stay free to mix) into coin-capable
// slots until the running total meets the target. Everything not reserved
// here is left in freeNames for normal bucket shuffling, so leftover
// coin-bearing levels can still land in non-coin slots (losing their coins)
// for extra variety, and non-coin levels can still fill leftover coin slots.
//
// slotPool/levelPool are the mixed-pool classification from
// mixedPoolAssignment (nil when SeparatePools is true, where there's no
// such split to respect): a forced pair must stay within the same pool, or
// it would silently steal a slot reserved for the other pool and reopen
// the exact contamination risk that partition exists to prevent.
func (r *Randomizer) pickForcedCoinPlacements(freeNames []string, target int, slotPool, levelPool map[string]string) map[string]string {
	forcedSlot := make(map[string]string)
	forcedUsed := make(map[string]bool)

	var coinLevels []string
	for _, name := range freeNames {
		if r.levels[name].Coins > 0 {
			coinLevels = append(coinLevels, name)
		}
	}
	coinSlots := make([]string, len(coinLevels))
	copy(coinSlots, coinLevels) // slot capacity comes from the same catalogue of names

	// Shuffle first so ties in coin value (and which slot each forced level
	// lands in) still vary by seed, then sort by value descending so the
	// greedy pick needs as few levels as possible.
	r.rng.Shuffle(len(coinLevels), func(i, j int) { coinLevels[i], coinLevels[j] = coinLevels[j], coinLevels[i] })
	r.rng.Shuffle(len(coinSlots), func(i, j int) { coinSlots[i], coinSlots[j] = coinSlots[j], coinSlots[i] })
	sortDescByCoins := func(names []string) {
		for i := 1; i < len(names); i++ {
			for j := i; j > 0 && r.levels[names[j]].Coins > r.levels[names[j-1]].Coins; j-- {
				names[j], names[j-1] = names[j-1], names[j]
			}
		}
	}
	sortDescByCoins(coinLevels)

	usedSlots := make(map[string]bool)
	total := 0
	for _, name := range coinLevels {
		if total >= target {
			break
		}
		l := r.levels[name]

		slot := ""
		for _, s := range coinSlots {
			if usedSlots[s] {
				continue
			}
			if r.cfg.CoinCountMustMatch && r.levels[s].Coins != l.Coins {
				continue
			}
			if r.cfg.MonkeyCountMustMatch && r.levels[s].Monkeys != l.Monkeys {
				continue
			}
			if slotPool != nil && slotPool[s] != levelPool[name] {
				continue
			}
			slot = s
			break
		}
		if slot == "" {
			continue // no compatible slot left over; leave this level for the free pool
		}

		usedSlots[slot] = true
		forcedSlot[slot] = name
		forcedUsed[name] = true
		total += l.Coins
	}

	return forcedSlot
}

// hasLiveTransitions reports whether a level's own hardcoded Transitions
// lead anywhere randomizable — i.e. the list isn't empty and doesn't solely
// target skipped/terminal slots like Credits or map.
func (r *Randomizer) hasLiveTransitions(name string) bool {
	for _, t := range r.levels[name].Transitions {
		if target, ok := r.levels[t]; ok && !target.IsSkipped {
			return true
		}
	}
	return false
}

// poolSubKey groups names for mixedPoolAssignment along the same
// monkeys/coins dimensions that tryOnce's own bucketKey uses. A slot and a
// level can only ever be paired if they share a bucketKey, so freeing a
// terminal story slot into the flexible pool only helps if some flexible
// LEVEL with the exact same sub-key can actually land there — computing
// the flexible/dangerous split per sub-key (instead of globally) keeps
// slot/level counts balanced within every bucket the shuffle will actually
// use, not just in aggregate.
type poolSubKey struct {
	monkeys int
	coins   int
}

func (r *Randomizer) poolSubKeyOf(name string) poolSubKey {
	l := r.levels[name]
	var k poolSubKey
	if r.cfg.MonkeyCountMustMatch {
		k.monkeys = l.Monkeys
	}
	if r.cfg.CoinCountMustMatch {
		k.coins = l.Coins
	}
	return k
}

// mixedPoolAssignment partitions the free names into "flexible" (safe for
// ANY slot, including a genuine minigame slot) and "dangerous" (must stay
// on a story slot) — separately for their role as a SLOT and as a LEVEL,
// since a name's two roles can land in different pools. This only matters
// when SeparatePools is false; with pools mixed, letting anything land
// anywhere makes Rule 2 (minigame-chain contamination) fail almost every
// attempt (empirically ~1 in 18,000, or far worse once other constraints
// like MonkeyCountMustMatch shrink the search space further), so instead
// of hoping retries stumble onto a safe layout, this constructs one.
//
// Within each sub-key (see poolSubKey), names split into three roles:
//   - "hard flexible" LEVELS: pure minigames. Their own transitions only
//     ever target other minigame slots, so they must land on a minigame
//     slot or a freed terminal story slot.
//   - "hard dangerous" LEVELS/SLOTS: every UnbeatableInMinigameMode level,
//     plus any story level/slot whose own transitions chain further into
//     the story graph (e.g. anything reaching Waterhole), plus anything
//     marked RequireStorySlot (DutyFree). These must stay on a story slot.
//   - "soft" names: non-unbeatable story levels with no live transitions
//     of their own (e.g. Credits) — these can't break anything either way,
//     so they swing to whichever pool needs the capacity.
//
// Terminal story slots (no live transitions) are the flexible pool's only
// source of extra capacity beyond the dedicated minigame slots; enough of
// them are freed, per sub-key, to fit every hard-flexible level plus as
// many soft levels as will fit, maximizing mixing while keeping every
// sub-key's slot/level counts exactly equal.
func (r *Randomizer) mixedPoolAssignment(freeNames []string) (slotPool, levelPool map[string]string) {
	slotPool = make(map[string]string, len(freeNames))
	levelPool = make(map[string]string, len(freeNames))

	type group struct {
		minigameSlots       []string
		terminalStorySlots  []string
		liveStorySlots      []string
		hardFlexibleLevels  []string
		hardDangerousLevels []string
		softLevels          []string
	}
	groups := make(map[poolSubKey]*group)
	for _, name := range freeNames {
		l := r.levels[name]
		k := r.poolSubKeyOf(name)
		g, ok := groups[k]
		if !ok {
			g = &group{}
			groups[k] = g
		}

		switch {
		case l.IsMinigame && !l.IsStory:
			g.minigameSlots = append(g.minigameSlots, name)
			g.hardFlexibleLevels = append(g.hardFlexibleLevels, name)
		case l.IsStory && (r.hasLiveTransitions(name) || l.RequireStorySlot || l.UnbeatableInMinigameMode):
			if r.hasLiveTransitions(name) || l.RequireStorySlot {
				g.liveStorySlots = append(g.liveStorySlots, name)
			} else {
				g.terminalStorySlots = append(g.terminalStorySlots, name)
			}
			g.hardDangerousLevels = append(g.hardDangerousLevels, name)
		case l.IsStory:
			g.terminalStorySlots = append(g.terminalStorySlots, name)
			g.softLevels = append(g.softLevels, name)
		}
	}

	for _, g := range groups {
		r.rng.Shuffle(len(g.terminalStorySlots), func(i, j int) {
			g.terminalStorySlots[i], g.terminalStorySlots[j] = g.terminalStorySlots[j], g.terminalStorySlots[i]
		})
		r.rng.Shuffle(len(g.softLevels), func(i, j int) {
			g.softLevels[i], g.softLevels[j] = g.softLevels[j], g.softLevels[i]
		})

		// Free as many terminal slots as the hard-flexible levels plus all
		// soft levels could use, capped by what's actually available.
		freed := len(g.hardFlexibleLevels) + len(g.softLevels) - len(g.minigameSlots)
		if freed < 0 {
			freed = 0
		}
		if freed > len(g.terminalStorySlots) {
			freed = len(g.terminalStorySlots)
		}

		for _, name := range g.minigameSlots {
			slotPool[name] = "flexible"
		}
		for i, name := range g.terminalStorySlots {
			if i < freed {
				slotPool[name] = "flexible"
			} else {
				slotPool[name] = "dangerous"
			}
		}
		for _, name := range g.liveStorySlots {
			slotPool[name] = "dangerous"
		}

		for _, name := range g.hardFlexibleLevels {
			levelPool[name] = "flexible"
		}
		for _, name := range g.hardDangerousLevels {
			levelPool[name] = "dangerous"
		}
		softToFlexible := (len(g.minigameSlots) + freed) - len(g.hardFlexibleLevels)
		if softToFlexible < 0 {
			softToFlexible = 0
		}
		if softToFlexible > len(g.softLevels) {
			softToFlexible = len(g.softLevels)
		}
		for i, name := range g.softLevels {
			if i < softToFlexible {
				levelPool[name] = "flexible"
			} else {
				levelPool[name] = "dangerous"
			}
		}
	}

	return slotPool, levelPool
}

// tryOnce attempts a single shuffle and validates all constraints.
func (r *Randomizer) tryOnce() ([]Mapping, bool) {
	// bucketKey determines which levels can swap with each other.
	// Levels sharing the same key are interchangeable.
	type bucketKey struct {
		pool    string // "story"/"minigame" (SeparatePools) or "flexible"/"dangerous" (mixed)
		monkeys int    // only used when MonkeyCountMustMatch is true
		coins   int    // only used when CoinCountMustMatch is true
	}

	locked := r.lockedSet()

	var mappings []Mapping
	var freeNames []string
	for _, l := range AllLevels() {
		if l.IsSkipped {
			continue
		}
		if locked[l.Name] {
			mappings = append(mappings, Mapping{Slot: l.Name, Level: l.Name, Locked: true})
			continue
		}
		freeNames = append(freeNames, l.Name)
	}

	// When pools are mixed, partition into "flexible"/"dangerous" pools so
	// Rule 2 (minigame-chain contamination) can never fire by construction
	// — see mixedPoolAssignment. Under SeparatePools=true, the classic
	// story/minigame split already keeps that concern apart entirely.
	var slotPool, levelPool map[string]string
	if !r.cfg.SeparatePools {
		slotPool, levelPool = r.mixedPoolAssignment(freeNames)
	}

	// Reserve just enough coin-bearing levels into coin-capable slots to
	// make CoinsNeeded achievable by construction, rather than hoping a
	// free-for-all shuffle stumbles into it.
	forcedSlot := make(map[string]string)
	forcedUsed := make(map[string]bool)
	if r.cfg.CoinsNeeded > 0 {
		forcedSlot = r.pickForcedCoinPlacements(freeNames, r.cfg.CoinsNeeded, slotPool, levelPool)
		for _, level := range forcedSlot {
			forcedUsed[level] = true
		}
	}

	for slot, level := range forcedSlot {
		mappings = append(mappings, Mapping{Slot: slot, Level: level})
	}

	// Gather the remaining free slots/levels into buckets. A name can be
	// "used up" as a slot (forcedSlot key) independently of being used up
	// as a level (forcedUsed) — e.g. IslandFever's LEVEL content may have
	// been force-placed elsewhere while the IslandFever SLOT still needs
	// filling from the bucket pool, so these two checks must stay separate
	// rather than skipping a name from both lists together.
	bucketSlots := make(map[bucketKey][]string)
	bucketLevels := make(map[bucketKey][]string)

	for _, name := range freeNames {
		l := r.levels[name]

		var slotPoolName, levelPoolName string
		if r.cfg.SeparatePools {
			if l.IsStory {
				slotPoolName, levelPoolName = "story", "story"
			} else if l.IsMinigame {
				slotPoolName, levelPoolName = "minigame", "minigame"
			} else {
				continue
			}
		} else {
			slotPoolName, levelPoolName = slotPool[name], levelPool[name]
		}

		slotKey := bucketKey{pool: slotPoolName}
		levelKey := bucketKey{pool: levelPoolName}
		if r.cfg.MonkeyCountMustMatch {
			slotKey.monkeys, levelKey.monkeys = l.Monkeys, l.Monkeys
		}
		if r.cfg.CoinCountMustMatch {
			slotKey.coins, levelKey.coins = l.Coins, l.Coins
		}

		if _, isForcedSlot := forcedSlot[name]; !isForcedSlot {
			bucketSlots[slotKey] = append(bucketSlots[slotKey], name)
		}
		if !forcedUsed[name] {
			bucketLevels[levelKey] = append(bucketLevels[levelKey], name)
		}
	}

	// Defensive check: forced placements are only guaranteed to keep a
	// bucket's slot/level counts balanced when they match on every active
	// key dimension (pool/monkeys/coins). If some future forcing rule
	// doesn't uphold that, fail this attempt cleanly instead of silently
	// dropping or misassigning slots.
	for key, slots := range bucketSlots {
		if len(slots) != len(bucketLevels[key]) {
			return nil, false
		}
	}

	// Shuffle each bucket independently.
	for key := range bucketLevels {
		levels := bucketLevels[key]
		r.rng.Shuffle(len(levels), func(i, j int) {
			levels[i], levels[j] = levels[j], levels[i]
		})
	}

	// Build the mapping from buckets.
	for key, slots := range bucketSlots {
		levels := bucketLevels[key]
		for i, slot := range slots {
			mappings = append(mappings, Mapping{Slot: slot, Level: levels[i]})
		}
	}

	// Validate remaining constraints (minigame contamination, accessible coins, etc.).
	if !r.validate(mappings) {
		return nil, false
	}
	return mappings, true
}

// validate checks every constraint against a candidate mapping.
func (r *Randomizer) validate(mappings []Mapping) bool {
	// Build lookup: slot → level-placed-there, and level → slot-it's-in.
	slotToLevel := make(map[string]string)
	levelToSlot := make(map[string]string)
	for _, m := range mappings {
		slotToLevel[m.Slot] = m.Level
		levelToSlot[m.Level] = m.Slot
	}

	// ── Rule 1: Monkey/coin count matching (safety net) ──
	// Bucketing already enforces this when the flags are on; this is a
	// belt-and-suspenders check for placements built outside the normal
	// bucket path (e.g. forced coin placements).
	if r.cfg.MonkeyCountMustMatch {
		for _, m := range mappings {
			if r.levels[m.Level].Monkeys != r.levels[m.Slot].Monkeys {
				return false
			}
		}
	}
	if r.cfg.CoinCountMustMatch {
		for _, m := range mappings {
			if r.levels[m.Level].Coins != r.levels[m.Slot].Coins {
				return false
			}
		}
	}

	// ── Rule 2: Minigame-chain contamination ──
	// If a slot is a minigame slot, the placed level runs in minigame mode.
	// Slots that are ALSO story slots (e.g. DrMelman) are reached via the
	// story hub, not the minigame menu, so they don't force minigame mode.
	for _, m := range mappings {
		slotDef := r.levels[m.Slot]
		if !slotDef.IsMinigame || slotDef.IsStory {
			continue
		}
		if r.chainHasUnbeatableInMinigameMode(m.Level, slotToLevel, make(map[string]bool)) {
			return false
		}
	}

	// If a PURE minigame (not story) is placed in a story slot, downstream
	// transitions from that slot are stuck in minigame mode.
	for _, m := range mappings {
		placedLevel := r.levels[m.Level]
		if !placedLevel.IsMinigame || placedLevel.IsStory {
			continue
		}
		slotDef := r.levels[m.Slot]
		for _, trans := range slotDef.Transitions {
			downstream, ok := slotToLevel[trans]
			if !ok {
				continue
			}
			if r.chainHasUnbeatableInMinigameMode(downstream, slotToLevel, make(map[string]bool)) {
				return false
			}
		}
	}

	// ── Rule 3: Completability — can we reach Dam_Busters? ──
	if !r.isCompletable(slotToLevel) {
		return false
	}

	// ── Rule 4: Minimum accessible coins ──
	if r.cfg.CoinsNeeded > 0 {
		sim := r.simulate(slotToLevel)
		if sim.coins < r.cfg.CoinsNeeded {
			return false
		}
	}

	return true
}

// chainHasUnbeatableInMinigameMode walks the transition graph from a level
// and returns true if any reachable level (including the start) is unbeatable
// in minigame mode.
func (r *Randomizer) chainHasUnbeatableInMinigameMode(levelName string, slotToLevel map[string]string, visited map[string]bool) bool {
	if visited[levelName] {
		return false
	}
	visited[levelName] = true

	lev, ok := r.levels[levelName]
	if !ok {
		return false
	}
	if lev.UnbeatableInMinigameMode {
		return true
	}

	// Transitions reference slot names; check the level placed in each.
	for _, transSlot := range lev.Transitions {
		downstream, ok := slotToLevel[transSlot]
		if !ok {
			continue
		}
		if r.chainHasUnbeatableInMinigameMode(downstream, slotToLevel, visited) {
			return true
		}
	}
	return false
}

// progressState tracks what the player has accumulated during a simulated
// playthrough.
type progressState struct {
	monkeys   int
	coins     int
	abilities map[string]bool // unlocked abilities (climb, monkeys_everywhere, etc.)
	visited   map[string]bool // slots reachable during the final pass
}

// simulate does a full progression walk: starts at IslandFever, follows
// each visited LEVEL's own hardcoded transitions (not the slot's — a
// level's exit script travels with its content wherever it's shuffled to),
// unlocks abilities, and iterates until no new slots are reachable
// (fixed-point). Resources are computed at the end using the final
// ability set.
func (r *Randomizer) simulate(slotToLevel map[string]string) progressState {
	state := progressState{
		abilities: make(map[string]bool),
		visited:   make(map[string]bool),
	}
	reachable := make(map[string]bool)      // all slots ever reached
	reachedContent := make(map[string]bool) // all LEVELS ever played, wherever they landed

	// Fixed-point: expand reachability until stable.
	// Each pass resets the per-walk cycle guard but preserves abilities
	// and cumulative reachability.
	changed := true
	for changed {
		changed = false
		state.visited = make(map[string]bool)
		r.walkSlot("IslandFever", slotToLevel, &state, reachable, reachedContent, &changed, false)
	}

	// Compute final resource totals from all reachable slots.
	// Reset first since walkSlot sets temporary monkey counts for gating.
	state.monkeys = 0
	state.coins = 0
	for slot := range reachable {
		levelName, ok := slotToLevel[slot]
		if !ok {
			continue
		}
		lev := r.levels[levelName]
		slotDef := r.levels[slot]

		// Monkeys.
		monkeys := lev.Monkeys - lev.LockedMonkeys
		if state.abilities["climb"] {
			monkeys = lev.Monkeys
		}
		state.monkeys += monkeys

		// Coins only register if the SLOT itself has coin-tracking; a
		// level's own coins are physically uncollectable/untracked when
		// dropped into a slot that never had any (see instruction.txt).
		if slotDef.Coins > 0 {
			coins := lev.Coins - lev.LockedCoins
			if state.abilities["climb"] {
				coins = lev.Coins
			}
			state.coins += coins
		}
	}

	return state
}

// walkSlot recursively explores reachable slots and unlocks abilities.
// It does NOT collect resources — that happens after the fixed-point.
func (r *Randomizer) walkSlot(slot string, slotToLevel map[string]string, state *progressState, reachable, reachedContent map[string]bool, changed *bool, inMinigameChain bool) {
	// Per-pass cycle guard.
	if state.visited[slot] {
		return
	}
	state.visited[slot] = true

	levelName, ok := slotToLevel[slot]
	if !ok {
		return
	}
	lev := r.levels[levelName]
	slotDef := r.levels[slot]

	// Determine if we're in minigame mode. Whether entering a physical
	// slot triggers minigame mode is a property of the SLOT (the access
	// point), not the content loaded there — a slot only forces minigame
	// mode if it's a minigame slot and not also a story slot (dual slots
	// like DrMelman are reached via the story hub).
	isMinigame := inMinigameChain || (slotDef.IsMinigame && !slotDef.IsStory)
	if lev.IsMinigame && !lev.IsStory {
		isMinigame = true
	}

	// Can't beat this level if it's unbeatable in minigame mode.
	if isMinigame && lev.UnbeatableInMinigameMode {
		return
	}

	// Mark as reachable (used for resource counting later).
	if !reachable[slot] {
		reachable[slot] = true
		*changed = true
	}
	if !reachedContent[levelName] {
		reachedContent[levelName] = true
		*changed = true
	}

	// Unlock any abilities this level grants.
	if lev.UnlocksAbility != "" && !state.abilities[lev.UnlocksAbility] {
		state.abilities[lev.UnlocksAbility] = true
		*changed = true
	}

	// For monkey-gated transitions, we need a running monkey count.
	// Compute it from currently reachable slots.
	tempMonkeys := 0
	for s := range reachable {
		if ln, ok := slotToLevel[s]; ok {
			l := r.levels[ln]
			m := l.Monkeys - l.LockedMonkeys
			if state.abilities["climb"] {
				m = l.Monkeys
			}
			tempMonkeys += m
		}
	}
	state.monkeys = tempMonkeys

	// Follow the CONTENT's own hardcoded transitions — a level's exit
	// script is baked into that level file and travels with it wherever
	// it's shuffled to, rather than belonging to the physical slot it
	// happens to occupy.
	for _, trans := range lev.Transitions {
		if !r.canFollowTransition(levelName, trans, state, reachedContent) {
			continue
		}
		r.walkSlot(trans, slotToLevel, state, reachable, reachedContent, changed, isMinigame)
	}
}

// canFollowTransition checks if a specific transition out of the level
// currently being played (fromContent) to a target slot is available
// given the current progression state. reachedContent tracks which LEVELS
// (not slots) have been played so far, since these gating rules are
// properties of the content's own script, wherever it's been shuffled to.
func (r *Randomizer) canFollowTransition(fromContent, toSlot string, state *progressState, reachedContent map[string]bool) bool {
	// Waterhole → Watercaves requires having entered RitesOfPassage.
	if fromContent == "Waterhole" && toSlot == "Watercaves" {
		if !reachedContent["RitesOfPassage"] {
			return false
		}
	}

	// Waterhole → VolcanoRave requires having completed Wooing_Gloria.
	if fromContent == "Waterhole" && toSlot == "VolcanoRave" {
		if !reachedContent["Wooing_Gloria"] {
			return false
		}
	}

	// Waterhole → penguins is permanently blocked once FixThePlane has
	// been played, regardless of where either is loaded.
	if fromContent == "Waterhole" && toSlot == "penguins" {
		if reachedContent["FixThePlane"] {
			return false
		}
	}

	// Waterhole → ConvoyChase only opens up once ConvoyChase has already
	// been reached via its own chain (penguins → penguins2 → ConvoyChase),
	// or once FixThePlane has been played (which also permanently closes
	// the penguins route above).
	if fromContent == "Waterhole" && toSlot == "ConvoyChase" {
		if !reachedContent["ConvoyChase"] && !reachedContent["FixThePlane"] {
			return false
		}
	}

	// FixThePlane → Dam_Busters requires 60 monkeys.
	if fromContent == "FixThePlane" && toSlot == "Dam_Busters" {
		if state.monkeys < 60 {
			return false
		}
	}

	return true
}

// isCompletable checks that the player can reach the final cutscene, i.e.
// whichever slot the Dam_Busters LEVEL CONTENT ended up in must be reachable.
// This is content-based rather than slot-based: if Dam_Busters' content gets
// shuffled into some other slot (e.g. MartyRace, with no monkey gate), the
// ending would be reachable through that slot instead — which is exactly
// why Dam_Busters is locked by default (see Config.LockedLevels).
func (r *Randomizer) isCompletable(slotToLevel map[string]string) bool {
	damBustersSlot := ""
	for slot, level := range slotToLevel {
		if level == "Dam_Busters" {
			damBustersSlot = slot
			break
		}
	}
	if damBustersSlot == "" {
		return false
	}

	state := r.simulate(slotToLevel)
	return state.visited[damBustersSlot]
}

// changeMarker returns the per-line prefix used in FormatMappings: "L" for
// a slot pinned via Config.LockedLevels, "*" for a slot that was actually
// shuffled, or a blank space if it happened to land back on itself.
func changeMarker(m Mapping) string {
	if m.Locked {
		return "L"
	}
	if m.Slot != m.Level {
		return "*"
	}
	return " "
}

// FormatMappings returns a human-readable mapping text.
func FormatMappings(mappings []Mapping, levels map[string]Level) string {
	var sb strings.Builder
	sb.WriteString("=== Madagascar 2 Level Randomizer Mapping ===\n\n")

	// Find the longest slot name for alignment.
	maxLen := 0
	for _, m := range mappings {
		if len(m.Slot) > maxLen {
			maxLen = len(m.Slot)
		}
	}

	// Separate story and minigame mappings for readability.
	var storyMappings, minigameMappings []Mapping
	for _, m := range mappings {
		slotDef := levels[m.Slot]
		if slotDef.IsStory {
			storyMappings = append(storyMappings, m)
		} else {
			minigameMappings = append(minigameMappings, m)
		}
	}

	if len(storyMappings) > 0 {
		sb.WriteString("── Story Levels ──\n")
		for _, m := range storyMappings {
			sb.WriteString(fmt.Sprintf("%s %-*s  >  %s\n", changeMarker(m), maxLen, m.Slot, m.Level))
		}
	}

	if len(minigameMappings) > 0 {
		sb.WriteString("\n── Minigames ──\n")
		for _, m := range minigameMappings {
			sb.WriteString(fmt.Sprintf("%s %-*s  >  %s\n", changeMarker(m), maxLen, m.Slot, m.Level))
		}
	}

	// Count changes.
	changes := 0
	for _, m := range mappings {
		if m.Slot != m.Level {
			changes++
		}
	}
	sb.WriteString(fmt.Sprintf("\n%d of %d levels shuffled.\n", changes, len(mappings)))

	return sb.String()
}

// WriteRedirectsFile writes mappings in the flat `Slot=Level` format
// mad2modloader's mad2levelredirectmod.dll reads at startup from
// level_redirects.txt (see
// mad2modloader/mad2levelredirectmod/src/levelredirect.cpp) -- that mod
// IAT-hooks the game's level archive opens and rewrites a slot's request
// to load the mapped level's content instead. Only non-identity mappings
// are written: an absent entry means "load normally," which is exactly
// what an unshuffled slot needs, and keeps the file small. `path` is
// typically pointed directly at a deployed game install, e.g.
// mad2modloader/mad2/level_redirects.txt.
func WriteRedirectsFile(path string, mappings []Mapping, seed int64) error {
	var sb strings.Builder
	sb.WriteString(fmt.Sprintf("# Generated by mad2rando5 (seed %d) -- read by mad2levelredirectmod.dll\n", seed))
	for _, m := range mappings {
		if m.Slot == m.Level {
			continue
		}
		sb.WriteString(fmt.Sprintf("%s=%s\n", m.Slot, m.Level))
	}
	return os.WriteFile(path, []byte(sb.String()), 0644)
}
