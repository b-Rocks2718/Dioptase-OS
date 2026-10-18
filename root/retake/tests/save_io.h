#ifndef RETAKE_SAVE_IO_H
#define RETAKE_SAVE_IO_H
/* Native fault-injection boundary; only save.c's guest file calls are replaced. */
int saveio_open(char* path);
int saveio_existing(char* path);
int saveio_read(int fd, void* dst, unsigned size);
int saveio_write(int fd, void* src, unsigned size);
int saveio_close(int fd);
int saveio_seek(int fd, int offset, int whence);
int saveio_truncate(int fd, unsigned size);
extern int saveio_errno;
#define errno saveio_errno
#define ENOENT 2
#define open saveio_open
#define open_existing saveio_existing
#define read saveio_read
#define write saveio_write
#define close saveio_close
#define seek saveio_seek
#define truncate saveio_truncate
#endif
