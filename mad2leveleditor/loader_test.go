package main

import (
	"os"
	"path/filepath"
	"testing"

	"mad2iga"
)

// levelForTest is a real retail archive; the test is skipped if it isn't
// present (it's gitignored game content, not part of the repo).
const levelForTest = "../mad2/Content/Streams/win/VolcanoRave.bld"

// TestLoadEditSaveRoundTrip exercises the non-UI half of the editor: load a
// .bld archive, find an editable field, write it, save (repacking the archive),
// reload, and confirm the edit survived — i.e. everything the GUI's Set/Save
// buttons drive, minus Fyne.
func TestLoadEditSaveRoundTrip(t *testing.T) {
	if _, err := os.Stat(levelForTest); err != nil {
		t.Skipf("game archive not present (%s) — skipping", levelForTest)
	}

	lvl, err := loadLevel(levelForTest)
	if err != nil {
		t.Fatalf("loadLevel: %v", err)
	}
	if !lvl.fromArchive {
		t.Fatalf("expected archive source")
	}

	// Find an editable ActorInfo Vec3f field (position/destination).
	roots, err := lvl.graph.RootObjects()
	if err != nil {
		t.Fatalf("RootObjects: %v", err)
	}
	var target iga.ObjectRef
	var field iga.FieldEntry
	found := false
	for _, r := range roots {
		if r.ClassName != "ActorInfo" {
			continue
		}
		for _, f := range lvl.graph.ObjectFields(r) {
			if f.Editable && f.Kind == "vec3f" {
				target, field, found = r, f, true
				break
			}
		}
		if found {
			break
		}
	}
	if !found {
		t.Skip("no editable ActorInfo Vec3f field found in this level")
	}
	t.Logf("editing %s.%s (%q) @0x%X", target.ClassName, field.Name, lvl.resolvedNameForTest(target), target.Addr)

	want := [3]float32{111.5, -222.25, 333.75}
	if _, _, err := lvl.graph.WriteField(target, field.Name, want); err != nil {
		t.Fatalf("WriteField: %v", err)
	}

	out := filepath.Join(t.TempDir(), "VolcanoRave_edited.bld")
	if err := lvl.save(out); err != nil {
		t.Fatalf("save: %v", err)
	}

	// Reload the saved archive and confirm the edit is there.
	lvl2, err := loadLevel(out)
	if err != nil {
		t.Fatalf("reload: %v", err)
	}
	obj2, ok := lvl2.graph.ObjectAt(target.Addr)
	if !ok || obj2.ClassName != "ActorInfo" {
		t.Fatalf("reloaded object at 0x%X is not the ActorInfo it was", target.Addr)
	}
	got, ok := lvl2.graph.FieldValue(obj2, field.Name)
	if !ok {
		t.Fatalf("reload: field %s unreadable", field.Name)
	}
	gv, ok := got.([3]float32)
	if !ok {
		t.Fatalf("reload: field %s is %T, want [3]float32", field.Name, got)
	}
	if gv != want {
		t.Fatalf("edit did not survive save/reload: got %v want %v", gv, want)
	}
}

// resolvedNameForTest is a tiny accessor so the test can log an object's name.
func (l *loadedLevel) resolvedNameForTest(obj iga.ObjectRef) string {
	if s, ok := l.graph.ResolveFieldName(obj, "_name"); ok {
		return s
	}
	return ""
}
