package main

import (
	"runtime"
	"syscall"
)

// linux/prctl.h's PR_SET_PDEATHSIG -- not exposed as a named constant by
// the standard syscall package, hand-duplicated here (it's a small, stable
// ABI value, same "hand duplicate rather than add a dependency" precedent
// as every wire-protocol constant elsewhere in this repo).
const prSetPdeathsig = 1

// armParentDeathSignal asks the kernel to deliver SIGTERM to this process
// the instant its parent (gamescope, per justfile's _play recipe) exits,
// by whatever means -- clean exit, crash, or a SIGKILL from the window
// being closed. Without this, if gamescope's own child-teardown doesn't
// happen to reach mad2relauncher first, this process is silently
// reparented to init and keeps running forever: its crash-relaunch loop
// (gameloop.go) never gets told to stop, and worse, if e.g. mad2music
// happens to exit around the same moment (music.go's own crash-relaunch
// racing against the death of its supervisor), it gets relaunched into a
// gamescope session that's already gone -- both symptoms were observed in
// practice before this was added.
//
// PR_SET_PDEATHSIG is a per-*thread* kernel property, tied to whichever OS
// thread made the prctl call, and fires when THAT thread's parent process
// exits. LockOSThread pins the calling goroutine (main(), called before
// anything else starts) to the OS thread that makes this call for the
// rest of the process's life, since main() itself blocks in loop.Run()
// until shutdown rather than returning early.
func armParentDeathSignal() {
	runtime.LockOSThread()
	syscall.Syscall(syscall.SYS_PRCTL, prSetPdeathsig, uintptr(syscall.SIGTERM), 0)
}
