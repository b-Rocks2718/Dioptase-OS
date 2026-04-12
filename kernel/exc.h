#ifndef EXC_H
#define EXC_H

// register exception handlers
void exc_init(void);

extern void invalid_instr_exc_handler_(void);
extern void priv_exc_handler_(void);
extern void misaligned_pc_exc_handler_(void);

#endif // EXC_H
