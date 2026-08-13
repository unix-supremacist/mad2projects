// mad2leveleditor is a small Fyne desktop GUI for browsing and editing
// Madagascar 2 level.bld object graphs. It opens a raw level.bld (IGZ) or a
// .bld igArchive, shows the whole object tree (names resolved via the same
// authoritative RSTR/TSTR fixup tables as `mad2repack level-tree`), and lets
// you edit the fields that are safe to write in place — the PC-verified
// float / Vec3f fields WriteField accepts (positions, ranges, scalars). Every
// other field is shown read-only. Saving writes a byte-exact edited level.bld
// (repacked back into the archive if that's what you opened).
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/widget"
)

func main() {
	a := app.New()
	w := a.NewWindow("mad2 level.bld editor")

	e := newEditor(w)
	e.status = widget.NewLabel("Open a level.bld or .bld to begin.")
	e.right = container.NewVBox()
	e.tree = e.buildTree()

	openBtn := widget.NewButton("Open…", func() { e.openDialog() })
	saveBtn := widget.NewButton("Save", func() { e.saveInPlace() })
	saveAsBtn := widget.NewButton("Save As…", func() { e.saveAsDialog() })
	toolbar := container.NewHBox(openBtn, saveBtn, saveAsBtn)

	split := container.NewHSplit(
		container.NewScroll(e.tree),
		container.NewScroll(e.right),
	)
	split.Offset = 0.45

	w.SetContent(container.NewBorder(toolbar, e.status, nil, nil, split))
	w.Resize(fyne.NewSize(1100, 720))

	if len(os.Args) > 1 {
		e.open(os.Args[1])
	}
	w.ShowAndRun()
}

// open loads a level and rebuilds the tree.
func (e *editor) open(path string) {
	lvl, err := loadLevel(path)
	if err != nil {
		dialog.ShowError(err, e.win)
		e.setStatus("open failed: " + err.Error())
		return
	}
	e.level = lvl
	e.resetTreeState()
	e.right.Objects = nil
	e.right.Refresh()
	e.tree.Refresh()
	e.tree.UnselectAll()
	src := "IGZ"
	if lvl.fromArchive {
		src = "archive:" + lvl.memberName
	}
	e.setStatus(fmt.Sprintf("Opened %s (%s, %d classes)", filepath.Base(path), src, len(lvl.graph.ClassNames)))
	e.win.SetTitle("mad2 level.bld editor — " + filepath.Base(path))
}

func (e *editor) openDialog() {
	dialog.ShowFileOpen(func(r fyne.URIReadCloser, err error) {
		if err != nil || r == nil {
			return
		}
		path := r.URI().Path()
		_ = r.Close()
		e.open(path)
	}, e.win)
}

func (e *editor) saveInPlace() {
	if e.level == nil {
		return
	}
	if err := e.level.save(e.level.srcPath); err != nil {
		dialog.ShowError(err, e.win)
		e.setStatus("save failed: " + err.Error())
		return
	}
	e.level.dirty = false
	e.setStatus("Saved " + filepath.Base(e.level.srcPath))
}

func (e *editor) saveAsDialog() {
	if e.level == nil {
		return
	}
	dialog.ShowFileSave(func(wc fyne.URIWriteCloser, err error) {
		if err != nil || wc == nil {
			return
		}
		path := wc.URI().Path()
		_ = wc.Close() // we write the file ourselves (may need to repack)
		if err := e.level.save(path); err != nil {
			dialog.ShowError(err, e.win)
			e.setStatus("save failed: " + err.Error())
			return
		}
		e.level.dirty = false
		e.setStatus("Saved " + filepath.Base(path))
	}, e.win)
}
