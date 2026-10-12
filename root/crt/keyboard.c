#include "errno.h"
#include "string.h"
#include "sys.h"
#include "terminal.h"
#include "unistd.h"

/*
 * Raw keyboard mode for programs running under the terminal. The mode is
 * terminal state, not kernel state: set_keyboard_raw() writes the mode
 * request and a keyboard sync request to the terminal (docs/terminal.md) and
 * then reads STDIN until the sync reply, so the switch is exact.
 */

// How long set_keyboard_raw() waits for the sync reply: one second of the
// kernel's 3,000-Hz jiffies. The terminal reads program output every few
// jiffies, so a reply normally arrives quickly; the margin covers a terminal
// kept off the CPU by busy cores.
#define KEYBOARD_SYNC_TIMEOUT_JIFFIES 3000

// Switch the terminal between cooked (0) and raw (1) keyboard input and wait
// until it has done so. Everything this process reads from STDIN afterwards
// is in the requested form: input forwarded before the switch, which would be
// in the old form, is read and discarded here. Returns 0 on success.
//
// Returns -1 with errno EINVAL for a mode other than 0 or 1. Returns -1
// without setting errno when no terminal answers: STDIN or STDOUT is not a
// pipe (the process was not started by the shell under the terminal), or no
// reply arrived within KEYBOARD_SYNC_TIMEOUT_JIFFIES. In the timeout case the
// request was sent, so a terminal that is merely slow may still switch later.
//
// Must not run while another thread or process of the program reads STDIN,
// since that reader could consume the reply.
int set_keyboard_raw(int enabled){
  if (enabled != 0 && enabled != 1){
    errno = EINVAL;
    return -1;
  }
  if (fd_bytes_available(STDIN) < 0 || fd_bytes_available(STDOUT) < 0){
    return -1;
  }

  char* mode = enabled ? TERMINAL_RAW_KEYBOARD_ON : TERMINAL_RAW_KEYBOARD_OFF;
  char* sync = TERMINAL_KEYBOARD_SYNC;
  if (write(STDOUT, mode, strlen(mode)) < 0 ||
      write(STDOUT, sync, strlen(sync)) < 0){
    return -1;
  }

  // Read one byte at a time so nothing after the reply is consumed.
  unsigned start = get_current_jiffies();
  int matched = 0;
  while (matched < TERMINAL_KEYBOARD_SYNC_BYTES){
    int avail = fd_bytes_available(STDIN);
    if (avail < 0){
      return -1;
    }
    if (avail == 0){
      if (get_current_jiffies() - start >= KEYBOARD_SYNC_TIMEOUT_JIFFIES){
        return -1;
      }
      sleep(1);
      continue;
    }
    unsigned char byte;
    if (read(STDIN, &byte, 1) != 1){
      return -1;
    }
    matched = (byte == TERMINAL_KEYBOARD_SYNC_REPLY_BYTE) ? matched + 1 : 0;
  }
  return 0;
}
