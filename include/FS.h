#include <xc.h>
#include <stdint.h>
#include "SerialComms.h"

void FS_task(void * params);
extern inline unsigned FS_isCardPresent();
uint8_t FS_dirUp(char * path);
char * FS_newCWD(char * oldPath, char * newPath);