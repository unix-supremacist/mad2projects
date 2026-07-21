package main

import (
	"fmt"
	"path/filepath"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

// ensureConfigFile returns state.Cfg, creating an empty in-memory one keyed
// to inst's config.cfg if it doesn't exist yet (the game hasn't run once to
// let mad2config.dll seed it) -- see ParseConfigFile's own "missing file is
// not an error" contract.
func ensureConfigFile(state *AppState, inst Install) *ConfigFile {
	if state.Cfg != nil {
		return state.Cfg
	}
	cfg := &ConfigFile{Path: filepath.Join(inst.Dir, "config.cfg"), Entries: map[string][]*ConfigEntry{}}
	state.Cfg = cfg
	return cfg
}

// boolConfigRow renders one config.cfg bool entry as an auto-saving Check
// with its seeded comment as help text -- same shape as
// buildConfigCategoryPage's generic bool row, duplicated here rather than
// shared since this page also seeds entries that generic page never touches.
func boolConfigRow(state *AppState, label string, e *ConfigEntry) fyne.CanvasObject {
	chk := widget.NewCheck("", nil)
	chk.SetChecked(e.BoolValue())
	chk.OnChanged = func(v bool) {
		state.Cfg.SetValue(e, BoolString(v))
		autosave(state)
	}
	row := container.NewBorder(nil, nil, widget.NewLabel(label), nil, chk)
	help := widget.NewLabel(e.Comment)
	help.Wrapping = fyne.TextWrapWord
	help.TextStyle = fyne.TextStyle{Italic: true}
	return container.NewVBox(row, help)
}

// buildMusicPage is the "Music" category: mad2music's own config.cfg
// settings (seeded here since nothing in the Wine-side mod chain owns
// seeding [Music] -- see mad2music/src/config.h's header comment), this
// launcher's own playback toggle/audio-dir override (routed through
// mad2relauncher -- see launch.go), a manual build/rebuild control for the
// native mad2music binary, and deploy/remove controls for the blanked
// in-game global.arc (mad2musicpatch/).
func buildMusicPage(state *AppState) fyne.CanvasObject {
	header := widget.NewLabelWithStyle("Music", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})

	inst := state.SelectedInstall()
	if inst == nil {
		return container.NewVBox(header, widget.NewLabel("No install selected -- pick one under Game Launch first."))
	}
	instCopy := *inst
	repoRoot := filepath.Dir(instCopy.Dir)
	settings := state.Settings.For(instCopy)

	persist := func() {
		state.Settings.Set(instCopy, settings)
		if err := state.Settings.Save(); err != nil {
			dialog.ShowError(err, state.Window)
		}
	}

	// Seed [Music] (never seeded by any Wine-side mod -- mad2music is a
	// native process, not a config-API consumer) and [Podcast] StatusPort
	// (mad2podcastmod only ever seeds ControlPort/Weight/TestTriggerKey) if
	// missing, then write it straight back -- the same "seed + write
	// immediately" contract mad2config.dll itself uses.
	cfg := ensureConfigFile(state, instCopy)
	enabledEntry := cfg.EnsureEntry("Music", "Enabled", "true",
		"Whether mad2music plays background music/podcast clips at all.")
	copyrightEntry := cfg.EnsureEntry("Music", "AllowCopyright", "true",
		"Include tracks flagged as real-world licensed songs (e.g. Iko Iko, Samba de\nJaneiro) in the pool, not just TFB's own score.")
	lyricsEntry := cfg.EnsureEntry("Music", "AllowLyrics", "true",
		"Include tracks with vocals/lyrics in the pool.")
	customEntry := cfg.EnsureEntry("Music", "AllowCustom", "true",
		"Include any user-added tracks dropped into mad2music's audio pool directory.")
	statusPortEntry := cfg.EnsureEntry("Podcast", "StatusPort", "47071",
		"Loopback UDP port mad2music broadcasts podcast play/finished status on. Must\n"+
			"match mad2music's own default (MAD2PODCAST_DEFAULT_STATUS_PORT) -- config.cfg\n"+
			"is read by both sides independently, see mad2music/src/config.cpp.")
	autosave(state)

	configCard := widget.NewCard("mad2music settings (config.cfg)", "", container.NewVBox(
		boolConfigRow(state, "Enabled", enabledEntry),
		boolConfigRow(state, "Allow copyrighted tracks", copyrightEntry),
		boolConfigRow(state, "Allow tracks with lyrics", lyricsEntry),
		boolConfigRow(state, "Allow custom tracks", customEntry),
	))

	statusPortEntryWidget := widget.NewEntry()
	statusPortEntryWidget.SetText(statusPortEntry.Value)
	statusPortEntryWidget.OnChanged = func(v string) {
		state.Cfg.SetValue(statusPortEntry, v)
		autosave(state)
	}
	controlPortNote := widget.NewLabel("ControlPort lives under the \"Podcast\" category (seeded by mad2podcastmod.dll).")
	controlPortNote.TextStyle = fyne.TextStyle{Italic: true}
	podcastCard := widget.NewCard("Podcast status port (config.cfg)", "",
		container.NewVBox(
			widget.NewForm(widget.NewFormItem("StatusPort", statusPortEntryWidget)),
			controlPortNote,
		))

	// Launcher-owned playback toggle + audio-dir override -- these aren't
	// mad2music's own settings, they're how *this launcher* invokes
	// mad2relauncher (see buildInnerLaunchCommand in launch.go).
	enablePlaybackChk := widget.NewCheck("Play background music/podcasts when launching via this launcher", nil)
	enablePlaybackChk.SetChecked(!settings.MusicDisabled)
	enablePlaybackChk.OnChanged = func(v bool) { settings.MusicDisabled = !v; persist() }

	defaultAudioDir := filepath.Join(repoRoot, "mad2music", "audio")
	audioDirEntry := widget.NewEntry()
	audioDirEntry.SetPlaceHolder(defaultAudioDir + " (default)")
	audioDirEntry.SetText(settings.MusicAudioDir)
	audioDirEntry.OnChanged = func(v string) { settings.MusicAudioDir = v; persist() }

	playbackCard := widget.NewCard("Playback (mad2launcher -> mad2relauncher)", "", container.NewVBox(
		enablePlaybackChk,
		widget.NewForm(widget.NewFormItem("Audio pool directory (blank = default)", audioDirEntry)),
	))

	// mad2music itself is shipped as a prebuilt binary now, not built on
	// demand from here (see build_native.go's musicBinPath and changes.md --
	// this used to shell out to cmake directly, duplicating the justfile's
	// build-music-linux recipe). This card is status-only: it just tells you
	// whether `just build-music-linux` has been run yet.
	binStatus := widget.NewLabel(musicBinStatusText(repoRoot))
	binCard := widget.NewCard("mad2music binary", "",
		container.NewVBox(binStatus, widget.NewLabel("Built via `just build-music-linux` (not from this launcher).")))

	// Music downloading -- runs a prebuilt mad2musicdownloader binary (see
	// ResolveMad2MusicDownloader in build_native.go -- never built on demand)
	// against mad2music/audio/music's category link lists.
	// Progress/errors go to mad2music/logs/music_download.log, not this UI
	// (per changes.md, a log file is enough -- no need to stream yt-dlp's
	// full output here).
	downloadStatus := widget.NewLabel("Idle.")
	downloadButton := widget.NewButton("Download music now", nil)
	downloadButton.OnTapped = func() {
		downloadButton.Disable()
		downloadStatus.SetText("Downloading... (see mad2music/logs/music_download.log)")
		go func() {
			err := RunMusicDownloader(repoRoot)
			fyne.Do(func() {
				downloadButton.Enable()
				if err != nil {
					downloadStatus.SetText("Failed: " + err.Error())
					dialog.ShowError(err, state.Window)
					return
				}
				downloadStatus.SetText("Done. See mad2music/logs/music_download.log for details.")
			})
		}()
	}
	downloadCard := widget.NewCard("Download music", "",
		container.NewVBox(
			widget.NewLabel("Fetches every URL listed in mad2music/audio/music/*.txt via yt-dlp, skipping anything already downloaded."),
			downloadStatus,
			downloadButton,
		))

	// Blanked in-game global.arc deploy/remove -- see mad2musicpatch/. The
	// description used to be passed as the Card's *subtitle*, which Fyne
	// renders as a single-line canvas.Text with no wrapping and no support
	// for embedded newlines -- a long subtitle blew the card (and the whole
	// page) out super wide, and the "\n"s in it were silently ignored. Fixed
	// by moving it into a wrapped Label inside the card's content instead.
	arcStatus := widget.NewLabel(globalArcStatusText(repoRoot))
	deployArcButton := widget.NewButton("Deploy blanked global.arc (all installs)", func() {
		arcStatus.SetText("Working...")
		out, err := DeployBlankGlobalArc(repoRoot)
		arcStatus.SetText(globalArcStatusText(repoRoot))
		if err != nil {
			dialog.ShowError(fmt.Errorf("%w\n\n%s", err, out), state.Window)
			return
		}
		dialog.ShowInformation("Blank global.arc deployed", out, state.Window)
	})
	removeArcButton := widget.NewButton("Remove blanked global.arc (all installs)", func() {
		if err := RemoveBlankGlobalArc(repoRoot); err != nil {
			dialog.ShowError(err, state.Window)
		}
		arcStatus.SetText(globalArcStatusText(repoRoot))
	})
	arcDesc := widget.NewLabel(
		"Silences the game's own background music inside global.arc (feeding the original " +
			"tracks into mad2music's pool instead) so it doesn't compete with mad2music's " +
			"external player. Non-destructive -- writes to Content/Streams/mod/global.arc in " +
			"every install found under this repo, never touching the original archive. " +
			"Requires ffmpeg on PATH; builds mad2arc/mad2repack automatically if needed.")
	arcDesc.Wrapping = fyne.TextWrapWord
	arcCard := widget.NewCard("Blanked in-game music (global.arc)", "",
		container.NewVBox(arcDesc, arcStatus, deployArcButton, removeArcButton))

	body := container.NewVBox(
		header,
		playbackCard,
		binCard,
		downloadCard,
		configCard,
		podcastCard,
		arcCard,
	)
	return container.NewScroll(body)
}

func musicBinStatusText(repoRoot string) string {
	if binPath, err := ResolveMad2Music(repoRoot); err == nil {
		return "Found: " + binPath
	}
	return "Not found: " + musicBinPath(repoRoot) + " (run `just build-music-linux`, or install the mad2music package via the Mod Installer)"
}

func globalArcStatusText(repoRoot string) string {
	if AnyBlankGlobalArcDeployed(repoRoot) {
		return "Deployed for at least one install."
	}
	return "Not deployed for any install."
}
