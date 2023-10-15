/*------------------------------------------------------------------------/
/  MMCv3/SDv1/SDv2 (in SPI mode) control module
/-------------------------------------------------------------------------/
/
/  Copyright (C) 2010, ChaN, all right reserved.
/
/ * This software is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
/
/-------------------------------------------------------------------------*/
//Edited by A. Morrison to function on PIC32.

// Ported by Riccardo Leonardi to PIC32MX795F512L  (22/11/2011)
// Many thanks to Aiden Morrison's good work!
// changes: parametrization of SPI port number

// Modified by Bryn Thomas (11/09/2016) to use Enhanced Buffer SPI mode
// and boost read performance with 32-bit transfers

// Modified by Aidan Mocke (04/09/2018) to work with the PIC32MZ series

#define _DISABLE_OPENADC10_CONFIGPORT_WARNING
#define _SUPPRESS_PLIB_WARNING

#include <xc.h>
#include "diskio.h"
#include "SPI.h"
#include "FreeRTOS.h"
#include "System.h"
#include "ff.h"
#include "diskioConfig.h"
#include "FS.h"

/* Definitions for MMC/SDC command */
#define CMD0   (0)			/* GO_IDLE_STATE */
#define CMD1   (1)			/* SEND_OP_COND */
#define ACMD41 (41|0x80)	/* SEND_OP_COND (SDC) */
#define CMD8   (8)			/* SEND_IF_COND */
#define CMD9   (9)			/* SEND_CSD */
#define CMD10  (10)			/* SEND_CID */
#define CMD12  (12)			/* STOP_TRANSMISSION */
#define ACMD13 (13|0x80)	/* SD_STATUS (SDC) */
#define CMD16  (16)			/* SET_BLOCKLEN */
#define CMD17  (17)			/* READ_SINGLE_BLOCK */
#define CMD18  (18)			/* READ_MULTIPLE_BLOCK */
#define CMD23  (23)			/* SET_BLOCK_COUNT */
#define ACMD23 (23|0x80)	/* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24  (24)			/* WRITE_BLOCK */
#define CMD25  (25)			/* WRITE_MULTIPLE_BLOCK */
#define CMD41  (41)			/* SEND_OP_COND (ACMD) */
#define CMD55  (55)			/* APP_CMD */
#define CMD58  (58)			/* READ_OCR */


static volatile DSTATUS Stat = STA_NOINIT;	/* Disk status */

static volatile
UINT Timer1, Timer2;		/* 1000Hz decrement timer */

static UINT CardType;

#define xmit_spi(dat) 	SPI_send(SD_spiHandle, dat)
#define rcvr_spi()		SPI_send(SD_spiHandle, 0xff)
#define rcvr_spi_m(p)	*(p) = SPI_send(SD_spiHandle, 0xff);

#define INIT_TIMEOUT pdMS_TO_TICKS(100)

SPI_HANDLE * SD_spiHandle;

/*-----------------------------------------------------------------------*/
/* Wait for card ready                                                   */
/*-----------------------------------------------------------------------*/

static BYTE wait_ready (void){
    if(!FS_clearPowerTimeout()) return 0xff;
    if(CardType == 0) return 0xff;
	BYTE res;
    
    TickType_t start = xTaskGetTickCount();
    
	rcvr_spi();
	do
		res = rcvr_spi();
	while ((res != 0xFF) && ((xTaskGetTickCount() - start) < INIT_TIMEOUT));
    
	return res;
}



/*-----------------------------------------------------------------------*/
/* Deselect the card and release SPI bus                                 */
/*-----------------------------------------------------------------------*/

static
void deselect (void)
{
	CS_HIGH();
	rcvr_spi();
}



/*-----------------------------------------------------------------------*/
/* Select the card and wait ready                                        */
/*-----------------------------------------------------------------------*/

static
int select (void)	/* 1:Successful, 0:Timeout */
{
	CS_LOW();
	if (wait_ready() != 0xFF) {
		deselect();
		return 0;
	}
	return 1;
}




/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/
/*-----------------------------------------------------------------------*/
/* Send a command packet to MMC                                          */
/*-----------------------------------------------------------------------*/

static BYTE send_cmd (BYTE cmd, DWORD arg){
	BYTE n, res, org;
    org = cmd;
	if (cmd & 0x80) {	/* ACMD<n> is the command sequense of CMD55-CMD<n> */
		cmd &= 0x7F;
		res = send_cmd(CMD55, 0);
		if (res > 1) return res;
	}

	/* Select the card and wait for ready */
	deselect();
	if (!select()) return 0xFF;

	/* Send command packet */
	xmit_spi(0x40 | cmd);			/* Start + Command index */
	xmit_spi((BYTE)(arg >> 24));	/* Argument[31..24] */
	xmit_spi((BYTE)(arg >> 16));	/* Argument[23..16] */
	xmit_spi((BYTE)(arg >> 8));		/* Argument[15..8] */
	xmit_spi((BYTE)arg);			/* Argument[7..0] */
	n = 0x01;						/* Dummy CRC + Stop */
	if (cmd == CMD0) n = 0x95;		/* Valid CRC for CMD0(0) */
	if (cmd == CMD8) n = 0x87;		/* Valid CRC for CMD8(0x1AA) */
	xmit_spi(n);

	/* Receive command response */
	if (cmd == CMD12) rcvr_spi();	/* Skip a stuff byte when stop reading */
	n = 10;							/* Wait for a valid response in timeout of 10 attempts */
	do
		res = rcvr_spi();
	while ((res & 0x80) && --n);

	return res;			/* Return with the response value */
}

/*-----------------------------------------------------------------------*/
/* Send a data packet to MMC                                             */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0
static int xmit_datablock (const BYTE *buff, BYTE token){
	BYTE resp;
	UINT bc = 512;

	if (wait_ready() != 0xFF) return 0;

	xmit_spi(token);		/* Xmit a token */
	if (token != 0xFD) {	/* Not StopTran token */
		do {						/* Xmit the 512 byte data block to the MMC */
			xmit_spi(*buff++);
			xmit_spi(*buff++);
		} while (bc -= 2);
		xmit_spi(0xFF);				/* CRC (Dummy) */
		xmit_spi(0xFF);
		resp = rcvr_spi();			/* Receive a data response */
		if ((resp & 0x1F) != 0x05)	/* If not accepted, return with error */
			return 0;
	}

	return 1;
}
#endif	/* _READONLY */


#if _READONLY == 0
DRESULT disk_write (BYTE pdrv, const BYTE *buff, DWORD sector, UINT count){
	if (pdrv || !count) return RES_PARERR;
	if (Stat & STA_NOINIT) return RES_NOTRDY;
	if (Stat & STA_PROTECT) return RES_WRPRT;

	if (!(CardType & CT_BLOCK)) sector *= 512;	/* Convert to byte address if needed */

	if (count == 1) {		/* Single block write */
		if ((send_cmd(CMD24, sector) == 0)	/* WRITE_BLOCK */
			&& xmit_datablock(buff, 0xFE))
			count = 0;
	}else {				/* Multiple block write */
		if (CardType & CT_SDC) send_cmd(ACMD23, count);
		if (send_cmd(CMD25, sector) == 0) {	/* WRITE_MULTIPLE_BLOCK */
			do {
				if (!xmit_datablock(buff, 0xFC)) break;
				buff += 512;
			} while (--count);
			if (!xmit_datablock(0, 0xFD))	/* STOP_TRAN token */
				count = 1;
		}
	}
	deselect();

	return count ? RES_ERROR : RES_OK;
}
#endif /* _READONLY */

static void power_on(void){
	Stat |= STA_NOINIT;	/* Set STA_NOINIT */
}

static void power_off(void){
	Stat |= STA_NOINIT;	/* Set STA_NOINIT */
}

/*-----------------------------------------------------------------------*/
/* Receive a data packet from MMC rather quickly                         */
/*-----------------------------------------------------------------------*/

#define FRS_WAIT_TOKEN  0
#define FRS_WAIT_READ   1
#define FRS_WAIT_SKIP   2
#define FRS_RETURN_ERROR   0xff
#define FRS_RETURN_OK   0xfe

typedef struct{
    uint32_t state;
    uint32_t bytesLeft;
    uint32_t currStartByte;
    uint8_t * garbageBin;
    uint8_t * buffer;
    SPI_HANDLE * spiHandle;
    SemaphoreHandle_t semaphore;
} rcvr_ISRDATA;

static void rcvr_fastReadDMAISR(uint32_t evt, void * data){
    rcvr_ISRDATA * d = (rcvr_ISRDATA *) data;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    if(evt & _DCH0INT_CHERIF_MASK){
        //error!
        d->state = FRS_RETURN_ERROR;
        xSemaphoreGiveFromISR(d->semaphore, &xHigherPriorityTaskWoken);
    }
    
    //TODO disable enhanced buffer for pattern match
    switch(d->state){
            
        case FRS_WAIT_READ:     //we just got the data -> check if we need any more
            rcvr_spi();
            rcvr_spi(); //skip crc TODO calculate crc with dma
            d->bytesLeft -= (512 - d->currStartByte);
            d->buffer += (512 - d->currStartByte);
            d->currStartByte = 0;

            if(d->bytesLeft == 0){
                d->state = FRS_RETURN_OK;
                xSemaphoreGiveFromISR(d->semaphore, &xHigherPriorityTaskWoken);
            }else{
                //start next read
                uint32_t count = 512; uint8_t token = 0xff;
                while(((token = rcvr_spi()) == 0xFF) && --count);
                if(token == 0xfe){
                    if(d->bytesLeft >= 512){
                        SPI_continueDMARead(d->spiHandle, d->buffer, 512, 1, 1);
                    }else{
                        d->state = FRS_WAIT_SKIP;
                        SPI_continueDMARead(d->spiHandle, d->buffer, d->bytesLeft, 1, 1);
                    }
                }else{ //error?
                    d->state = FRS_RETURN_ERROR;
                    xSemaphoreGiveFromISR(d->semaphore, &xHigherPriorityTaskWoken);
                }
            }
            return;
            
        case FRS_WAIT_SKIP:     //we just got the data to skip -> continue with normal data
            if(d->currStartByte != 0){    //we are skipping from the beginning -> just continue with normal read
                SPI_continueDMARead(d->spiHandle, d->buffer, 512 - d->currStartByte, 1, 1);
                d->state = FRS_WAIT_READ;
            }else{ //we received tailing skip data -> read crc and go back to task
                SPI_continueDMARead(d->spiHandle, d->buffer, 512 - d->bytesLeft, 1, 1);
                d->currStartByte = 512 - d->bytesLeft;
                d->state = FRS_WAIT_READ;
            }
            return;
    }
}

static int rcvr_datablockFast (BYTE *buff, UINT startOffset, UINT btr){
    rcvr_ISRDATA * isrData = pvPortMalloc(sizeof(rcvr_ISRDATA));
    isrData->buffer = buff;
    isrData->bytesLeft = btr;
    isrData->spiHandle = SD_spiHandle;
    isrData->semaphore = SD_spiHandle->semaphore;
    isrData->currStartByte = startOffset;
    
    uint32_t garbageDataSize = startOffset;
    if(garbageDataSize < (512 - startOffset)) garbageDataSize = (512 - startOffset);
    isrData->garbageBin = pvPortMalloc(garbageDataSize);
    
    SPI_setDMAEnabled(SD_spiHandle, 1);
    
	BYTE token;
    
	Timer1 = 100;
	do {							/* Wait for data packet in timeout of 100ms */
		token = rcvr_spi();
	} while ((token == 0xFF) && Timer1);

	if(token != 0xFE){ 
        xSemaphoreGive(SD_spiHandle->semaphore);
        return 0;		/* If not valid data token, return with error */
    }
    
    //token received correctly -> card is ready to give us the d(ata) kekW
    if(startOffset == 0){   //any offset?
        //no -> start normal read
        isrData->state = FRS_WAIT_READ;
        SPI_sendBytes(SD_spiHandle, buff, 512, 1, 1, rcvr_fastReadDMAISR, isrData);
        
    }else{
        //yes -> start offset read
        isrData->state = FRS_WAIT_SKIP;
        SPI_sendBytes(SD_spiHandle, isrData->garbageBin, startOffset, 1, 1, rcvr_fastReadDMAISR, isrData);
    }
    
    uint32_t ret = btr;
    if(!xSemaphoreTake(SD_spiHandle->semaphore, 1000)) ret = 0;
    
    if(isrData->state != FRS_RETURN_OK) ret = btr - isrData->bytesLeft;
    
    vPortFree(isrData->garbageBin);
    vPortFree(isrData);
    SPI_setDMAEnabled(SD_spiHandle, 0);

	return ret;
}

/*-----------------------------------------------------------------------*/
/* Receive a data packet from MMC                                        */
/*-----------------------------------------------------------------------*/
static int rcvr_datablock (BYTE *buff, UINT btr){
	BYTE token;
    
	Timer1 = 100;
	do {							/* Wait for data packet in timeout of 100ms */
		token = rcvr_spi();
	} while ((token == 0xFF) && Timer1);

	if(token != 0xFE) return 0;		/* If not valid data token, retutn with error */
    
    //SPI_setDMAEnabled(SD_spiHandle, 1);
    SPI_sendBytes(SD_spiHandle, buff, btr, 1, 1, NULL, NULL);
    //SPI_setDMAEnabled(SD_spiHandle, 0);
    
	rcvr_spi();						/* Discard CRC */
	rcvr_spi();

	return 1;						/* Return with success */
}

void disk_setSPIHandle(SPI_HANDLE * handle){
    SD_spiHandle = handle;
}

DSTATUS disk_initialize (BYTE drv){
	BYTE n, cmd, ty, ocr[4];
    
    FS_clearPowerTimeout();
    
    CardType = 0;
	power_on();							/* Force socket power on */
    FCLK_SLOW();
	for (n = 80; n; n--) rcvr_spi();	/* 80 dummy clocks */
    
	ty = 0;
	if (send_cmd(CMD0, 0) == 1) {			/* Enter Idle state */
		Timer1 = 100;						/* Initialization timeout of 1000 msec */
		if (send_cmd(CMD8, 0x1AA) == 1) {	/* SDv2? */
			for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();			/* Get trailing return value of R7 resp */
			if (ocr[2] == 0x01 && ocr[3] == 0xAA) {				/* The card can work at vdd range of 2.7-3.6V */
				while (Timer1 && send_cmd(ACMD41, 0x40000000));	/* Wait for leaving idle state (ACMD41 with HCS bit) */
				if (Timer1 && send_cmd(CMD58, 0) == 0) {			/* Check CCS bit in the OCR */
					for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();
					ty = (ocr[0] & 0x40) ? CT_SD2|CT_BLOCK : CT_SD2;	/* SDv2 */
				}
			}
		} else {							/* SDv1 or MMCv3 */
			if (send_cmd(ACMD41, 0) <= 1) 	{
				ty = CT_SD1; cmd = ACMD41;	/* SDv1 */
			} else {
				ty = CT_MMC; cmd = CMD1;	/* MMCv3 */
			}
			while (Timer1 && send_cmd(cmd, 0));		/* Wait for leaving idle state */
			if (!Timer1 || send_cmd(CMD16, 512) != 0)	/* Set read/write block length to 512 */
				ty = 0;
		}
	}
	CardType = ty;
	deselect();

	if (ty) {			/* Initialization succeded */
		Stat &= ~STA_NOINIT;	/* Clear STA_NOINIT */
		FCLK_FAST();
	} else {			/* Initialization failed */
		power_off();
	}

	return Stat;
}



/*-----------------------------------------------------------------------*/
/* Get Disk Status                                                       */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status (BYTE drv){
	if (drv) return STA_NOINIT;		/* Supports only single drive */
	return Stat;
}


DRESULT disk_readList (BYTE pdrv, BYTE* buff, DLLObject * list){
    if(!xSemaphoreTake(SD_spiHandle->semaphore, 1000)) return RES_ERROR;
    
    if (pdrv) return RES_PARERR;
	if (Stat & STA_NOINIT) return RES_NOTRDY;

    ff_readListData_t * currObj = NULL;
    FRESULT result = FR_OK;
    
    while((currObj = DLL_pop(list))){
        //get address to start reading at
        uint32_t startSectorAdress = currObj->startSector;
        if (!(CardType & CT_BLOCK)) startSectorAdress *= 512;	/* Convert to byte address if needed */
        
        uint32_t sectorsToRead = currObj->bytesToRead / 512; //TODO dynamic sector sizes!
        
        if(sectorsToRead <= 1){
            if ((send_cmd(CMD17, startSectorAdress) == 0)){
                if(!rcvr_datablockFast(buff, currObj->startByte, currObj->bytesToRead)){ 
                    result = FR_DISK_ERR;
                    break;
                }
            }else{
                result = FR_DISK_ERR;
                break;
            }
        }else{
            if (send_cmd(CMD18, startSectorAdress) == 0) {	/* READ_MULTIPLE_BLOCK */
                uint32_t success = rcvr_datablockFast(buff, currObj->startByte, currObj->bytesToRead);
                send_cmd(CMD12, 0);				/* STOP_TRANSMISSION */
                if (!success){ 
                    result = FR_DISK_ERR;
                    break;
                }
            }
        }
        vPortFree(currObj);
        currObj = NULL;
    }
    
    //free incase we broke out of the loop, won't do anything if NULL 
    vPortFree(currObj);
    
	deselect();
    
    uint32_t count = DLL_length(list);
    
    //empty list if anything remains
    while((currObj = DLL_pop(list))){
        vPortFree(currObj);
        currObj = NULL;
    }
    
    DLL_free(list);
    
    xSemaphoreGive(SD_spiHandle->semaphore);

	return count ? RES_ERROR : result;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */
/*-----------------------------------------------------------------------*/

DRESULT disk_read (BYTE pdrv, BYTE* buff, DWORD sector, UINT count)
{
	if (pdrv || !count) return RES_PARERR;
	if (Stat & STA_NOINIT) return RES_NOTRDY;

	if (!(CardType & CT_BLOCK)) sector *= 512;	/* Convert to byte address if needed */

	if (count == 1) {		/* Single block read */
		if ((send_cmd(CMD17, sector) == 0)	/* READ_SINGLE_BLOCK */
			&& rcvr_datablock(buff, 512))
			count = 0;
	}
	else {				/* Multiple block read */
		if (send_cmd(CMD18, sector) == 0) {	/* READ_MULTIPLE_BLOCK */
			do {
				if (!rcvr_datablock(buff, 512)) break;
				buff += 512;
			} while (--count);
			send_cmd(CMD12, 0);				/* STOP_TRANSMISSION */
		}
	}
	deselect();

	return count ? RES_ERROR : RES_OK;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0
DRESULT w (BYTE drv, const BYTE *buff, DWORD sector, BYTE count){
	if (drv || !count) return RES_PARERR;
	if (Stat & STA_NOINIT) return RES_NOTRDY;
	if (Stat & STA_PROTECT) return RES_WRPRT;

	if (!(CardType & CT_BLOCK)) sector *= 512;	/* Convert to byte address if needed */

	if (count == 1) {		/* Single block write */
		if ((send_cmd(CMD24, sector) == 0)	/* WRITE_BLOCK */
			&& xmit_datablock(buff, 0xFE))
			count = 0;
	}
	else {				/* Multiple block write */
		if (CardType & CT_SDC) send_cmd(ACMD23, count);
		if (send_cmd(CMD25, sector) == 0) {	/* WRITE_MULTIPLE_BLOCK */
			do {
				if (!xmit_datablock(buff, 0xFC)) break;
				buff += 512;
			} while (--count);
			if (!xmit_datablock(0, 0xFD))	/* STOP_TRAN token */
				count = 1;
		}
	}
	deselect();

	return count ? RES_ERROR : RES_OK;
}
#endif /* _READONLY */



/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl (BYTE drv, BYTE ctrl, void *buff){
	DRESULT res;
	BYTE n, csd[16], *ptr = buff;
	DWORD csize;


	if (drv) return RES_PARERR;
	if (Stat & STA_NOINIT) return RES_NOTRDY;

	res = RES_ERROR;
	switch (ctrl) {
		case CTRL_SYNC :	/* Flush dirty buffer if present */
			if (select()) {
				deselect();
				res = RES_OK;
			}
			break;

		case GET_SECTOR_COUNT :	/* Get number of sectors on the disk (WORD) */
			if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {
				if ((csd[0] >> 6) == 1) {	/* SDv2? */
					csize = csd[9] + ((WORD)csd[8] << 8) + 1;
					*(DWORD*)buff = (DWORD)csize << 10;
				} else {					/* SDv1 or MMCv2 */
					n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
					csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
					*(DWORD*)buff = (DWORD)csize << (n - 9);
				}
				res = RES_OK;
			}
			break;

		case GET_SECTOR_SIZE :	/* Get sectors on the disk (WORD) */
			*(WORD*)buff = 512;
			res = RES_OK;
			break;

		case GET_BLOCK_SIZE :	/* Get erase block size in unit of sectors (DWORD) */
			if (CardType & CT_SD2) {	/* SDv2? */
				if (send_cmd(ACMD13, 0) == 0) {		/* Read SD status */
					rcvr_spi();
					if (rcvr_datablock(csd, 16)) {				/* Read partial block */
						for (n = 64 - 16; n; n--) rcvr_spi();	/* Purge trailing data */
						*(DWORD*)buff = 16UL << (csd[10] >> 4);
						res = RES_OK;
					}
				}
			} else {					/* SDv1 or MMCv3 */
				if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16)) {	/* Read CSD */
					if (CardType & CT_SD1) {	/* SDv1 */
						*(DWORD*)buff = (((csd[10] & 63) << 1) + ((WORD)(csd[11] & 128) >> 7) + 1) << ((csd[13] >> 6) - 1);
					} else {					/* MMCv3 */
						*(DWORD*)buff = ((WORD)((csd[10] & 124) >> 2) + 1) * (((csd[11] & 3) << 3) + ((csd[11] & 224) >> 5) + 1);
					}
					res = RES_OK;
				}
			}
			break;

		case MMC_GET_TYPE :		/* Get card type flags (1 byte) */
			*ptr = CardType;
			res = RES_OK;
			break;

		case MMC_GET_CSD :	/* Receive CSD as a data block (16 bytes) */
			if ((send_cmd(CMD9, 0) == 0)	/* READ_CSD */
				&& rcvr_datablock(buff, 16))
				res = RES_OK;
			break;

		case MMC_GET_CID :	/* Receive CID as a data block (16 bytes) */
			if ((send_cmd(CMD10, 0) == 0)	/* READ_CID */
				&& rcvr_datablock(buff, 16))
				res = RES_OK;
			break;

		case MMC_GET_OCR :	/* Receive OCR as an R3 resp (4 bytes) */
			if (send_cmd(CMD58, 0) == 0) {	/* READ_OCR */
				for (n = 0; n < 4; n++)
					*((BYTE*)buff+n) = rcvr_spi();
				res = RES_OK;
			}
			break;

		case MMC_GET_SDSTAT :	/* Receive SD statsu as a data block (64 bytes) */
			if (send_cmd(ACMD13, 0) == 0) {	/* SD_STATUS */
				rcvr_spi();
				if (rcvr_datablock(buff, 64))
					res = RES_OK;
			}
			break;

		default:
			res = RES_PARERR;
	}

	deselect();

	return res;
}