#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

#include "../crt/stdbool.h"

char** preprocess(int num_files, int* file_names, bool has_start,
  char **argv, char **files);

#endif  // PREPROCESSOR_H
