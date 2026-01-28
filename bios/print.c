#include "print.h"
#include "machine.h"

int print_lock = 0;

int puts(char* str){
  int count = 0;
  while (*str != '\0'){
    putchar(*str);
    ++str;
    ++count;
  }
  return count;
}

int print_num(int n){
  if(n == 0){
      putchar('0');
      return 1;
  }
  if(n < 0){
      putchar('-');
      n = -n;
  }

  int d = n % 10;
  n = n / 10;
  
  int count = 1;

  if (n != 0){
    count += print_num(n);
  } 
  putchar('0' + d);
  
  // return number of characters printed
  return count;
}