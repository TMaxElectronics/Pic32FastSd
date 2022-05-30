#include <xc.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "SPI.h"
#include "FS.h"
#include "diskio.h"
#include "ff.h"

//#define DEBUG

void FS_task(void * params){
    SPI_HANDLE * handle = (SPI_HANDLE *) handle;
    disk_setSPIHandle(handle);
    
    //FATFS fso;
    FATFS * fso = pvPortMalloc(sizeof(FATFS));
    
    unsigned SDState = 0;
    unsigned SDintialized = 0;
    
    while(1){
        unsigned currState = FS_isCardPresent();
        if(currState != SDState){
            SDState = currState;
            if(SDState){        //sd card was just connected
                vTaskDelay(50);
#ifdef FS_SD_EVENT_HANDLER
				FS_SD_EVENT_HANDLER(FS_SD_CONNECTED);
#endif
            }else{              //sd card was just removed
#ifdef FS_SD_EVENT_HANDLER
				FS_SD_EVENT_HANDLER(FS_SD_DISCONNECTED);
#endif
                SDintialized = 0;
                f_mount(NULL, "", 0);
            }
        }
        
        if(SDState && !SDintialized){
            if(disk_initialize(0) == 0){
                SDintialized = 1;
                f_mount(fso, "", 0);
                f_chdir("/");
#ifdef FS_SD_EVENT_HANDLER
				FS_SD_EVENT_HANDLER(FS_SD_INIT_SUCCESSFUL);
#endif
            }else{
#ifdef FS_SD_EVENT_HANDLER
				FS_SD_EVENT_HANDLER(FS_SD_INIT_FAILED);
#endif
            }
        }
        
        vTaskDelay(50/portTICK_PERIOD_MS);
    }
}

char * FS_newCWD(char * oldPath, char * newPath){
    uint8_t count = 0;
    
    char * cp = newPath;
    while(*(cp) != 0){
        if(*cp == '/') count ++;
        cp++;
    }
    
    count ++;
    char ** pathParts = pvPortMalloc(sizeof(char *) * count);
    
    cp = newPath;
    uint8_t currPart = 0;
    pathParts[0] = newPath;
    while(*(cp) != 0){
        if(*cp == '/'){
            pathParts[++currPart] = cp + 1;
            *cp = 0;
        }
        cp++;
    }
    
    currPart = 0; 
    char * temp = pvPortMalloc(FF_MAX_LFN+1);
    unsigned slashSkip = 0;
    
    if(*pathParts[currPart] != 0){  //the first position is a valid name -> append to current path
        strncpy(temp, oldPath, FF_MAX_LFN);
        uint16_t len = strlen(temp);
        if(temp[len - 1] != '/'){
            temp[len] = '/';
            temp[len + 1] = 0;
        }
        slashSkip = 1;
    }else{
        strcpy(temp, "/");
        slashSkip = 1;
        currPart++;
    }
    
    for(; currPart < count; currPart++){
        if(strcmp(pathParts[currPart], "..") == 0){
            FS_dirUp(temp);
            slashSkip = 1;
        }else{
            sprintf(temp, "%s%s%s", temp, slashSkip ? "" : "/", pathParts[currPart]);
            slashSkip = 0;
        }
    }
    
    uint16_t len = strlen(temp);
    
    char * ret = pvPortMalloc(len + 1);
    strcpy(ret, temp);
    
    vPortFree(temp);
    vPortFree(pathParts);
    
    return ret;
}

uint8_t FS_dirUp(char * path){
    uint16_t currPos = strlen(path) - 1;
    
    if(currPos == 0) return 0;
    
    if(path[currPos] == '/'){
        path[currPos--] = 0;
    }
    
    while(path[currPos] != '/'){
        currPos --;
        if(currPos == 0) break;
    }
    
    path[currPos+1] = 0;
    
    return 1;
}

inline unsigned FS_isCardPresent(){
    return 0;
}