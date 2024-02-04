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
static TaskHandle_t FS_taskHandle;
    

static void goLowPower(SPIHandle_t * handle){
    //power down spi module
    handle->CON->ON = 0;
    TRISBSET = _LATB_LATB10_MASK | _LATB_LATB11_MASK | _LATB_LATB15_MASK;
    
    //power down sd card and drop vdd to 2.3V
    LATBCLR = _LATB_LATB5_MASK;
    disk_uninitialize(0);
                        //TERM_printDebug(TERM_handle, "went low power\r\n");
}

static void goHighPower(SPIHandle_t * handle){
    
    //power down sd card and drop vdd to 2.3V
    LATBSET = _LATB_LATB5_MASK;
                        //TERM_printDebug(TERM_handle, "went high power\r\n");
    
    //delay until sd card power is ready. This MUST be blocking to not return to the sd comms code
    SYS_waitCP0(1);
    
    //power down spi module
    handle->CON->ON = 1;
    TRISBCLR = _LATB_LATB10_MASK | _LATB_LATB11_MASK | _LATB_LATB15_MASK;
    disk_uninitialize(0);
}

static uint32_t initSD(SPIHandle_t * handle){
    //try to init the card a couple of times
    for(uint32_t attemptCounter = 0; attemptCounter < 5; attemptCounter++){
        //TERM_printDebug(TERM_handle, "start init attempt %d\r\n", attemptCounter);
        if(disk_initialize(0) == 0){
            //init successful, return
            //TERM_printDebug(TERM_handle, "veri nais, king in the castle\r\n");
            return 1;
        }
        //init failed... cycle sd card power and try again
        //goLowPower(handle);
        //goHighPower(handle);
        vTaskDelay(100);
    }
    //init failed 5 times, give up and return
    //TERM_printDebug(TERM_handle, "ERROR: SD Card init failed\r\n");
    return 0;
}

void __ISR(_CHANGE_NOTICE_VECTOR) FS_cnISR(){
    //clear flag
    uint32_t trash = PORTB;
    IFS1CLR = _IFS1_CNBIF_MASK;
    
    //disable cn, this is to prevent contact bouncing overloading the cpu with interrupts
    IEC1CLR = _IEC1_CNBIE_MASK;
    
    //send cmd to fs task queue
    FSCMD_t cmd = FSCMD_IOEVT;
    xQueueSendFromISR(sdQueue, &cmd, 0);
}

uint32_t FS_clearPowerTimeout(){
    //check if the calling task is the FS_TASK, if so we obviously must not wait for command completion
    if(xTaskGetCurrentTaskHandle() == FS_taskHandle) return 1;
    
    //is there anything to wait for?
    if(currState != SD_READY){
        //yes, card isn't ready yet. Schedule a command and wait for it to be finished
        //uint32_t csState = LATB & _LATB_LATB10_MASK;
        
        //try to take the semaphore
        if(!xSemaphoreTake(sdCMD, FS_SD_ACCESS_TIMEOUT)){
            //hmm failed, some other task must be waiting for a command to run too
            TERM_printDebug(TERM_handle, "fs command que timeout!\r\n");
            return 0;
        }
        
        //send command
        FSCMD_t cmd = FSCMD_SD_ACCESSED;
        xQueueSend(sdQueue, &cmd, 0);
            //TERM_printDebug(TERM_handle, "fs command queued\r\n");

        //now try to take the semaphore again, this will only work once the command ran and fs_task returned it
        if(!xSemaphoreTake(sdCMD, FS_SD_ACCESS_TIMEOUT)){
            //cmd timed out...
            TERM_printDebug(TERM_handle, "fs command timeout!!!!\r\n");
            return 0;
        }
        //now return the semaphore
        xSemaphoreGive(sdCMD);

        //LATBbits.LATB10 = csState;
    }else{
        //no, just renew the timeout
        
        FSCMD_t cmd = FSCMD_SD_ACCESSED;
        xQueueSend(sdQueue, &cmd, 0);
    }   
    
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
    IEC1SET = _IEC1_CNBIE_MASK;
    
    xTaskCreate(FS_task, "fs Task", configMINIMAL_STACK_SIZE + 200, spiHandle, tskIDLE_PRIORITY + 4, &FS_taskHandle);
    
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
        if(!xQueueReceive(sdQueue, &currCMD, (currState == SD_READY || currState == SD_ERROR) ? FS_SD_ACCESS_TIMEOUT : portMAX_DELAY)) currCMD = FSCMD_TIMEOUT; //peek timed out => set error flag
        
        //now process the event
        //TERM_printDebug(TERM_handle, "event occured! id=%d\r\n", currCMD);
        
        //check which event was received

        if(currCMD == FSCMD_IOEVT){
            //sd card was just connected or removed
            
            //wait a bit for debounce
            vTaskDelay(10);
            
            //was it connected or disconnected?
            if(FS_isCardPresent()){
                //connected
                TERM_printDebug(TERM_handle, "card was connected\r\n");
                
                //does the fs know about it?
                if(currState == SD_NOT_PRESENT){
                    //no, mount it (but don't initialize it yet!)
                    f_mount(fso, "", 0);
                    currState = SD_LOW_POWER;
                //TERM_printDebug(TERM_handle, "card was mounted\r\n");
                }
            }else{
                //disconnected
                TERM_printDebug(TERM_handle, "card was disconnected\r\n");
                
                //does the fs know about it?
                if(currState != SD_NOT_PRESENT){
                    //no, unmount it
                    f_mount(NULL, "", 0);
                    currState = SD_NOT_PRESENT;
                    goLowPower(handle);
                //TERM_printDebug(TERM_handle, "card was unmounted\r\n");
                }
            }
            
        }else if(currCMD == FSCMD_SD_ACCESSED){
            //card was accessed, check if its ready
            if(currState == SD_LOW_POWER){
                        //TERM_printDebug(TERM_handle, "powering up card\r\n");
                        
                //card isn't ready, power it up and initialize
                goHighPower(handle);
                
                if(initSD(handle)){
                    //init succeeded
                    currState = SD_READY;
                        //TERM_printDebug(TERM_handle, "succcccccess\r\n");
                }else{
                    //init failed :( power down the card again and set error state
                    goLowPower(handle);
                    currState = SD_ERROR;   //error state will remain until write timeout occurs
                    TERM_printDebug(TERM_handle, "sd init failure :( locking out until timeout\r\n");
                }
            }
        }else if(currCMD == FSCMD_GO_LP || currCMD == FSCMD_TIMEOUT){
            //timeout occured or low power command was sent, shutdown card if necessary
            if(currState == SD_READY){
                goLowPower(handle);
                        //TERM_printDebug(TERM_handle, "powering down card\r\n");
                currState = SD_LOW_POWER; 
            }else if(currState == SD_ERROR){
                currState = SD_LOW_POWER; 
                TERM_printDebug(TERM_handle, "sd error time out\r\n");
            }
        }else{
            TERM_printDebug(TERM_handle, "invalid command received! &d\r\n", currCMD);
        }
        
        xSemaphoreGive(sdCMD);
        
        //re-enable cn in case it was disabled
        IEC1SET = _IEC1_CNBIE_MASK;
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