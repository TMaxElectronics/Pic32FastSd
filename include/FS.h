#include <xc.h>
#include <stdint.h>

void FS_init();
uint8_t FS_dirUp(char * path);
char * FS_newCWD(char * oldPath, char * newPath);
uint32_t FS_clearPowerTimeout();