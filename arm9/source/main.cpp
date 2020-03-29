#include <nds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fat.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_SIZE	(1*1024*1024)

#define CONSOLE_SCREEN_WIDTH 32
#define CONSOLE_SCREEN_HEIGHT 24

extern "C" {
	bool nand_ReadSectors(sec_t sector, sec_t numSectors,void* buffer);
	bool nand_WriteSectors(sec_t sector, sec_t numSectors,const void* buffer); //!!!
}

//Declaration for building with libnds < 1.6.0
#if _LIBNDS_MINOR_ < 6
static inline bool isDSiMode() {
	extern bool __dsimode;
	return __dsimode;
}
#endif

int menuTop = 5, statusTop = 18;

//---------------------------------------------------------------------------------
int saveToFile(const char *filename, u8 *buffer, size_t size) {
//---------------------------------------------------------------------------------
	FILE *f = fopen(filename, "wb");
	if (NULL==f) return -1;
	size_t written = fwrite(buffer, 1, size, f);
	fclose(f);
	if (written != size) return -2;
	return 0;
}

//---------------------------------------------------------------------------------
int readJEDEC() {
//---------------------------------------------------------------------------------

	fifoSendValue32(FIFO_USER_01, 1);

	fifoWaitValue32(FIFO_USER_01);

	return  fifoGetValue32(FIFO_USER_01);
}

struct menuItem {
	const char* name;
	fp function;
};

u8 *firmware_buffer;
size_t userSettingsOffset, fwSize, wifiOffset, wifiSize;

//---------------------------------------------------------------------------------
void clearStatus() {
//---------------------------------------------------------------------------------
	iprintf("\x1b[%d;0H\x1b[J\x1b[15;0H",statusTop); 
	iprintf("                                ");    //clean up after previous residents
	iprintf("                                ");
	iprintf("                                ");
	iprintf("                                ");
	iprintf("\x1b[%d;0H\x1b[J\x1b[15;0H",statusTop);
}

//---------------------------------------------------------------------------------
void dummy() {
//---------------------------------------------------------------------------------
	clearStatus();
	iprintf("\x1b[%d;6HNOT IMPLEMENTED!",statusTop+3);
}

char dirname[15] = "FW";
char serial[13];

u32 sysid=0;
u32 ninfo=0;
u32 sizMB=0;
char nand_type[20]={0};
char nand_dump[80]={0};
char nand_rest[80]={0};

void chk() {
	
	nand_ReadSectors(0 , 1 , firmware_buffer);
	memcpy(&sysid, firmware_buffer + 0x100, 4);
	memcpy(&ninfo, firmware_buffer + 0x104, 4);
	
	if     (ninfo==0x00200000){sizMB=943; strcpy(nand_type,"nand_o3ds.bin");} //old3ds
	else if(ninfo==0x00280000){sizMB=1240;strcpy(nand_type,"nand_n3ds.bin");} //new3ds
	else if(sysid!=0x4453434E){sizMB=240; strcpy(nand_type,"nand_dsi.bin");}  //dsi
	else                      {sizMB=0;   strcpy(nand_type,"");}              //not recognized, do nothing
	sprintf(nand_dump,"Dump    %s",nand_type);
	sprintf(nand_rest,"Restore %s",nand_type);
	
}

//---------------------------------------------------------------------------------
void restoreNAND() {
//---------------------------------------------------------------------------------

	clearStatus();

	if (!isDSiMode()) {
		iprintf("Not a DSi or 3ds!\n");
		return;
	}

	FILE *f = fopen(nand_type, "rb");

	if (f == NULL) {
		iprintf("Failure opening %s\n", nand_type);
		return;
	}

	//Sanity checks

	//	Size check
	fseek(f, 0, SEEK_END);
	size_t dump_size = ftell(f);
	if ( dump_size != (sizMB * 1024 * 1024) ) {
		iprintf("%s and NAND sizes\ndo not match.\nOperation aborted.", nand_type);
		fclose(f);
		return;
	}
	//	NO$GBA footer check in image of normal size
	//	yes, that works in emulator
	fseek(f, -64, SEEK_CUR);
	char ngsign[] = "DSi eMMC CID/CPU\0";
	char fsign[17];
	fsign[16] = '\0';
	fread(fsign, 1, 16, f);
	if ( !strcmp(ngsign, fsign) ) {
		iprintf("%s has NO$GBA footer\nthat replaces end of image!\nOperation aborted.", nand_type);
		fclose(f);
		return;
	}	
	rewind(f);
	
	//	MBR(decrypted image) check
	//	Taken from TWLtool (https://github.com/WinterMute/twltool)
	struct {
		u8 code[446];
		struct {
			u8  status;
			u8  start_chs[3];
			u8  partition_type;
			u8  end_chs[3];
			u32 start_sector;
			u32 num_sectors;
		} __attribute__((__packed__)) partition[4];
		u8 signature[2];
	} mbr;
    fread(&mbr, 1, 0x200, f);
    if(mbr.signature[0] == 0x55 || mbr.signature[1] == 0xAA) {
		iprintf("Found MBR in %s.\nImage is not encrypted.\nOperation aborted.", nand_type);
		fclose(f);
		return;
	}
	rewind(f);
	
	//	Battery level/charger check
	u32 pwrReg = getBatteryLevel();
	bool isCharging = (pwrReg >> 7) & 1;
	int batLevel = pwrReg & 0b1111;
	
	if ( !isCharging && (batLevel < 7) ) {
		iprintf("Battery level below 50%%(2 bars)\nPlease connect charger.\nOperation aborted.");
		fclose(f);
		return;
	}
	//Sanity checks end
	
	iprintf("Sure? NAND restore is DANGEROUS!");
	iprintf("START + SELECT confirm\n");
	iprintf("B to cancel\n");
	
	while (1) {
		scanKeys();
		int keys = keysHeld();
		if ((keys & KEY_START) && (keys & KEY_SELECT))
			break;
		if (keys & KEY_B) {
			clearStatus();
			fclose(f);
			return;
		}
		swiWaitForVBlank();
	}
	
	clearStatus();
	
	if (isCharging) {
		iprintf("DON'T poweroff console\nor disconnect charger!\n");
	} else {
		iprintf("DON'T poweroff console!\n");
	}
	
	iprintf("Reading:\n%s/%s\n", dirname, nand_type);
	size_t i;
	size_t sectors = 128;
	size_t blocks = (sizMB * 1024 * 1024) / (sectors * 512);
	for (i=0; i < blocks; i++) {
		
		size_t read = fread(firmware_buffer, 1, 512 * sectors, f);
		
		if(read != 512 * sectors) {
			iprintf("\nError reading SD!\n");
			break;
		}
		
		if(!nand_WriteSectors(i * sectors,sectors,firmware_buffer)) {
			iprintf("\nError writing NAND!\n");
			break;
		}
		
		iprintf("Progress: %d/%d blocks\r", i+1, blocks);
	}
	fclose(f);
	
	clearStatus();
	iprintf("Restore %s success.\nYou may now Exit and restart\nyour console.",nand_type);
	
}

bool quitting = false;

//---------------------------------------------------------------------------------
void quit() {
//---------------------------------------------------------------------------------
	quitting = true;
	powerOn(PM_BACKLIGHT_TOP);
}

struct menuItem mainMenu[] = {
	{ "Exit", quit },
	{ nand_rest , restoreNAND}
};

//---------------------------------------------------------------------------------
void showMenu(menuItem menu[], int count) {
//---------------------------------------------------------------------------------
	int i;
	for (i=0; i<count; i++ ) {
		iprintf("\x1b[%d;5H%s", i + menuTop, menu[i].name);
	}
}


//---------------------------------------------------------------------------------
int main() {
//---------------------------------------------------------------------------------
	defaultExceptionHandler();
	
	/*
	// This doesn't do much right now. Simply ensures top screen doesn't remain white. :P
	videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE);
	vramSetBankA (VRAM_A_MAIN_BG_0x06000000);
	REG_BG0CNT = BG_MAP_BASE(0) | BG_COLOR_256 | BG_TILE_BASE(2);
	BG_PALETTE[0]=0;
	BG_PALETTE[255]=0xffff;
	u16* bgMapTop = (u16*)SCREEN_BASE_BLOCK(0);
	for (int i = 0; i < CONSOLE_SCREEN_WIDTH*CONSOLE_SCREEN_HEIGHT; i++) {
		bgMapTop[i] = (u16)i;
	}
	*/
	// Turn off top screen backlight. fwtool doesn't use topscreen for anything right now.
	powerOff(PM_BACKLIGHT_TOP);
	
	consoleDemoInit();

	if (!fatInitDefault()) {
		printf("FAT init failed!\n");
		return 0;
	}

	iprintf("DS(i) firmware tool %s\n",VERSION);

	firmware_buffer = (u8 *)memalign(32,MAX_SIZE);

	readFirmware(0, firmware_buffer, 512);

	iprintf("\x1b[2;0HMAC ");
	for (int i=0; i<6; i++) {
		printf("%02X", firmware_buffer[0x36+i]);
		sprintf(&dirname[2+(2*i)],"%02X",firmware_buffer[0x36+i]);
		if (i < 5) printf(":");
	}


	dirname[14] = 0;

	mkdir(dirname, 0777);
	chdir(dirname);

	userSettingsOffset = (firmware_buffer[32] + (firmware_buffer[33] << 8)) *8;

	fwSize = userSettingsOffset + 512;

	iprintf("\n%dK flash, jedec %X", fwSize/1024,readJEDEC());

	wifiOffset = userSettingsOffset - 1024;
	wifiSize = 1024;

	if ( firmware_buffer[29] == 0x57 ) {
		wifiOffset -= 1536;
		wifiSize += 1536;
	}

	int count = sizeof(mainMenu) / sizeof(menuItem);

	chk();

	showMenu(mainMenu, count);


	int selected = 0;
	quitting = false;

	while(!quitting) {
			iprintf("\x1b[%d;3H]\x1b[23C[",selected + menuTop);
			swiWaitForVBlank();
			scanKeys();
			int keys = keysDownRepeat();
			iprintf("\x1b[%d;3H \x1b[23C ",selected + menuTop);
			if ( (keys & KEY_UP)) selected--;
			if (selected < 0)	selected = count - 1;
			if ( (keys & KEY_DOWN)) selected++;
			if (selected == count)	selected = 0;
			if ( keys & KEY_A ) mainMenu[selected].function();
	}

	return 0;
}
