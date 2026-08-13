package main

import (
	"fyne.io/fyne/v2/dialog"
)

// autosave writes cfg to disk immediately and surfaces any error -- every
// control that edits state.Cfg calls this straight from its OnChanged
// instead of requiring a separate Save button. Used by ui_music.go's own
// [Music]-section page (state.Cfg / ConfigFile itself, from iniconfig.go,
// stays -- only the GENERIC "every config.cfg section becomes its own
// browsable/editable category" page this file used to also render
// (buildConfigCategoryPage/buildIndexedGroupCard/buildCommentBlock) was
// removed: that's now the in-game debug menu's "Config" tab's job
// instead, see mad2debugmenu/src/debugmenu.cpp's RenderConfigCategory --
// no reason to maintain two independent generic config.cfg editors).
func autosave(state *AppState) {
	if err := state.Cfg.Save(); err != nil {
		dialog.ShowError(err, state.Window)
	}
}
