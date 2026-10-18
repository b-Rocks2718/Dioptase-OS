# Controller and projected-platform verification

2026-10-17: the new movement core is tested before integration into Opus's
renderer. Relevant baseline framebuffer/input tests passed 4/4 before work.
Native syntax and bcc compilation of both modules pass. bcc rejects comma
declarations inside structs; splitting members fixes this without a compiler
change or layout change.

The native/guest suite in `tests/test_core.c` covers collision edges, movement
feel bounds, recording lifetime and playback, and a 6000-tick mixed trace.
Specific concern to check: a sideways carry clipped by a wall must not regain
the same attachment during the subsequent support probe. Likewise an upward
projection cannot embed its rider in a ceiling. Record results and fixes here.

First sanitizer run: all focused cases and the long nonpenetration trace
passed except the predicted sideways-carry reattachment. A platform wide
enough to remain beneath the clipped rider was reacquired by `support` in the
same tick. Fix: carry resolution returns the obstructed platform id, and both
support probes and landing ignore that id for the remainder of that tick.
The projection still follows its recording. No public state or API changed.

After the fix: native AddressSanitizer/UndefinedBehaviorSanitizer suite passes.
The same source, compiled in separate modules by optimized bcc and executed
by the full Dioptase emulator, passes all cases and matches native trace
3239651184 exactly. The trace is a parity check, not a substitute for the
independent collision assertions. The four existing OS framebuffer/input
tests pass again. Commands:

- `make -C root/retake/tests test`
- `make -j4 retake_core.summary-test user_pixel_fb.summary-test user_foreground_display.summary-test mouse_syscall.summary-test user_line_editor.summary-test TEST_RUNS=1`

The guest fixture is the only addition outside `root/retake`: it connects the
same suite to the existing OS harness without changing kernel or toolchain
behavior. `/tmp/astra-retake-core-guest.log` contains this verification run.
