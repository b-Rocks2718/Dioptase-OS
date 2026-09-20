#ifndef DIRS_H
#define DIRS_H

#include "../../../root/crt/dirent.h"
#include "../../../root/crt/string.h"

#define BUFFER_SIZE 1024

struct LinkedDirent { /* Link one directory-entry record into the test's expected listing. */
    struct LinkedDirent *next;
    char d_type;
    struct linux_dirent dirent;
};

struct LinkedDirent *create_linked_dirent(struct linux_dirent *dirent);
void destroy_linked_dirents(struct LinkedDirent *head);

struct LinkedDirent *read_directory(char *path);

#endif // DIRS_H
