#include "dirs.h"
#include "../../../crt/print.h"
#include "../../../crt/heap.h"
#include "../../../crt/sys.h"

#define ENTRIES_PER_LINE 4
#define SPACES_PER_TAB 8

// Probably have these in CRT. They seem to be in kernel/string.h right now.
unsigned strlen(char* str) {
    unsigned len = 0;
    while (str[len] != '\0') {
        len++;
    }
    return len;
}

void* memcpy(void* dest, void* src, unsigned n) {
    char* src_char = (char*) src;
    char* dest_char = (char*) dest;
    for (unsigned i = 0; i < n; i++) {
        dest_char[i] = src_char[i];
    }
    return dest;
}

// Combines with '/'. Adds null terminator.
char* combine_path(char* base_path, char* rest, unsigned base_length, unsigned rest_length) {
    char has_base = base_length != 0;
    char* full_path = (char*) malloc(base_length + rest_length + 1 + has_base); // 0 and '/'.
    memcpy(full_path + base_length + has_base, rest, rest_length);
    if (has_base) {
        memcpy(full_path, base_path, base_length);
        full_path[base_length] = '/';
    }
    full_path[base_length + rest_length + has_base] = 0;
    return full_path;
}

struct LinkedDirent *create_linked_dirent(struct linux_dirent *dirent) {
    struct LinkedDirent *entry = (struct LinkedDirent*) malloc(sizeof(struct LinkedDirent) + dirent->d_reclen - sizeof(struct linux_dirent));
    memcpy(&entry->dirent, dirent, dirent->d_reclen);
    entry->d_type = *((char*)dirent + dirent->d_reclen - 1);
    entry->next = 0;
    return entry;
}

void destroy_linked_dirents(struct LinkedDirent *head) {
    struct LinkedDirent *current = head;
    while (current != 0) {
        struct LinkedDirent *next = current->next;
        free(current);
        current = next;
    }
}

struct LinkedDirent *read_directory(char* path) {
    int fd = open(path);
    if (fd < 0) {
        return 0;
    }

    char* buffer = (char*) malloc(BUFFER_SIZE);
    struct LinkedDirent *head = 0;
    struct LinkedDirent *tail = 0;
    while (1) {
        int n = getdents(fd, buffer, BUFFER_SIZE);
        int argshi[1] = {(int) n};
        printf("***getdents returned %d.\n", argshi);
        if (n < 0) {
            close(fd);
            destroy_linked_dirents(head);
            free(buffer);
            return 0;
        }
        if (n == 0) {
            // No more entries.
            break;
        }

        int offset = 0;
        while (offset < n) {
            struct linux_dirent* dirent = (struct linux_dirent*) (buffer + offset);
            struct LinkedDirent* new_entry = create_linked_dirent(dirent);
            if (head == 0) {
                head = new_entry;
                tail = new_entry;
            } else {
                tail->next = new_entry;
                tail = new_entry;
            }
            offset += dirent->d_reclen;

            int args[1] = {(int) &dirent->d_name};

            printf("***Found entry: %s\n", args);
            printf("***  Type: ", NULL);
            switch (new_entry->d_type) {
            case DT_REG:
                printf("Regular file.\n", NULL);
                break;
            case DT_DIR:
                printf("Directory.\n", NULL);
                break;
            case DT_LNK:
                printf("Symbolic link.\n", NULL);
                char link[BUFFER_SIZE];
                char* combined_path = combine_path(path, (char*) &dirent->d_name, strlen(path), strlen((char*) &dirent->d_name));
                readlink(combined_path, link, BUFFER_SIZE);
                free(combined_path);
                int args[1] = {(int) link};
                printf("***  To: %s\n", args);
                break;
            default:
                printf("Other.\n", NULL);
                break;
            }
        }
    }
    close(fd);
    free(buffer);
    return head;
}
