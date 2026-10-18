# Recoverable-save validation

2026-10-18 baseline: existing native core/game tests passed before adding the
save codec and fault injection. Read docs/syscalls.md Filesystem and Pipes /
Error Reporting, docs/filesystem.md Regular File Reads/Writes and Rename,
root/crt errno/fcntl/unistd contracts, and the canonical ABI's 32-bit word and
4-byte alignment rules. Writes are not promised atomic; power-loss durability
is unspecified. Only one user process may write this game's saves at a time.

Opus and Astra both started save hardening before queued ownership messages
arrived. Opus committed an in-place version as07c37b7. Astra has not edited
game.c and proposed using the separately tested codec via small wrappers.
The in-place version leaves sequence metadata outside its checksum, relies on
a process-local sequence to choose a slot before reading old saves, rejects
all health above9 despite frame pickups, and does not check checkpoint tiles.

The first every-byte overwrite test exposed a test assumption, not a torn
record: if the untouched suffix already equals the requested suffix, an
interrupted write can nevertheless leave the exact complete new record.
Recovery must therefore accept the exact old or exact new checkpoint while
proving the previous latest slot remains byte-for-byte intact. It must never
accept mixed progress. Successful UI feedback still requires full writes,
close and readback to succeed. The test is corrected to this stronger actual
contract, not to allow arbitrary values after a failed save.

The native codec suite now passes under ASan/UBSan: every write-byte boundary
for both a fresh slot and an overwrite, every corrupted/truncated byte of the
newest slot, malformed checksummed progress, incompatible worlds, serial wrap,
one-byte transfers, I/O failures, and unchanged output on rejection. The full
game builds with bcc/basm (codec linked but wrappers still owned by Opus).

Before the new guest fixture, file_syscalls and user_fs_mutation_syscalls
passed. After adding tests/retake_save.dir, all three guest tests pass. The
new fixture runs the actual codec against real file syscalls with a small
room-data fixture: create two slots, select newest, truncate newest, recover
previous, reject an invalid checkpoint, preserve output when both slots fail.
bcc rejects a global pointer initializer naming a static array; assigning it
in main fixes the fixture without a compiler change. Only the new fixture's
Makefile/init.c and expected .ok are outside the demo.
