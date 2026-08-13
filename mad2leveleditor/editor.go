package main

import (
	"fmt"
	"strconv"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/widget"

	"mad2iga"
)

// nodeInfo is the per-tree-node state. Tree UIDs are path-encoded (see
// childUIDs) so the same object reachable via two paths gets two distinct
// nodes — that keeps Fyne's strictly-tree widget happy on what is really a
// DAG, and `path` (the chain of object addresses down to this node) lets us
// stop when a reference loops back on itself.
type nodeInfo struct {
	addr    uint32
	display string
	branch  bool
	path    []uint32
}

// editor holds all mutable UI state for one window.
type editor struct {
	win    fyne.Window
	level  *loadedLevel
	tree   *widget.Tree
	right  *fyne.Container // scroll content for the field panel
	status *widget.Label

	meta map[string]nodeInfo // uid -> node
	memo map[string][]string // uid -> child uids (computed once)
}

func newEditor(win fyne.Window) *editor {
	return &editor{win: win, meta: map[string]nodeInfo{}, memo: map[string][]string{}}
}

// resetTreeState drops all cached tree nodes (after opening a new file).
func (e *editor) resetTreeState() {
	e.meta = map[string]nodeInfo{}
	e.memo = map[string][]string{}
}

func (e *editor) childUIDs(uid widget.TreeNodeID) []widget.TreeNodeID {
	if e.level == nil {
		return nil
	}
	if ids, ok := e.memo[uid]; ok {
		return ids
	}
	g := e.level.graph

	var refs []iga.LabeledChild
	var parentPath []uint32
	if uid == "" {
		roots, err := g.RootObjects()
		if err != nil {
			return nil
		}
		for _, r := range roots {
			refs = append(refs, iga.LabeledChild{Child: r})
		}
	} else {
		info := e.meta[uid]
		parentPath = info.path
		obj, ok := g.ObjectAt(info.addr)
		if !ok {
			return nil
		}
		refs = g.ChildRefs(obj)
	}

	ids := make([]widget.TreeNodeID, 0, len(refs))
	for i, c := range refs {
		cyclic := false
		for _, a := range parentPath {
			if a == c.Child.Addr {
				cyclic = true
				break
			}
		}
		cuid := uid + "/" + strconv.Itoa(i) + "." + strconv.FormatUint(uint64(c.Child.Addr), 10)
		np := make([]uint32, len(parentPath), len(parentPath)+1)
		copy(np, parentPath)
		np = append(np, c.Child.Addr)
		e.meta[cuid] = nodeInfo{
			addr:    c.Child.Addr,
			display: e.nodeDisplay(c),
			branch:  !cyclic && len(g.ChildRefs(c.Child)) > 0,
			path:    np,
		}
		ids = append(ids, cuid)
	}
	e.memo[uid] = ids
	return ids
}

// nodeDisplay is the one-line label for a tree node: "field: ClassName "name"".
func (e *editor) nodeDisplay(c iga.LabeledChild) string {
	class := c.Child.ClassName
	if class == "" {
		class = fmt.Sprintf("<unknown:%d>", c.Child.ClassIdx)
	}
	var b strings.Builder
	if c.Label != "" {
		b.WriteString(c.Label)
		b.WriteString(": ")
	}
	b.WriteString(class)
	if name := e.resolvedName(c.Child); name != "" {
		b.WriteString(" \"")
		b.WriteString(name)
		b.WriteString("\"")
	}
	return b.String()
}

func (e *editor) resolvedName(obj iga.ObjectRef) string {
	for _, fn := range []string{"_name", "_varName"} {
		if s, ok := e.level.graph.ResolveFieldName(obj, fn); ok && s != "" {
			return s
		}
	}
	return ""
}

func (e *editor) isBranch(uid widget.TreeNodeID) bool {
	if uid == "" {
		return true
	}
	return e.meta[uid].branch
}

func (e *editor) buildTree() *widget.Tree {
	t := widget.NewTree(
		e.childUIDs,
		e.isBranch,
		func(bool) fyne.CanvasObject { return widget.NewLabel("") },
		func(uid widget.TreeNodeID, _ bool, o fyne.CanvasObject) {
			o.(*widget.Label).SetText(e.meta[uid].display)
		},
	)
	t.OnSelected = func(uid widget.TreeNodeID) {
		if info, ok := e.meta[uid]; ok {
			e.showFields(info.addr)
		}
	}
	return t
}

// showFields rebuilds the right-hand field panel for the object at addr.
func (e *editor) showFields(addr uint32) {
	e.right.Objects = nil
	g := e.level.graph
	obj, ok := g.ObjectAt(addr)
	if !ok {
		e.right.Add(widget.NewLabel(fmt.Sprintf("no object at 0x%X", addr)))
		e.right.Refresh()
		return
	}

	header := fmt.Sprintf("%s @0x%X", obj.ClassName, addr)
	if name := e.resolvedName(obj); name != "" {
		header += fmt.Sprintf("  %q", name)
	}
	e.right.Add(widget.NewLabelWithStyle(header, fyne.TextAlignLeading, fyne.TextStyle{Bold: true}))
	e.right.Add(widget.NewSeparator())

	for _, f := range g.ObjectFields(obj) {
		e.right.Add(e.fieldRow(obj, f))
	}
	e.right.Refresh()
}

// fieldRow renders one field: an editable entry+Set button for writable
// float/Vec3f fields, or a read-only label otherwise.
func (e *editor) fieldRow(obj iga.ObjectRef, f iga.FieldEntry) fyne.CanvasObject {
	name := widget.NewLabel(f.Name)
	if f.Editable {
		entry := widget.NewEntry()
		entry.SetText(editableText(f.Value))
		field := f // capture
		set := widget.NewButton("Set", func() {
			if err := e.applyEdit(obj, field, entry.Text); err != nil {
				e.setStatus("edit failed: " + err.Error())
				return
			}
			e.level.dirty = true
			e.setStatus(fmt.Sprintf("set %s.%s = %s", obj.ClassName, field.Name, entry.Text))
		})
		return container.NewBorder(nil, nil, name, set, entry)
	}
	val := widget.NewLabel(displayValue(f))
	val.Wrapping = fyne.TextWrapWord
	return container.NewBorder(nil, nil, name, nil, val)
}

// applyEdit parses the entry text into the field's type and writes it.
func (e *editor) applyEdit(obj iga.ObjectRef, f iga.FieldEntry, text string) error {
	switch f.Kind {
	case "vec3f":
		parts := strings.FieldsFunc(text, func(r rune) bool { return r == ',' || r == ' ' })
		if len(parts) != 3 {
			return fmt.Errorf("expected 3 comma/space-separated numbers")
		}
		var v3 [3]float32
		for i, p := range parts {
			x, err := strconv.ParseFloat(strings.TrimSpace(p), 32)
			if err != nil {
				return fmt.Errorf("component %d: %v", i, err)
			}
			v3[i] = float32(x)
		}
		_, _, err := e.level.graph.WriteField(obj, f.Name, v3)
		return err
	default: // "float"
		x, err := strconv.ParseFloat(strings.TrimSpace(text), 32)
		if err != nil {
			return err
		}
		_, _, err = e.level.graph.WriteField(obj, f.Name, float32(x))
		return err
	}
}

func (e *editor) setStatus(s string) {
	if e.status != nil {
		e.status.SetText(s)
	}
}

// editableText formats a writable field's current value for its entry box.
func editableText(v interface{}) string {
	switch t := v.(type) {
	case float32:
		return strconv.FormatFloat(float64(t), 'g', -1, 32)
	case [3]float32:
		return fmt.Sprintf("%g, %g, %g", t[0], t[1], t[2])
	default:
		return fmt.Sprintf("%v", v)
	}
}

// displayValue formats a read-only field for its label.
func displayValue(f iga.FieldEntry) string {
	switch f.Kind {
	case "ptr":
		if cp, ok := f.Value.(*iga.ObjectRef); ok && cp != nil {
			return fmt.Sprintf("-> %s @0x%X", cp.ClassName, cp.Addr)
		}
		return "-> null"
	case "string":
		if s, ok := f.Value.(string); ok {
			return strconv.Quote(s)
		}
	case "matrix44f":
		if m, ok := f.Value.([16]float32); ok {
			return fmt.Sprintf("matrix (translation %g, %g, %g)", m[12], m[13], m[14])
		}
	case "vec3f":
		if v, ok := f.Value.([3]float32); ok {
			return fmt.Sprintf("[%g, %g, %g]", v[0], v[1], v[2])
		}
	case "float":
		if x, ok := f.Value.(float32); ok {
			return strconv.FormatFloat(float64(x), 'g', -1, 32)
		}
	}
	return fmt.Sprintf("%v", f.Value)
}
