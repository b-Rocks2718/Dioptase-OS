/*
 * Boot the shipping SKYFORGE binary with its deterministic guest self-test.
 * Exercises exec, 5-sector 3D terrain generation, live crater deformation and
 * Terrain Forge sculpting, AI flight & combat, 3D Voxel-Space rendering,
 * display MMIO mappings, 25 kHz PCM audio synthesis, and ext2 save/load.
 */

#include "../../../root/crt/unistd.h"
#include "../../../root/crt/print.h"
#include "../../../root/crt/stddef.h"

/* Replace the test init image with the packaged SKYFORGE self-check. */
int main(void){
  char* args[3];
  args[0] = "skyforge";
  args[1] = "--selftest";
  args[2] = NULL;
  execv("/sbin/skyforge", 2, args);
  puts("***FAIL skyforge guest exec /sbin/skyforge\n");
  return 1;
}
