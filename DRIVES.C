
/*
Watcom C - Simple hard drives detection and read for 32-bit protected mode using INT 13h API under a DOS extender
Written by Piotr Ulaszewski (pulaweb) March 2020
This is just a demonstartion on how to obtain hard drive info on any ATA legacy system
*/

#include <stdlib.h>
#include <stdio.h>
#include <conio.h>
#include <string.h>
#include <i86.h>
#include <math.h>
#include "drives.h"
#include "ata.h"


// frequency of the high resolution timer
unsigned __int64 calculated_frequency = 0;

// ATA status and error registers
BYTE status_register = 0;
BYTE device_register = 0;
BYTE chigh_register = 0;
BYTE clow_register = 0;
BYTE sector_register = 0;
BYTE count_register = 0;
BYTE error_register = 0;

BYTE lbahigh07_register = 0;
BYTE lbahigh815_register = 0;
BYTE lbamid07_register = 0;
BYTE lbamid815_register = 0;
BYTE lbalow07_register = 0;
BYTE lbalow815_register = 0;
BYTE count07_register = 0;
BYTE count815_register = 0;

// disk address packet pointer for BIOS reads
WORD DiskAddressPacket_RealSegment;
WORD DiskAddressPacket_MemSelector;
DISKADDRESSPACKET *DiskAddressPacket = NULL;

// memory to hold data read/write from/to disk
WORD DiskMultiSectorTransferBufferDOS_RealSegment;
WORD DiskMultiSectorTransferBufferDOS_MemSelector;
BYTE *DiskMultiSectorTransferBufferDOS = NULL;
BYTE *DiskMultiSectorTransferBuffer = NULL;

// to display and update current read starting sector
unsigned __int64 CurrentSector;

// array of 64 BIOS drives
DISKDRIVE diskdrive[64];

// temporary string buffer
char tmpstring[1024];


int main (void) {
    int bios_ext_major = 0;               // simply info
    int bios_ext_minor = 0;
    WORD bios_ext_api = 0;                // api support

    WORD DriveParam_RealSegment = 0;      // for hard disk drive parameters
    WORD DriveParam_MemSelector = 0;      // for hard disk drive parameters
    DRIVEPARAM *DriveParam = NULL;        // for hard disk drive parameters
    CONFIGPARAM *ConfigParam = NULL;
    DPMIREGS dpmiregs;
    int i, j;
    BOOL status = FALSE;
    DRIVEINFO driveinfo;
    unsigned __int64 calculated_frequency_start = 0;
    unsigned __int64 calculated_frequency_stop = 0;


    // (1) CHECK IF SYSTEM OK AND CALC FREQUENCY TIMER
    // calculate frequency in cycles for 1ns of pentium timer
    QueryPerformanceCounter(&calculated_frequency_start);
    delay(1000);
    QueryPerformanceCounter(&calculated_frequency_stop);
    calculated_frequency = (calculated_frequency_stop - calculated_frequency_start);

    // check int 13h extensions supported
    memset(&dpmiregs, 0, sizeof(DPMIREGS));
    dpmiregs.eax = 0x4100;
    dpmiregs.ebx = 0x55aa;
    dpmiregs.edx = 0x80;
    DPMI_SimulateRMI(0x13, &dpmiregs);
    if (dpmiregs.flags & 1) {
        printf("Sorry INT13H extensions not supported or no hard drive!\n");
        exit(0);
    }
    if (LOWORD(dpmiregs.ebx) != 0xaa55) {
        printf("Sorry INT13H extensions not active or no hard drive!\n");
        exit(0);
    }

    // INT 13h extensions supported
    bios_ext_major = HIBYTE(dpmiregs.eax) >> 4;
    bios_ext_minor = HIBYTE(dpmiregs.eax) & 0xF;
    bios_ext_api = dpmiregs.ecx & 0xFFFF;


    // (2) ALLOCATE MEMORY
    // allocate low DOS memory for drive param buffer (1)
    if (!DPMI_DOSmalloc(512, &DriveParam_RealSegment, &DriveParam_MemSelector)) {
        // DPMI_DOSMalloc error
        printf("Error allocating DOS memory drive param");
        exit(0);
    }
    DriveParam = (DRIVEPARAM *) (DriveParam_RealSegment << 4);

    // allocate low DOS memory for disk address packet blocks (2)
    if (!DPMI_DOSmalloc(512, &DiskAddressPacket_RealSegment, &DiskAddressPacket_MemSelector)) {
        printf("Error allocating DOS memory for disk addres packet");
        exit(0);
    }
    DiskAddressPacket = (DISKADDRESSPACKET *) (DiskAddressPacket_RealSegment << 4);
	
    // prepare multi sector transfer buffer (64 sectors at a given time - low DOS memory) (3)
    if (!DPMI_DOSmalloc(512 * 64, &DiskMultiSectorTransferBufferDOS_RealSegment, &DiskMultiSectorTransferBufferDOS_MemSelector)) {
        printf("Error allocating DOS memory for disk multi sector transfer buffer");
        exit(0);
    }
    DiskMultiSectorTransferBufferDOS = (BYTE *) (DiskMultiSectorTransferBufferDOS_RealSegment << 4);

    // allocate disk buffer (4)
    DiskMultiSectorTransferBuffer = (BYTE *) malloc(MULTI_SECTOR * 512);
    if (DiskMultiSectorTransferBuffer == NULL) {
        printf("Error allocating extended memory for disk buffer");
        exit(0);
    }

    // (3) FIND DISKS
    // search for disks - BIOS hard drives start from 0x80
    i = 0x80;
    j = 0;
    while (i < 0x100) {

        // get drive parameters - int 13h extensions call
        memset(&dpmiregs, 0, sizeof(DPMIREGS));
        memset(DriveParam, 0, sizeof(DRIVEPARAM));
        switch ((bios_ext_major << 4) + bios_ext_minor) {
            case 0x01 : DriveParam->buffer_size = 0x1A; break;
            case 0x20 :
            case 0x21 : DriveParam->buffer_size = 0x1E; break;
            case 0x30 : DriveParam->buffer_size = 0x42; break;
            default   : DriveParam->buffer_size = 0x200;
        }

        dpmiregs.eax = 0x4800;
        dpmiregs.edx = i;
        dpmiregs.esi = 0;
        dpmiregs.ds = DriveParam_RealSegment;
        DPMI_SimulateRMI(0x13, &dpmiregs);

        if (!(dpmiregs.flags & 1) && !HIBYTE(dpmiregs.eax) && DriveParam->total_sectors) {   // test if no CF set and no error code in AH

            // int 13h extensions must be verison 2 or higher and config param structure must be filled
            if ((bios_ext_major >= 2) && (bios_ext_api & 0x01) && (DriveParam->config_param != 0xFFFFFFFF)) {
                printf("\n");

                // set config param structure
                ConfigParam = (CONFIGPARAM *) ( ((DriveParam->config_param >> 16) << 4) + (WORD)DriveParam->config_param );
            
                if (DriveParam->info_flags & (1 + 4) && !DriveParam->config_param) {
                    // removable drive
                        printf("Drive 0x%x is a removable drive probably connected via USB\n", i);
                        i++;
                        continue;
                } else {
                    // hard drive
                    diskdrive[j].bios_nr = i;

                    // skip detection via ATA/SATA if buffer size is less than 30, config_param pointer is not returned in this case
                    if ((DriveParam->buffer_size < 30) || (ConfigParam == NULL)) {
                        printf("Config parameters structure not found for drive 0x%x\n", i);
                        i++;
                        continue;
                    }
                    
                    if (ConfigParam->flags & 0x10) {
                        printf ("HDD 0x%x is slave, HDD Base port is: 0x%x, HDD Control port is: 0x%x\n", i, ConfigParam->base_port, ConfigParam->control_port);
                        diskdrive[j].ata_master = FALSE;
                    } else {
                        printf ("HDD 0x%x is master, HDD Base port is: 0x%x, HDD Control port is: 0x%x\n", i, ConfigParam->base_port, ConfigParam->control_port);
                        diskdrive[j].ata_master = TRUE;
                    }
                    diskdrive[j].ata_base = ConfigParam->base_port;
                    diskdrive[j].ata_ctrl = ConfigParam->control_port;
                    diskdrive[j].bytes_per_sector = DriveParam->bytes_per_sector;
                    diskdrive[j].total_sectors = DriveParam->total_sectors;
                    diskdrive[j].total_gb = (__int64)ceil(((double)diskdrive[j].total_sectors * (double)diskdrive[j].bytes_per_sector) / (double)(1024.0 * 1024.0 * 1024.0));
                    
                    printf("Drive size in sectors : %d, ", diskdrive[j].total_sectors);
                    printf("drive size in GB : %d", diskdrive[j].total_gb);
                    printf("\n");
                    
                    // send ATA - command, features, count, direction
                    status = ata_send_command (0XEC, 0x00, 0x01, 1, &diskdrive[j], (BYTE *)&driveinfo);
                    if (!status) {
                        printf("ATA identfy cmmand error!\n");
                        i++;
                        continue;
                    }
                        
                    // get drive model
                    strcpy(tmpstring, text_CutSpacesAfter (text_ConvertToString (driveinfo.sModelNumber, 40)));
                    strcpy(diskdrive[j].drive_model, tmpstring);

                    // get drive serial
                    strcpy(tmpstring, text_CutSpacesBefore (text_ConvertToString (driveinfo.sSerialNumber, 20)));
                    strcpy(diskdrive[j].drive_serial, tmpstring);

                    // get drive firmware
                    strcpy(tmpstring, text_CutSpacesAfter (text_ConvertToString (driveinfo.sFirmwareRev, 8)));
                    strcpy(diskdrive[j].drive_firmware, tmpstring);

                    // print it on the screen
                    itoa(diskdrive[j].bios_nr, tmpstring, 16);
                    printf("Bios drive nr : 0x");
                    printf(tmpstring);
                    printf("\n");
					
                    printf("Drive model : ");
                    printf(diskdrive[j].drive_model);
                    printf("\n");
                    
                    printf("Drive serial number : ");
                    printf(diskdrive[j].drive_serial);
                    printf("\n");
                    
                    printf("Drive firmware revision : ");
                    printf(diskdrive[j].drive_firmware);
                    printf("\n");
                    
                }
            } else {
                printf ("INT 13h extensions version too low or no config parameters structure.\nUnable to detect hard drive base and control port!\n");
            }
        }
        // next drive
        i++;
        j++;
    }
	
    // no drives found?
    if (j == 0) {
        printf("Sorry, no hard drives found!");
        exit(0);
    }
	
    printf ("\n\nHit ESC to stop.\n");
		
    CurrentSector = 0;
    while (1) {
        printf("BIOS read from drive 0x80 (0), sector : %d\n", CurrentSector);
        if (!read_multisector (CurrentSector, &diskdrive[0])) break;
        CurrentSector += MULTI_SECTOR;
        if ( kbhit())
        {
            int c = getch();
            if (c == 27) break;
        }
        text_UpLine();
    }

    return 1;
}


/*************************************/
/* temporary for testing rdtsc timer */
/*************************************/

void QueryPerformanceFrequency (unsigned __int64 *freq)
{
    *freq = calculated_frequency;
}

void QueryPerformanceCounter (unsigned __int64 *count)
{
    _asm rdtsc;   // pentium or above
    _asm mov ebx, count;
    _asm mov [ebx], eax;
    _asm mov [ebx + 4], edx;
}


/******************/
/* DPMI functions */
/******************/

BOOL DPMI_SimulateRMI (BYTE IntNum, DPMIREGS *regs) {
    BYTE noerror = 0;

    _asm {
        mov edi, [regs]
        sub ecx,ecx
        sub ebx,ebx
        mov bl, [IntNum]
        mov eax, 0x300
        int 0x31
        setnc [noerror]
    }   
    return noerror;
}

BOOL DPMI_DOSmalloc (DWORD size, WORD *segment, WORD *selector) {
    BYTE noerror = 0;
    
    _asm {
        mov eax, 0x100
        mov ebx, [size]
        add ebx, 0x0f
        shr ebx, 4
        int 0x31
        setnc [noerror]
        mov ebx, [segment]
        mov [ebx], ax
        mov ebx, [selector]
        mov [ebx], dx
    }
    return noerror;
}

void DPMI_DOSfree (WORD *selector) {
    
    _asm {
        mov eax, [selector]
        mov dx, [eax]
        mov eax, 0x101
        int 0x31
    }
}


/******************/
/* text functions */
/******************/

char *text_ConvertToString (char stringdata[256], int count)
{
    int i = 0;
    static char string[512];

    // characters are stored backwards
    for (i = 0; i < count; i += 2) {
        string [i] = (char) (stringdata[i + 1]);
        string [i + 1] = (char) (stringdata[i]);
    }

    // end the string
    string[i] = '\0';

    return string;
}

char *text_CutSpacesAfter (char *str)
{
    int i = 0;
    static char cut [512];

    // cut spaces after text
    strcpy (cut, str);
    for (i = strlen(cut) - 1; i > 0 && cut[i] == ' '; i--)
        cut[i] = '\0';

    return cut;
}

char *text_CutSpacesBefore (char *str)
{
    int i = 0;
    int j = 0;
    static char cut [512];

    // cut spaces before text
    for (i = 0; i < strlen(str); i++)
        if (str[i] != ' ') {
            cut[j] = str[i];
            j++;
        }
    cut[j] = '\0';

    return cut;
}

void text_SetPosition (BYTE col, BYTE row)
{
    DPMIREGS dpmiregs;

    memset(&dpmiregs, 0, sizeof(DPMIREGS));
    dpmiregs.eax = 0x0200;
    dpmiregs.ebx = 0x00;
    dpmiregs.edx = (row << 8) + col;
    DPMI_SimulateRMI(0x10, &dpmiregs);
}

void text_UpLine(void) {
    BYTE row;
    BYTE col;
    DPMIREGS dpmiregs;

    memset(&dpmiregs, 0, sizeof(DPMIREGS));
    dpmiregs.eax = 0x0300;
    dpmiregs.ebx = 0x00;
    DPMI_SimulateRMI(0x10, &dpmiregs);
    row = (dpmiregs.edx >> 8) & 0xFF;
    col = dpmiregs.edx & 0xFF;
    if (row > 0 ) row--;
	
    dpmiregs.eax = 0x0200;
    dpmiregs.ebx = 0x00;
    dpmiregs.edx = (row << 8) + col;
    DPMI_SimulateRMI(0x10, &dpmiregs);
}

/******************/
/* IO functions */
/******************/

BOOL read_multisector (__int64 starting_sector, DISKDRIVE *sdrive) {

    int i,idx;
    DPMIREGS dpmiregs;
    int exit_flag = 0;
       
    // MULTI_SECTOR must be at least 64 and a multiple of 64
    if (MULTI_SECTOR % 64) return FALSE;

    i = MULTI_SECTOR >> 6; // i = 256/64 = 4
    for (idx = 0; idx < i; idx ++) {
        if ((starting_sector + (idx * 64)) >= sdrive->total_sectors) {
            DiskAddressPacket->sectors_to_transfer = (starting_sector + (idx * 64)) - sdrive->total_sectors;
            exit_flag = 1;
        } else {
            DiskAddressPacket->sectors_to_transfer = 64;
        }
        if (DiskAddressPacket->sectors_to_transfer == 0) break;
         
        // set packet
        DiskAddressPacket->structure_size = 0x10;
        DiskAddressPacket->reserved = 0;
        DiskAddressPacket->transfer_buffer_offset = 0;
        DiskAddressPacket->transfer_buffer_segment = DiskMultiSectorTransferBufferDOS_RealSegment;
        DiskAddressPacket->starting_sector = (__int64) (starting_sector + (idx * 64));

        // call int13h extensions - BIOS call
        memset(&dpmiregs, 0, sizeof(DPMIREGS));
        dpmiregs.eax = 0x4200;
        dpmiregs.edx = sdrive->bios_nr;
        dpmiregs.esi = 0;
        dpmiregs.ds = DiskAddressPacket_RealSegment;
        DPMI_SimulateRMI(0x13, &dpmiregs);
        if (HIBYTE(dpmiregs.eax)) {
            return FALSE;
        }

        // copy by chunks of (sdrive->bytes_per_sector * 64 sectors) bytes
        memcpy (DiskMultiSectorTransferBuffer + (idx * (sdrive->bytes_per_sector << 6)), DiskMultiSectorTransferBufferDOS, sdrive->bytes_per_sector << 6);
           
        // check for exit
        if (exit_flag) break;
    }
    return TRUE;       
}
