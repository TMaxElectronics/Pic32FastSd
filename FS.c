#include <xc.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/attribs.h>
#include "FreeRTOS.h"
#include "SPI.h"
#include "FS.h"
#include "diskio.h"
#include "ff.h"
#include "TTerm.h"
#include "diskioConfig.h"
#include "System.h"

//#define DEBUG

typedef enum {SD_NOT_PRESENT, SD_LOW_POWER, SD_READY, SD_ERROR} FSState_t;
typedef enum {FSCMD_TIMEOUT = 0, FSCMD_SD_ACCESSED, FSCMD_GO_LP, FSCMD_IOEVT} FSCMD_t;

static uint8_t FS_testCommand(TERMINAL_HANDLE * handle, uint8_t argCount, char ** args);

QueueHandle_t sdQueue; //TODO init
SemaphoreHandle_t sdCMD; //TODO init
static volatile FSState_t currState = SD_NOT_PRESENT;
static void FS_task(void * params);
    

static void goLowPower(SPIHandle_t * handle){
    //power down spi module
    handle->CON->ON = 0;
    TRISBSET = _LATB_LATB10_MASK | _LATB_LATB11_MASK | _LATB_LATB13_MASK;
    
    //power down sd card and drop vdd to 2.3V
    LATBCLR = _LATB_LATB5_MASK;
    
    //delay until sd card power is ready. This MUST be blocking to not return to the sd comms code
    //SYS_waitCP0(1);
}

static void goHighPower(SPIHandle_t * handle){
    //power down spi module
    handle->CON->ON = 1;
    TRISBCLR = _LATB_LATB10_MASK | _LATB_LATB11_MASK | _LATB_LATB13_MASK;
    
    //power down sd card and drop vdd to 2.3V
    LATBSET = _LATB_LATB5_MASK;
    
    //delay until sd card power is ready. This MUST be blocking to not return to the sd comms code
    //SYS_waitCP0(1);
}

static uint32_t initSD(SPIHandle_t * handle){
    //try to init the card a couple of times
    for(uint32_t attemptCounter = 0; attemptCounter < 5; attemptCounter++){
        if(disk_initialize(0) == 0){
            //init successful, return
            return 1;
        }
        //init failed... cycle sd card power and try again
        goLowPower(handle);
        goHighPower(handle);
    }
    //init failed 5 times, give up and return
    TERM_printDebug(TERM_handle, "ERROR: SD Card init failed\r\n");
    return 0;
}

void __ISR(_CHANGE_NOTICE_VECTOR) FS_cnISR(){
    //clear flag
    uint32_t trash = PORTB;
    IFS1CLR = _IFS1_CNAIF_MASK;
    
    //disable cn, this is to prevent contact bouncing overloading the cpu with interrupts
    IEC1CLR = _IEC1_CNAIE_MASK;
    
    //send cmd to fs task queue
    FSCMD_t cmd = FSCMD_IOEVT;
    xQueueSendFromISR(sdQueue, &cmd, 0);
}

uint32_t FS_clearPowerTimeout(){
    //first try to take the command semaphore
    /*if(!xSemaphoreTake(sdCMD, 0)){
        //didn't get it, somebody else is already waiting for an op. Cancel it for now
        return 0;
    }*/
    
    uint32_t csState = LATB & _LATB_LATB10_MASK;
    
    FSCMD_t cmd = FSCMD_SD_ACCESSED;
    xQueueSend(sdQueue, &cmd, 0);
    
    /*if(!xSemaphoreTake(sdCMD, FS_SD_ACCESS_TIMEOUT)){
        //cmd timed out...
        return 0;
    }
    xSemaphoreGive(sdCMD);
    */
    
    LATBbits.LATB10 = csState;
    
    return currState == SD_READY;
}

void FS_init(SPIHandle_t * spiHandle){
    sdQueue = xQueueCreate(2, sizeof(FSCMD_t));
    sdCMD = xSemaphoreCreateBinary();
    
    //sd card cs
    LATBSET = _LATB_LATB10_MASK;
    //TRISBCLR = _LATB_LATB10_MASK | _LATB_LATB11_MASK | _LATB_LATB15_MASK;
    
    SPI_setCLKFreq(spiHandle, 400000);
    
    //init change notice
    CNCONB = _CNCONB_ON_MASK;
    CNPUBSET = _CNPUB_CNPUB9_MASK;
    CNENBSET = _CNENB_CNIEB9_MASK;
    
    IPC8bits.CNIP = 3;
    IEC1SET = _IEC1_CNAIE_MASK;
    
    xTaskCreate(FS_task, "fs Task", configMINIMAL_STACK_SIZE + 200, spiHandle, tskIDLE_PRIORITY + 4, NULL);
    
    FSCMD_t cmd = FSCMD_IOEVT;
    if(FS_isCardPresent()) xQueueSend(sdQueue, &cmd, 0);
}

static void FS_task(void * params){
    SPIHandle_t * handle = (SPIHandle_t *) params;
    disk_setSPIHandle(handle);
    
    //FATFS fso;
    FATFS * fso = pvPortMalloc(sizeof(FATFS));
    
    FSCMD_t currCMD;
    
    while(1){
        //wait until we get notified of an event
        //Timeout depends on the state the machine is in, if the card is powered up we need to have a timeout
        if(!xQueueReceive(sdQueue, &currCMD, (currState == SD_READY) ? FS_SD_ACCESS_TIMEOUT : portMAX_DELAY)) currCMD = FSCMD_TIMEOUT; //peek timed out => set error flag
        
        //now process the event
        
        //first of all check for sd card presence
        if(FS_isCardPresent()){
            //sd card is present, was it during the last cycle?
            if(currState == SD_NOT_PRESENT){
                //nope, we need to init it
                goHighPower(handle);
                
                if(initSD(handle)){
                    //init successful
                    
                    //mount card
                    f_mount(fso, "", 0);
                    f_chdir("/");
                    
                    //now check what the command was
                    if(currCMD != FSCMD_SD_ACCESSED){
                        //cmd was NOT an access request => go to sleep again
                        goLowPower(handle);
                        currState = SD_LOW_POWER;
                    }else{
                        //cmd was an access request, don't go low power and just switch state
                        currState = SD_READY;
                    }
                }else{
                    //init failed :(
                    goLowPower(handle);
                    currState = SD_ERROR;
                }
                
            }else{
                //yes, present and initialised. Now check if its asleep
                if(currState == SD_LOW_POWER){
                    //wakeup the card an init it if the command is an access request
                    if(currCMD == FSCMD_SD_ACCESSED){
                        goHighPower(handle);

                        if(initSD(handle)){
                            //success :)
                            currState = SD_READY;
                        }else{
                            //failed  :(
                            goLowPower(handle);
                            currState = SD_ERROR;
                        }
                    }
                }else{
                    //sd card is present and powered up, check if we got a cmd to sleep or a timeout occurred. Otherwise just go back to the timeout
                    if(currCMD == FSCMD_GO_LP || currCMD == FSCMD_TIMEOUT){
                        //go back to low power mode
                        goLowPower(handle);
                        currState = SD_LOW_POWER;
                    }
                }
            }
        }else{
            //no card present, do we need to de-init anything?
            if(currState != SD_NOT_PRESENT){
                //yes, do so
                goLowPower(handle);
                f_mount(NULL, "", 0);
                currState = SD_NOT_PRESENT;
            }else; //nope, statemachine is already in the correct state
        }
        
        xSemaphoreGive(sdCMD);
        
        //re-enable cn in case it was disabled
        IEC1SET = _IEC1_CNAIE_MASK;
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