package main

import (
	"fmt"
	"os"
	"strings"
	"time"

	"github.com/BurntSushi/xgb"
	"github.com/BurntSushi/xgb/xproto"
)

// Minimal, self-contained X11 client -- no xgbutil, just raw xgb/xproto.
// Talks to whatever $DISPLAY points at, which under gamescope is its own
// nested X server (gamescope execs mad2relauncher as its `-- ...` child,
// same as it did for ../mad2mod/src/main.cpp's overlay tool, so the
// nested DISPLAY is already set correctly by the time this runs).
//
// Window lookup uses a raw XQueryTree recursive walk + WM_NAME/_NET_WM_NAME
// property reads, not EWMH's _NET_CLIENT_LIST -- deliberately mirroring
// ../mad2mod/src/main.cpp's find_window_by_title_substring technique rather
// than a higher-level EWMH library, since that's the approach already
// proven to work against gamescope's nested X server in this exact setup.
type X11 struct {
	conn *xgb.Conn
	root xproto.Window

	netWmName       xproto.Atom
	utf8String      xproto.Atom
	netActiveWindow xproto.Atom

	mad2Win    xproto.Window
	mad2Hidden bool
}

func OpenX11() (*X11, error) {
	conn, err := xgb.NewConnDisplay("")
	if err != nil {
		return nil, fmt.Errorf("connect to X11 (DISPLAY=%q): %w", envDisplay(), err)
	}
	setup := xproto.Setup(conn)
	screen := setup.DefaultScreen(conn)

	x := &X11{conn: conn, root: screen.Root}
	x.netWmName = x.internAtom("_NET_WM_NAME")
	x.utf8String = x.internAtom("UTF8_STRING")
	x.netActiveWindow = x.internAtom("_NET_ACTIVE_WINDOW")
	return x, nil
}

func envDisplay() string {
	if d := os.Getenv("DISPLAY"); d != "" {
		return d
	}
	return "(unset)"
}

func (x *X11) internAtom(name string) xproto.Atom {
	reply, err := xproto.InternAtom(x.conn, false, uint16(len(name)), name).Reply()
	if err != nil || reply == nil {
		return xproto.AtomNone
	}
	return reply.Atom
}

// GrabStopKey grabs Ctrl+Q as a global hotkey (all lock-key modifier
// variants, so it fires regardless of Caps Lock/Num Lock state) and calls
// stopFn from a dedicated event-loop goroutine when it's pressed. Blocks
// forever pumping X events -- call with `go`.
func (x *X11) GrabStopKey(stopFn func()) error {
	keycode, err := x.keycodeForLatin1('q')
	if err != nil {
		return err
	}

	// XGrabKey matches modifier state EXACTLY, so Caps Lock (Lock) and Num
	// Lock (Mod2, on the vast majority of X servers) being on would
	// otherwise make Ctrl+Q silently not match -- grab every combination
	// of those two "don't care" locks alongside Control.
	const lockMask = xproto.ModMaskLock
	const numLockMask = xproto.ModMask2
	for _, extra := range []uint16{0, lockMask, numLockMask, lockMask | numLockMask} {
		mods := uint16(xproto.ModMaskControl) | extra
		xproto.GrabKey(x.conn, true, x.root, mods, keycode, xproto.GrabModeAsync, xproto.GrabModeAsync)
	}

	for {
		ev, xerr := x.conn.WaitForEvent()
		if xerr != nil {
			logf("X11 protocol error while waiting for Ctrl+Q: %v", xerr)
			continue
		}
		if ev == nil {
			// The nested X server going away (gamescope exiting, or hanging
			// badly enough to tear down its own display) is at least as
			// strong a "we're done" signal as Ctrl+Q -- observed live: a
			// session where this fired left mad2relauncher and mad2music
			// running for minutes afterward because nothing else told them
			// to stop (armParentDeathSignal only fires once gamescope's
			// *process* actually dies, which doesn't always coincide with
			// its nested display going away). Treat it the same as the
			// stop-key instead of just logging and returning.
			logf("X11 connection closed -- treating as a stop signal")
			stopFn()
			return nil
		}
		if kp, ok := ev.(xproto.KeyPressEvent); ok && kp.Detail == keycode {
			logf("Ctrl+Q pressed -- stopping")
			stopFn()
		}
	}
}

func (x *X11) keycodeForLatin1(ch byte) (xproto.Keycode, error) {
	setup := xproto.Setup(x.conn)
	count := byte(setup.MaxKeycode - setup.MinKeycode + 1)
	reply, err := xproto.GetKeyboardMapping(x.conn, setup.MinKeycode, count).Reply()
	if err != nil {
		return 0, fmt.Errorf("GetKeyboardMapping: %w", err)
	}
	perCode := int(reply.KeysymsPerKeycode)
	want := xproto.Keysym(ch) // Latin-1 keysyms equal their ASCII code for a-z
	for i := 0; i < int(count); i++ {
		base := i * perCode
		if base >= len(reply.Keysyms) {
			break
		}
		if reply.Keysyms[base] == want {
			return xproto.Keycode(int(setup.MinKeycode) + i), nil
		}
	}
	return 0, fmt.Errorf("no keycode found for keysym %#x", want)
}

// FindWindowByTitle recursively walks the window tree from the root
// looking for a window whose WM_NAME or _NET_WM_NAME contains substr
// (case-insensitive). Returns (0, false) if none is found.
func (x *X11) FindWindowByTitle(substr string) (xproto.Window, bool) {
	return x.findByTitle(x.root, strings.ToLower(substr))
}

func (x *X11) findByTitle(win xproto.Window, substrLower string) (xproto.Window, bool) {
	if title, ok := x.windowTitle(win); ok && strings.Contains(strings.ToLower(title), substrLower) {
		return win, true
	}

	tree, err := xproto.QueryTree(x.conn, win).Reply()
	if err != nil || tree == nil {
		return 0, false
	}
	for _, child := range tree.Children {
		if found, ok := x.findByTitle(child, substrLower); ok {
			return found, true
		}
	}
	return 0, false
}

func (x *X11) windowTitle(win xproto.Window) (string, bool) {
	if reply, err := xproto.GetProperty(x.conn, false, win, xproto.AtomWmName, xproto.AtomString, 0, 256).Reply(); err == nil && reply != nil && reply.ValueLen > 0 {
		return string(reply.Value), true
	}
	if x.netWmName != xproto.AtomNone {
		if reply, err := xproto.GetProperty(x.conn, false, win, x.netWmName, x.utf8String, 0, 256).Reply(); err == nil && reply != nil && reply.ValueLen > 0 {
			return string(reply.Value), true
		}
	}
	return "", false
}

func (x *X11) moveWindow(win xproto.Window, xPos, yPos int32) {
	xproto.ConfigureWindow(x.conn, win, xproto.ConfigWindowX|xproto.ConfigWindowY,
		[]uint32{uint32(xPos), uint32(yPos)})
}

// unmapWindow/mapWindow -- see FocusWindowAndHideMad2's comment for why
// hiding Mad2 needs this rather than (or in addition to) a plain
// off-screen ConfigureWindow move.
func (x *X11) unmapWindow(win xproto.Window) {
	xproto.UnmapWindow(x.conn, win)
}

func (x *X11) mapWindow(win xproto.Window) {
	xproto.MapWindow(x.conn, win)
}

// sync forces a round-trip to the X server, guaranteeing every request
// queued before this call (e.g. MapWindow) has actually been processed --
// X11 requests on one connection are handled strictly in order, so a
// reply-generating request can't complete before earlier ones do. Needed
// between mapWindow and raiseAndFocus in RestoreMad2: SetInputFocus errors
// BadMatch if the target window isn't yet viewable, and MapWindow is
// otherwise fire-and-forget (no reply to wait on), so without this the two
// calls can race against the server's own processing of the map (observed
// live: an intermittent BadMatch on restore before this was added).
func (x *X11) sync() {
	xproto.GetInputFocus(x.conn).Reply()
}

func (x *X11) resizeWindow(win xproto.Window, width, height uint32) {
	xproto.ConfigureWindow(x.conn, win, xproto.ConfigWindowWidth|xproto.ConfigWindowHeight,
		[]uint32{width, height})
}

// raiseAndFocus asks for win to be raised/activated via the standard EWMH
// _NET_ACTIVE_WINDOW client-message protocol (sent to the root window, per
// https://specifications.freedesktop.org/wm-spec/wm-spec-latest.html#idm45623487798480)
// -- NOT plain ConfigureWindow(StackModeAbove) + SetInputFocus, which was
// tried first and didn't work: gamescope is a Wayland compositor exposing
// an Xwayland surface, and its own notion of "which app is currently
// displayed" is tracked through this EWMH protocol (the same one every
// taskbar/pager/alt-tab switcher uses to raise windows in someone else's
// client), not through raw X11 stacking order, which a third-party client
// requesting it on another client's window is routinely ignored for
// (focus-stealing prevention) by modern WMs/compositors in general.
func (x *X11) raiseAndFocus(win xproto.Window) {
	data := xproto.ClientMessageDataUnionData32New([]uint32{
		1, // source indication: 1 = normal application
		uint32(xproto.TimeCurrentTime),
		0, 0, 0,
	})
	ev := xproto.ClientMessageEvent{
		Format: 32,
		Window: win,
		Type:   x.netActiveWindow,
		Data:   data,
	}
	const evMask = xproto.EventMaskSubstructureRedirect | xproto.EventMaskSubstructureNotify
	xproto.SendEvent(x.conn, false, x.root, evMask, string(ev.Bytes()))

	// Kept alongside the EWMH message rather than replaced by it: harmless
	// on servers/compositors that do honor raw stacking requests, and
	// SetInputFocus is still how keyboard input actually gets routed to
	// win once it's the visible/active one.
	xproto.ConfigureWindow(x.conn, win, xproto.ConfigWindowStackMode, []uint32{xproto.StackModeAbove})
	xproto.SetInputFocus(x.conn, xproto.InputFocusParent, win, xproto.TimeCurrentTime)
}

// FocusWindowAndHideMad2 polls for a window whose title contains
// titleSubstr for a few seconds, then raises/focuses it and hides Mad2's
// own window ("Madagascar" title substring) so it stops being displayed
// while still running (D3D9 Present calls against an unmapped window don't
// error -- Wine/DXVK keeps happily rendering, it just isn't shown, which is
// all "hide" ever needed to mean here).
//
// Root-caused live (the "flashing triangles" bug, changes.md item 12/the
// user's Jak1-gamescope-glitch report): the original implementation here
// only moved Mad2's window off-screen (ConfigureWindow to 10000,10000) --
// a technique that assumes gamescope behaves like a spatial X11 desktop,
// where a window positioned outside the visible framebuffer simply isn't
// drawn. It doesn't: gamescope is a Wayland compositor (steamcompmgr) that
// tracks and composites app *surfaces*, not raw X11 window geometry --
// ConfigureWindow's X/Y is largely vestigial there, and gamescope has its
// own documented history of getting confused about which of several
// simultaneously-mapped windows to actually display/composite (see
// ValveSoftware/gamescope#437, "Dealing with multiple windows"). With Mad2
// merely repositioned (never unmapped), its D3D9 process keeps submitting
// full frames every frame, and gamescope still had a second live, mapped
// surface to reconcile against gk's own OpenGL surface -- consistent with
// two full 3D scenes' geometry visibly fighting for the same composited
// output ("flashing triangles"), not corruption in either game's own
// renderer (confirmed clean: gk's own log shows a normal OpenGL 4.6/AMD
// context init and no GL errors across a live run with Mad2 simultaneously
// active). Unmapping is the actual "stop compositing this" signal every
// X11/Wayland-surface model honors unconditionally, unlike raw geometry.
// Shared by sm64.go ("Super Mario 64") and jak1.go ("OpenGOAL", gk's SDL
// window title -- see jak1.go's call site).
//
// resizeTo, if non-zero, forcibly resizes the found window before raising
// it -- jak1.go needs this because gk has no --width/--height/--fullscreen
// CLI flags like sm64ex-alo's do (game/graphics/gfx.cpp hardcodes an
// initial 640x480 window; the real size only gets applied by GOAL menu
// code we skip entirely by booting straight into a level via -boot
// -fakeiso). sm64.go passes 0,0 (already sized correctly via its own CLI
// flags) to skip this.
func (x *X11) FocusWindowAndHideMad2(titleSubstr string, resizeTo [2]uint32) {
	for i := 0; i < 100; i++ {
		if win, ok := x.FindWindowByTitle(titleSubstr); ok {
			if mad2, ok := x.FindWindowByTitle("Madagascar"); ok {
				x.mad2Win = mad2
				x.mad2Hidden = true
				// Kept alongside the unmap (harmless, and a cheap defense
				// in depth): if some future gamescope version, or a
				// non-gamescope X11/WM environment this code also runs
				// under, DOES treat raw window position as compositing
				// input, this still helps; unmap is what actually matters.
				x.moveWindow(mad2, 10000, 10000)
				x.unmapWindow(mad2)
				logf("hid Madagascar 2 window (id=%d, unmapped + moved off-screen)", mad2)
			}
			if resizeTo[0] != 0 && resizeTo[1] != 0 {
				x.resizeWindow(win, resizeTo[0], resizeTo[1])
				logf("resized %q window (id=%d) to %dx%d", titleSubstr, win, resizeTo[0], resizeTo[1])
			}
			x.raiseAndFocus(win)
			logf("focused %q window (id=%d)", titleSubstr, win)
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	logf("gave up waiting for %q window to appear", titleSubstr)
}

// RestoreMad2 remaps Mad2's window, moves it back, and refocuses it once
// SM64/Jak1 exits.
func (x *X11) RestoreMad2() {
	if !x.mad2Hidden {
		return
	}
	x.mapWindow(x.mad2Win)
	x.sync() // ensure the map lands before raiseAndFocus's SetInputFocus -- see sync's comment
	x.moveWindow(x.mad2Win, 0, 0)
	x.raiseAndFocus(x.mad2Win)
	x.mad2Hidden = false
	logf("restored Madagascar 2 window")
}
