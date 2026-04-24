#include "../crt/sys.h"
#include "../crt/print.h"

// Starts counting from 30, and assumes no skips.
char* color_names[20] = {
  "black",
  "red",
  "green",
  "yellow",
  "blue",
  "magenta",
  "cyan",
  "white",
  "light gray",
  "darker gray",
  "orange",
  "bright pink",
  "bright green",
  "bright yellow",
  "bright blue",
  "purple",
  "indigo",
  "light orange",
  "light yellow",
  "gold"
};

int main(void) {
  puts("\n");
  for (int i = 0; i < sizeof(color_names) / sizeof(char*); i++) {
    int args[3] = {30 + i, 30 + i, (int)color_names[i]};
    printf("\x1b[%dm%d: %s\n", args);
  }
  puts("\x1b[37m\n"); // Reset to white.

  return 0;
}
