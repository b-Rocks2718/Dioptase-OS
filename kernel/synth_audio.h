#ifndef SYNTH_AUDIO_H
#define SYNTH_AUDIO_H

/*
 * Synth audio MMIO page.
 *
 * Hardware contract:
 * - docs/mem_map.md defines 0x7FBC000 - 0x7FBCFFF as the register page for
 *   the register-driven synth audio device.
 * - All registers in this page are 32-bit little-endian MMIO registers.
 * - This device is separate from the existing PCM ring-buffer audio path in
 *   audio.h and does not raise kernel interrupts.
 * - Version 4 of the device includes timestamped command-ring storage in the
 *   same page; the kernel still treats it as ordinary synth MMIO and only maps
 *   it.
 *
 * Kernel usage:
 * - The kernel does not virtualize individual synth registers today. It maps
 *   the whole page into user space through the get_synth_audio() syscall, just
 *   like the framebuffer helper syscalls map VGA MMIO regions.
 */
#define SYNTH_AUDIO_BASE 0x7FBC000
#define SYNTH_AUDIO_SIZE 0x1000

#endif // SYNTH_AUDIO_H
