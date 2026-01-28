#ifndef PRINT_H
#define PRINT_H

extern int print_lock;

// print the number n to the console in decimal
// returns the number of characters printed
int print_num(int n);

int puts(char* str);

#endif // PRINT_H