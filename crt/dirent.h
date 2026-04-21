#ifndef DIRENT_H
#define DIRENT_H

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14

struct linux_dirent {
    unsigned d_ino; // i-node number.
    unsigned d_off;
    unsigned short d_reclen; // Length of this record.
    char d_name; // Filename (null-terminated array).
    // char pad;
    // char d_type; // Offset is (d_reclen - 1).
};

#endif // DIRENT_H
