#ifndef IVT_H
#define IVT_H

// addresses of trap handler entry points

#define TRAP_IVT_ENTRY 0x004
#define INVALID_INSTR_IVT_ENTRY 0x200
#define PRIV_EXC_IVT_ENTRY 0x204
#define TLB_MISS_IVT_ENTRY 0x208
#define MISALIGNED_PC_IVT_ENTRY 0x210
#define PIT_IVT_ENTRY  0x3C0
#define PS2_IVT_ENTRY  0x3C4
#define UART_RX_IVT_ENTRY 0x3C8
#define SD_0_IVT_ENTRY 0x3CC
#define VGA_VBLANK_IVT_ENTRY 0x3D0
#define IPI_IVT_ENTRY  0x3D4
#define SD_1_IVT_ENTRY 0x3D8
#define AUDIO_IVT_ENTRY 0x3DC

// register the given handler function for the given IVT entry
void register_handler(void* func, void* ivt_entry);

// register spurious_interrupt_handler for all IVT entries to catch unexpected interrupts and exceptions
extern void register_spurious_handlers(void);

#endif // IVT_H
