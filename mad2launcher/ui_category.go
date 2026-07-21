package main

import (
	"fmt"
	"net/url"
	"regexp"
	"strconv"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

// urlInCommentRe finds a bare URL inside a config comment string -- e.g.
// mad2igttimer seeds its "Timer" section's ResetKey/ResetComboButtons
// comments with a trailing "See https://learn.microsoft.com/..." line (see
// mad2igttimer/src/igttimer.cpp's LoadConfig). Plain-text rendering left
// these dead, unclickable strings; buildCommentBlock below turns any match
// into a real widget.Hyperlink instead.
var urlInCommentRe = regexp.MustCompile(`https?://\S+`)

// buildCommentBlock renders a config.cfg comment (the text mad2config seeds
// above a key, possibly multi-line) as wrapped, auto-clickable text: any
// line containing a bare URL gets that URL split out into a
// widget.Hyperlink, everything else renders as an italic wrapped Label.
// Used for every comment shown in the generically-rendered config category
// pages, not just "Timer" -- any mod's config comment gets the same
// treatment if it ever grows a URL.
func buildCommentBlock(comment string) fyne.CanvasObject {
	lines := strings.Split(comment, "\n")
	rows := make([]fyne.CanvasObject, 0, len(lines))
	for _, line := range lines {
		loc := urlInCommentRe.FindStringIndex(line)
		if loc == nil {
			lbl := widget.NewLabel(line)
			lbl.Wrapping = fyne.TextWrapWord
			lbl.TextStyle = fyne.TextStyle{Italic: true}
			rows = append(rows, lbl)
			continue
		}

		rawURL := line[loc[0]:loc[1]]
		parsed, err := url.Parse(rawURL)
		if err != nil {
			lbl := widget.NewLabel(line)
			lbl.Wrapping = fyne.TextWrapWord
			lbl.TextStyle = fyne.TextStyle{Italic: true}
			rows = append(rows, lbl)
			continue
		}

		// A URL is one unbroken token with no whitespace for TextWrapWord to
		// break at, so a long one (e.g. the ~70-char MSDN virtual-key-codes
		// reference several mods' TestTriggerKey/ResetKey comments link to)
		// forces the Hyperlink's -- and therefore the whole window's --
		// minimum width out to fit it verbatim, however this page is later
		// laid out. Truncate what's *displayed* instead; the real URL stays
		// the actual link target (parsed, unmodified) either way.
		link := widget.NewHyperlink(shortenURLForDisplay(rawURL), parsed)
		if prefix := strings.TrimRight(line[:loc[0]], " "); prefix != "" {
			pfx := widget.NewLabel(prefix)
			pfx.TextStyle = fyne.TextStyle{Italic: true}
			rows = append(rows, container.NewHBox(pfx, link))
		} else {
			rows = append(rows, link)
		}
	}
	return container.NewVBox(rows...)
}

// shortenURLForDisplay caps a URL's displayed text at maxURLDisplayLen,
// eliding the middle -- keeps both the scheme/host (so it's still
// recognizable/trustworthy at a glance) and the tail (often the most
// specific part, e.g. the actual API name in an MSDN link) visible.
const maxURLDisplayLen = 42

func shortenURLForDisplay(rawURL string) string {
	if len(rawURL) <= maxURLDisplayLen {
		return rawURL
	}
	head := maxURLDisplayLen * 3 / 5
	tail := maxURLDisplayLen - head - 1 // -1 for the ellipsis rune itself
	return rawURL[:head] + "…" + rawURL[len(rawURL)-tail:]
}

// autosave writes cfg to disk immediately and surfaces any error -- every
// control in this file calls this straight from its OnChanged instead of
// requiring a separate Save button.
func autosave(state *AppState) {
	if err := state.Cfg.Save(); err != nil {
		dialog.ShowError(err, state.Window)
	}
}

// buildConfigCategoryPage renders every key in config.cfg's [section] as an
// editable, auto-saving row: a Check for bool-looking values, a Select for
// keys carrying an "@select(...)" directive (see mad2saveloader's Language
// key), an "Add/Remove mapping" control for a key carrying an
// "@indexed(...)" directive (see mad2twitchcontrolsmod's MappingCount), and
// a plain Entry otherwise. The comment mad2config seeded above the key is
// shown as help text. This is what turns "every category in config.cfg"
// into editable launcher categories generically, without the launcher
// needing to know anything about any specific mod's settings.
func buildConfigCategoryPage(state *AppState, section string) fyne.CanvasObject {
	if state.Cfg == nil {
		return container.NewVBox(
			widget.NewLabelWithStyle(section, fyne.TextAlignLeading, fyne.TextStyle{Bold: true}),
			widget.NewLabel("No config.cfg found for this install yet -- run the game once so mad2config.dll can seed it, then reopen this install."),
		)
	}

	entries := state.Cfg.Entries[section]

	// An "@indexed(Reward,Effect)" count key (e.g. TwitchControls'
	// MappingCount) renders its own Reward<N>/Effect<N> fields inside
	// buildIndexedGroupCard below. Those <Prefix><N> keys also exist as
	// their own plain ConfigEntry in this same section slice (see
	// ParseConfigFile -- only the count key itself carries IndexedGroups),
	// so without this exclusion they'd *also* fall through to the generic
	// rendering below and every mapping would render twice. Collect every
	// prefix from every indexed-group count key in this section first, so
	// the main loop can skip anything matching "<prefix><digits>".
	var indexedPrefixRes []*regexp.Regexp
	for _, e := range entries {
		for _, prefix := range e.IndexedGroups {
			indexedPrefixRes = append(indexedPrefixRes, regexp.MustCompile(`^`+regexp.QuoteMeta(prefix)+`\d+$`))
		}
	}
	isIndexedGroupKey := func(key string) bool {
		for _, re := range indexedPrefixRes {
			if re.MatchString(key) {
				return true
			}
		}
		return false
	}

	var rows []fyne.CanvasObject

	for _, e := range entries {
		if e.IndexedGroups != nil {
			rows = append(rows, buildIndexedGroupCard(state, section, e))
			continue
		}
		if isIndexedGroupKey(e.Key) {
			continue
		}

		var control fyne.CanvasObject
		switch {
		case e.SelectOptions != nil:
			sel := widget.NewSelect(e.SelectOptions, nil)
			sel.SetSelected(e.Value)
			sel.OnChanged = func(v string) {
				state.Cfg.SetValue(e, v)
				autosave(state)
			}
			control = sel
		case e.IsBool():
			chk := widget.NewCheck("", nil)
			chk.SetChecked(e.BoolValue())
			chk.OnChanged = func(v bool) {
				state.Cfg.SetValue(e, BoolString(v))
				autosave(state)
			}
			control = chk
		default:
			entryWidget := widget.NewEntry()
			entryWidget.SetText(e.Value)
			entryWidget.OnChanged = func(v string) {
				state.Cfg.SetValue(e, v)
				autosave(state)
			}
			control = entryWidget
		}

		row := container.NewBorder(nil, nil, widget.NewLabel(e.Key), nil, control)
		if e.Comment != "" {
			rows = append(rows, container.NewVBox(row, buildCommentBlock(e.Comment)), widget.NewSeparator())
		} else {
			rows = append(rows, row, widget.NewSeparator())
		}
	}

	if len(entries) == 0 {
		rows = append(rows, widget.NewLabel("No keys found in this section."))
	}

	header := widget.NewLabelWithStyle(section, fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	scroll := container.NewScroll(container.NewVBox(rows...))

	return container.NewBorder(header, nil, nil, nil, scroll)
}

// buildIndexedGroupCard renders a count key's "@indexed(Prefix1,Prefix2,...)"
// directive as a repeating group of fields (e.g. Reward<N>/Effect<N>) with
// "+ Add mapping" / "- Remove last mapping" controls that manage the count
// key and create/reuse the matching <Prefix><N> entries -- this is the
// "funny spec" for growing TwitchControls' reward->effect mappings past
// whatever MappingCount the config.cfg happened to be seeded with.
func buildIndexedGroupCard(state *AppState, section string, countEntry *ConfigEntry) fyne.CanvasObject {
	groups := countEntry.IndexedGroups
	count, err := strconv.Atoi(countEntry.Value)
	if err != nil || count < 0 {
		count = 0
	}

	title := ""
	for i, g := range groups {
		if i > 0 {
			title += " / "
		}
		title += g
	}
	title += " mappings"

	var rows []fyne.CanvasObject
	for i := 1; i <= count; i++ {
		var fields []*widget.FormItem
		for _, g := range groups {
			key := fmt.Sprintf("%s%d", g, i)
			entry := state.Cfg.Find(section, key)
			if entry == nil {
				entry = state.Cfg.AddEntry(section, key, "")
			}
			w := widget.NewEntry()
			w.SetText(entry.Value)
			entryRef := entry
			w.OnChanged = func(v string) {
				state.Cfg.SetValue(entryRef, v)
				autosave(state)
			}
			fields = append(fields, widget.NewFormItem(fmt.Sprintf("%s%d", g, i), w))
		}
		rows = append(rows, widget.NewForm(fields...))
	}

	addButton := widget.NewButton("+ Add mapping", func() {
		newCount := count + 1
		for _, g := range groups {
			key := fmt.Sprintf("%s%d", g, newCount)
			if state.Cfg.Find(section, key) == nil {
				state.Cfg.AddEntry(section, key, "")
			}
		}
		state.Cfg.SetValue(countEntry, strconv.Itoa(newCount))
		autosave(state)
		state.ShowCategory(section)
	})

	removeButton := widget.NewButton("- Remove last mapping", func() {
		if count <= 0 {
			return
		}
		state.Cfg.SetValue(countEntry, strconv.Itoa(count-1))
		autosave(state)
		state.ShowCategory(section)
	})
	if count <= 0 {
		removeButton.Disable()
	}

	comment := buildCommentBlock(countEntry.Comment)

	body := container.NewVBox(rows...)
	buttons := container.NewGridWithColumns(2, addButton, removeButton)
	return widget.NewCard(title, "", container.NewVBox(comment, body, buttons))
}
