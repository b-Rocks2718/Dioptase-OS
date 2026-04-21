#include "../../../crt/print.h"

int main(int argc, char** argv){
  puts("***unexpected exec success\n");
  puts("***argc = ");
  print_signed(argc);
  puts("\n");
  if (argc > 0){
    puts("***argv[0] = ");
    puts(argv[0]);
    puts("\n");
  }
  return 99;
}
