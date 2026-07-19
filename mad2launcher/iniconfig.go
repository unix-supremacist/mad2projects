package main

import (
	"os"
	"regexp"
	"strconv"
	"strings"
)

// ConfigEntry is one Key=Value line from config.cfg, along with the
// comment block (if any) that immediately preceded it -- mad2config's own
// format seeds a comment above every key on first write (see
// mad2config/src/config_dll.cpp), so surfacing that comment as help text is
// what makes a generic editor legible instead of just a wall of keys.
type ConfigEntry struct {
	Section string
	Key     string
	Value   string
	Comment string

	// SelectOptions comes from a "@select(a,b,c)" directive line in the
	// seeded comment -- see mad2saveloader's Language key for the
	// prototype case. When non-nil, the generic editor renders a dropdown
	// over these options instead of a free-text field.
	SelectOptions []string

	// IndexedGroups comes from an "@indexed(Reward,Effect)" directive line
	// on a count key (e.g. TwitchControls' MappingCount) -- see
	// mad2twitchcontrolsmod's LoadConfig. It names the key prefixes (here
	// "Reward"/"Effect") that repeat as <Prefix><N> for N in 1..Value.
	// When non-nil, the generic editor renders an "Add/Remove mapping"
	// control instead of a plain number field.
	IndexedGroups []string

	lineIdx int // index into ConfigFile.Lines of this entry's "Key=Value" line
}

// ConfigFile is mad2config.dll's config.cfg, parsed into sections while
// keeping the raw lines around so edits only ever touch the one line being
// changed -- every comment, blank line, and key this parser doesn't
// specifically understand survives a round trip untouched.
type ConfigFile struct {
	Path     string
	Lines    []string
	Sections []string // in file order
	Entries  map[string][]*ConfigEntry
}

var sectionHeaderRe = regexp.MustCompile(`^\[(.+)\]$`)
var keyValueRe = regexp.MustCompile(`^([A-Za-z0-9_]+)\s*=(.*)$`)

// Directive lines a seeded comment can carry, on their own "# ..." line --
// see ConfigEntry.SelectOptions/IndexedGroups. Kept out of the human-facing
// Comment text since they're meant for this launcher, not a person reading
// config.cfg by hand.
var selectDirectiveRe = regexp.MustCompile(`^@select\(([^)]*)\)$`)
var indexedDirectiveRe = regexp.MustCompile(`^@indexed\(([^)]*)\)$`)

func splitCSV(s string) []string {
	var out []string
	for _, p := range strings.Split(s, ",") {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

// ParseConfigFile reads and parses path. A missing file is not an error --
// it just means the relevant mod hasn't run yet to seed it (see
// mad2config's "read or seed default" behavior) -- callers should treat a
// nil, no-error return as "nothing to edit yet".
func ParseConfigFile(path string) (*ConfigFile, error) {
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}

	rawLines := strings.Split(strings.ReplaceAll(string(data), "\r\n", "\n"), "\n")

	cf := &ConfigFile{
		Path:    path,
		Lines:   rawLines,
		Entries: map[string][]*ConfigEntry{},
	}

	var currentSection string
	var commentBuf []string
	var pendingSelect, pendingIndexed []string

	for idx, line := range rawLines {
		trimmed := strings.TrimSpace(line)

		if trimmed == "" {
			commentBuf = nil
			pendingSelect, pendingIndexed = nil, nil
			continue
		}
		if strings.HasPrefix(trimmed, "#") {
			text := strings.TrimSpace(strings.TrimPrefix(trimmed, "#"))
			if m := selectDirectiveRe.FindStringSubmatch(text); m != nil {
				pendingSelect = splitCSV(m[1])
			} else if m := indexedDirectiveRe.FindStringSubmatch(text); m != nil {
				pendingIndexed = splitCSV(m[1])
			} else {
				commentBuf = append(commentBuf, text)
			}
			continue
		}
		if m := sectionHeaderRe.FindStringSubmatch(trimmed); m != nil {
			currentSection = m[1]
			cf.Sections = append(cf.Sections, currentSection)
			commentBuf = nil
			pendingSelect, pendingIndexed = nil, nil
			continue
		}
		if m := keyValueRe.FindStringSubmatch(trimmed); m != nil && currentSection != "" {
			entry := &ConfigEntry{
				Section:       currentSection,
				Key:           m[1],
				Value:         strings.TrimSpace(m[2]),
				Comment:       strings.Join(commentBuf, " "),
				SelectOptions: pendingSelect,
				IndexedGroups: pendingIndexed,
				lineIdx:       idx,
			}
			cf.Entries[currentSection] = append(cf.Entries[currentSection], entry)
			commentBuf = nil
			pendingSelect, pendingIndexed = nil, nil
			continue
		}
		commentBuf = nil
		pendingSelect, pendingIndexed = nil, nil
	}

	return cf, nil
}

// SetValue updates entry's value in-place, both in memory and in the
// backing raw line -- Save() just writes cf.Lines back out.
func (cf *ConfigFile) SetValue(entry *ConfigEntry, newValue string) {
	entry.Value = newValue
	cf.Lines[entry.lineIdx] = entry.Key + "=" + newValue
}

func (cf *ConfigFile) Save() error {
	return os.WriteFile(cf.Path, []byte(strings.Join(cf.Lines, "\n")), 0644)
}

// Find returns the entry for section/key, or nil.
func (cf *ConfigFile) Find(section, key string) *ConfigEntry {
	for _, e := range cf.Entries[section] {
		if e.Key == key {
			return e
		}
	}
	return nil
}

// AddEntry appends a new "key=value" line at the end of section (right
// after that section's last existing key, or right after its header if it
// has none) and registers it as a ConfigEntry. Used for @indexed groups,
// where a mapping's Reward<N>/Effect<N> keys don't exist in the file yet
// until the launcher's "Add mapping" control creates them.
func (cf *ConfigFile) AddEntry(section, key, value string) *ConfigEntry {
	insertAt := cf.sectionEndLine(section)

	for _, entries := range cf.Entries {
		for _, e := range entries {
			if e.lineIdx >= insertAt {
				e.lineIdx++
			}
		}
	}

	newLines := make([]string, 0, len(cf.Lines)+1)
	newLines = append(newLines, cf.Lines[:insertAt]...)
	newLines = append(newLines, key+"="+value)
	newLines = append(newLines, cf.Lines[insertAt:]...)
	cf.Lines = newLines

	entry := &ConfigEntry{Section: section, Key: key, Value: value, lineIdx: insertAt}
	cf.Entries[section] = append(cf.Entries[section], entry)
	return entry
}

func (cf *ConfigFile) sectionEndLine(section string) int {
	entries := cf.Entries[section]
	if len(entries) > 0 {
		max := entries[0].lineIdx
		for _, e := range entries {
			if e.lineIdx > max {
				max = e.lineIdx
			}
		}
		return max + 1
	}
	header := "[" + section + "]"
	for i, line := range cf.Lines {
		if strings.TrimSpace(line) == header {
			return i + 1
		}
	}
	return len(cf.Lines)
}

// IsBool reports whether Value looks like a bool key (mad2config only ever
// writes lowercase "true"/"false" -- see config_dll.cpp -- but tolerate any
// case since these files are hand-editable).
func (e *ConfigEntry) IsBool() bool {
	v := strings.ToLower(e.Value)
	return v == "true" || v == "false"
}

func (e *ConfigEntry) BoolValue() bool {
	return strings.EqualFold(e.Value, "true")
}

func BoolString(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

// EnsureSection makes sure section has a "[section]" header line in the
// file, appending one at the end if it doesn't. Used by categories that seed
// config.cfg content nothing in the Wine-side mod chain owns seeding for
// (e.g. mad2music's [Music] section -- see its own config.h header comment
// on why mad2config.dll never gets a chance to seed it).
func (cf *ConfigFile) EnsureSection(section string) {
	for _, s := range cf.Sections {
		if s == section {
			return
		}
	}
	if len(cf.Lines) > 0 && strings.TrimSpace(cf.Lines[len(cf.Lines)-1]) != "" {
		cf.Lines = append(cf.Lines, "")
	}
	cf.Lines = append(cf.Lines, "["+section+"]")
	cf.Sections = append(cf.Sections, section)
}

// EnsureEntry returns the existing entry for section/key if present,
// otherwise seeds it with value, rendering comment as "# ..." lines above
// the key -- the same "read or seed default, self-documenting" contract
// mad2config.dll itself uses (see config_dll.cpp).
func (cf *ConfigFile) EnsureEntry(section, key, value, comment string) *ConfigEntry {
	cf.EnsureSection(section)
	if e := cf.Find(section, key); e != nil {
		return e
	}

	insertAt := cf.sectionEndLine(section)
	var commentLines []string
	for _, l := range strings.Split(comment, "\n") {
		commentLines = append(commentLines, "# "+l)
	}

	shift := len(commentLines) + 1
	for _, entries := range cf.Entries {
		for _, e := range entries {
			if e.lineIdx >= insertAt {
				e.lineIdx += shift
			}
		}
	}

	newLines := make([]string, 0, len(cf.Lines)+shift)
	newLines = append(newLines, cf.Lines[:insertAt]...)
	newLines = append(newLines, commentLines...)
	newLines = append(newLines, key+"="+value)
	newLines = append(newLines, cf.Lines[insertAt:]...)
	cf.Lines = newLines

	entry := &ConfigEntry{Section: section, Key: key, Value: value, Comment: comment, lineIdx: insertAt + len(commentLines)}
	cf.Entries[section] = append(cf.Entries[section], entry)
	return entry
}

// IsNumeric reports whether Value parses as an int or float, purely so the
// UI layer can pick a numeric keyboard/validator for the entry widget.
func (e *ConfigEntry) IsNumeric() bool {
	if _, err := strconv.Atoi(e.Value); err == nil {
		return true
	}
	if _, err := strconv.ParseFloat(e.Value, 64); err == nil {
		return true
	}
	return false
}
