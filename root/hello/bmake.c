#include "../crt/unistd.h"
#include "../crt/stdio.h"
#include "../crt/stdlib.h"
#include "../crt/sys/wait.h"

int main(void){
  puts("Compiling...\n");
  
  int id = fork();
  if (id == 0){
    // exec compiler
    char* argv[5] = {"/sbin/bcc", "-s", "hello.c", "-o", "hello.s"};
    execv("/sbin/bcc", 5, argv);

    puts("execv failed\n");
    return -1;
  } 

  int rc = wait_child(id);
  if (rc < 0){
    puts("Compilation failed\n");
    return -1;
  }

  puts("Assembling...\n");
  
  id = fork();
  if (id == 0){
    // exec assembler
    char* argv[8] = {"/sbin/basm", "-bin", "crt0.s", "arithmetic.s", "stdio.s", "hello.s", "-o", "hello"};
    execv("/sbin/basm", 8, argv);

    puts("execv failed\n");
    return -1;
  }

  wait_child(id);
}