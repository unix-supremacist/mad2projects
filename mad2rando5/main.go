package main

import (
	"flag"
	"fmt"
	"os"
	"strings"
	"time"
)

func main() {
	seed := flag.Int64("seed", 0, "RNG seed (0 = random)")
	coinsNeeded := flag.Int("coins", 0, "Force just enough coin-bearing levels into coin-bearing slots to reach this many accessible coins (0 = no minimum)")
	maxCoins := flag.Bool("maxcoins", false, "Require every coin-bearing level to land in a coin-bearing slot, guaranteeing a 100%-completable seed (overrides -coins)")
	separatePools := flag.Bool("separate", true, "Keep minigames and story levels in separate pools")
	monkeyMatch := flag.Bool("monkeymatch", false, "Require monkey counts to match between slot and placed level")
	coinMatch := flag.Bool("coinmatch", false, "Require coin counts to match between slot and placed level")
	lockedLevels := flag.String("locked", "Dam_Busters,map,title", "Comma-separated level names to pin to their own slot (never shuffled). Empty string disables all locks.")
	outPath := flag.String("out", "", "Write a level_redirects.txt mapping file mad2modloader's mad2levelredirectmod.dll reads at startup (see mad2modloader/mad2levelredirectmod), in addition to printing the table below. Typically pointed at a deployed install, e.g. mad2modloader/mad2/level_redirects.txt. Empty = don't write one.")
	flag.Parse()

	if *seed == 0 {
		*seed = time.Now().UnixNano()
	}

	coinsTarget := *coinsNeeded
	if *maxCoins {
		coinsTarget = TotalCoins()
	}

	var locked []string
	for _, name := range strings.Split(*lockedLevels, ",") {
		name = strings.TrimSpace(name)
		if name == "" {
			continue
		}
		locked = append(locked, name)
	}

	// Validate locked level names up front so typos fail fast instead of
	// silently doing nothing.
	allLevels := make(map[string]Level)
	for _, l := range AllLevels() {
		allLevels[l.Name] = l
	}
	for _, name := range locked {
		l, ok := allLevels[name]
		if !ok {
			fmt.Fprintf(os.Stderr, "ERROR: -locked references unknown level %q\n", name)
			os.Exit(1)
		}
		if l.IsSkipped {
			fmt.Fprintf(os.Stderr, "ERROR: -locked references %q, which is never randomized\n", name)
			os.Exit(1)
		}
	}

	cfg := Config{
		Seed:                 *seed,
		CoinsNeeded:          coinsTarget,
		SeparatePools:        *separatePools,
		MonkeyCountMustMatch: *monkeyMatch,
		CoinCountMustMatch:   *coinMatch,
		LockedLevels:         locked,
	}

	fmt.Println("Madagascar 2 Logic Randomizer")
	fmt.Println("─────────────────────────────")
	fmt.Printf("Seed:                %d\n", cfg.Seed)
	fmt.Printf("Separate Pools:      %v\n", cfg.SeparatePools)
	fmt.Printf("Monkey Count Match:  %v\n", cfg.MonkeyCountMustMatch)
	fmt.Printf("Coin Count Match:    %v\n", cfg.CoinCountMustMatch)
	fmt.Printf("Coins Needed:        %d\n", cfg.CoinsNeeded)
	fmt.Printf("Locked Levels:       %v\n", cfg.LockedLevels)
	fmt.Println()

	r := NewRandomizer(cfg)
	mappings, err := r.Randomize()
	if err != nil {
		fmt.Fprintf(os.Stderr, "ERROR: %v\n", err)
		os.Exit(1)
	}

	fmt.Print(FormatMappings(mappings, r.levels))

	if *outPath != "" {
		if err := WriteRedirectsFile(*outPath, mappings, cfg.Seed); err != nil {
			fmt.Fprintf(os.Stderr, "ERROR: failed to write %s: %v\n", *outPath, err)
			os.Exit(1)
		}
		fmt.Printf("\nWrote redirect mapping to %s\n", *outPath)
	}
}
