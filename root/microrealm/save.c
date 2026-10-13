#include "realm.h"
#include "../crt/fcntl.h"
#include "../crt/unistd.h"

#define RECORD_FILE_PATH "/microrealm_save.dat"

/* Load royal campaign high records from persistent SD filesystem storage. */
void save_load_record(struct HighScore* record) {
  record->best_days = 1;
  record->best_score = 0;
  record->best_kills = 0;

  int fd = open_existing(RECORD_FILE_PATH);
  if (fd >= 0) {
    int bytes = read(fd, record, sizeof(struct HighScore));
    if (bytes != sizeof(struct HighScore)) {
      record->best_days = 1;
      record->best_score = 0;
      record->best_kills = 0;
    }
    close(fd);
  }
}

/* Save new royal campaign record to persistent SD storage if score exceeds record. */
void save_write_record(struct HighScore* record) {
  int fd = open(RECORD_FILE_PATH);
  if (fd >= 0) {
    write(fd, record, sizeof(struct HighScore));
    close(fd);
  }
}
