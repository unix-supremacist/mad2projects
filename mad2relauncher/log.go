package main

import (
	"fmt"
	"log"
	"os"
)

// Own log file (mad2relauncher.log), same per-process-logs-to-its-own-file
// convention every mod DLL in this repo already follows -- see CLAUDE.md's
// "Logging conventions" section.
var logger *log.Logger

func initLogger() func() {
	f, err := os.OpenFile("mad2relauncher.log", os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0644)
	if err != nil {
		// Fall back to stderr -- still useful when run interactively for
		// debugging, just not persisted next to the game like every other
		// mod's log.
		logger = log.New(os.Stderr, "", log.Ldate|log.Ltime|log.Lmicroseconds)
		logger.Printf("failed to open mad2relauncher.log: %v -- logging to stderr instead", err)
		return func() {}
	}
	logger = log.New(f, "", log.Ldate|log.Ltime|log.Lmicroseconds)
	return func() { f.Close() }
}

func logf(format string, args ...any) {
	if logger == nil {
		fmt.Fprintf(os.Stderr, format+"\n", args...)
		return
	}
	logger.Printf(format, args...)
}
