#include "../../crt/print.h"
#include "../../crt/sys.h"
#include "../../crt/constants.h"
#include "../../crt/debug.h"
#include "../../crt/string.h"
#include "../../crt/heap.h"
#include "../../crt/dirent.h"

int main(int argc, char** argv) {
  puts("Hello from test process!\n");

  // print args
  puts("Args:\n");
  for (int i = 0; i < argc; i++) {
    puts(argv[i]);
    puts("\n");
  }
  
  return 0;
}
