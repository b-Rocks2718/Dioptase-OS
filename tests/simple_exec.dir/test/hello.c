#include "../../../root/crt/print.h"
#include "../../../root/crt/sys.h"

int main(int argc, char** argv) {
  puts("***hello from execed program\n");
  puts("***argc = ");
  print_signed(argc);
  puts("\n");
  for (int i = 0; i < argc; i++){
    puts("***argv[");
    print_signed(i);
    puts("] = ");
    puts(argv[i]);
    puts("\n");
  }
  return 42;
}
