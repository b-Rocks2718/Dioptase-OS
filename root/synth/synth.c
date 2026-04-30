#include "../crt/fcntl.h"
#include "../crt/print.h"
#include "../crt/stdlib.h"
#include "../crt/synth_audio.h"
#include "../crt/unistd.h"
#include "../crt/sys.h"

static void print_usage(char* program_name){
  int args[1];

  if (program_name == NULL){
    fdputs(STDERR, "usage: synth <file.dsyn>\n");
    return;
  }

  args[0] = (int)program_name;
  fdprintf(STDERR, "usage: %s <file.dsyn>\n", args);
}

static void print_path_error(char* action, char* path){
  int args[2];

  args[0] = (int)action;
  args[1] = (int)path;
  fdprintf(STDERR, "synth: %s: %s\n", args);
}

static void print_dsyn_error(char* path, int err){
  int args[3];

  args[0] = (int)path;
  args[1] = err;
  args[2] = (int)synth_audio_error_string(err);
  fdprintf(STDERR, "synth: %s: DSYN error %d: %s\n", args);
}

int main(int argc, char** argv){
  char* path;
  int fd;
  int rc;
  int close_rc;

  if (argc != 2){
    print_usage(argc > 0 ? argv[0] : NULL);
    return EXIT_FAILURE;
  }
  path = argv[1];

  fd = open(path);
  if (fd < 0){
    print_path_error("failed to open", path);
    return EXIT_FAILURE;
  }

  request_priority(DIOPTASE_PRIORITY_HIGH);
  rc = synth_audio_play_dsyn_fd(fd);
  request_priority(DIOPTASE_PRIORITY_NORMAL);
  close_rc = close(fd);

  if (close_rc < 0){
    print_path_error("failed to close", path);
    if (rc == DSYN_OK){
      return EXIT_FAILURE;
    }
  }

  if (rc != DSYN_OK){
    print_dsyn_error(path, rc);
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
