#include <xc.h>
#include <stdint.h>
#include "SPI.h"

void FS_init(SPIHandle_t * spiHandle);
uint8_t FS_dirUp(char * path);
char * FS_newCWD(char * oldPath, char * newPath);
uint32_t FS_clearPowerTimeout();