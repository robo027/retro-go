#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef RETRO_GO
#include <rg_system.h>
#define LOG_PRINTF(level, x...) rg_system_log(RG_LOG_USER, NULL, x)
#else
#define LOG_PRINTF(level, x...) printf(x)
#define IRAM_ATTR
#endif

#define MESSAGE_ERROR(x, ...) LOG_PRINTF(1, "!! %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_WARN(x, ...)  LOG_PRINTF(2, "** %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_INFO(x, ...)  LOG_PRINTF(3, " * %s: " x, __func__, ## __VA_ARGS__)
// #define MESSAGE_DEBUG(x, ...) LOG_PRINTF(4, ">> %s: " x, __func__, ## __VA_ARGS__)
#define MESSAGE_DEBUG(x, ...) {}

#define GB_WIDTH (160)
#define GB_HEIGHT (144)

typedef uint8_t byte;
typedef uint8_t un8;
typedef uint16_t un16;
typedef uint32_t un32;
typedef int8_t n8;
typedef int16_t n16;
typedef int32_t n32;

typedef struct
{
	struct {
		byte *buffer;
		un16 cgb_pal[64];
		un16 dmg_pal[4][4];
		int colorize;
		int format;
		int enabled;
		void (*vblank)(void);
	} lcd;

	struct {
		uint samplerate;
		bool stereo;
		size_t pos, len;
		int16_t* buffer;
	} snd;
} gb_host_t;

typedef enum
{
	GB_HW_DMG,
	GB_HW_CGB,
	GB_HW_SGB,
} gb_hwtype_t;

typedef enum
{
	GB_PAD_RIGHT = 0x01,
	GB_PAD_LEFT = 0x02,
	GB_PAD_UP = 0x04,
	GB_PAD_DOWN = 0x08,
	GB_PAD_A = 0x10,
	GB_PAD_B = 0x20,
	GB_PAD_SELECT = 0x40,
	GB_PAD_START = 0x80,
} gb_padbtn_t;

typedef enum
{
	GB_PIXEL_PALETTED,
	GB_PIXEL_565_LE,
	GB_PIXEL_565_BE,
} gb_pixformat_t;

typedef enum
{
	GB_PALETTE_DEFAULT,
	GB_PALETTE_2BGRAYS,
	GB_PALETTE_LINKSAW,
	GB_PALETTE_NSUPRGB,
	GB_PALETTE_NGBARNE,
	GB_PALETTE_GRAPEFR,
	GB_PALETTE_MEGAMAN,
	GB_PALETTE_POKEMON,
	GB_PALETTE_DMGREEN,
	GB_PALETTE_GBC,
	GB_PALETTE_SGB,
	GB_PALETTE_COUNT,
} gb_palette_t;

enum {
	MBC_NONE = 0,
	MBC_MBC1,
	MBC_MBC2,
	MBC_MBC3,
	MBC_MBC5,
	MBC_MBC6,
	MBC_MBC7,
	MBC_HUC1,
	MBC_HUC3,
	MBC_MMM01,
};

typedef struct
{
	byte rambanks[8][4096];
	byte ioregs[256];
	byte *rmap[0x10];
	byte *wmap[0x10];
	un32 interrupts;
	un32 joypad;
	un32 hwtype;
	byte *bios;
	n32 serial;
	n32 hdma;
	un32 timer, timer_div;
} gb_hw_t;

typedef struct
{
	n32 sel, flags, latch;
	n32 ticks;      // Ticks (60 = +1s)
	n32 d, h, m, s; // Current time
	n32 regs[5];    // Latched time
} gb_rtc_t;

typedef struct
{
	// Meta information
	char name[20];
	un16 checksum;
	un16 colorize;
	un32 romsize;
	un32 ramsize;

	// Memory
	byte *rombanks[512];
	byte (*rambanks)[8192];
	un32 sram_dirty;
	un32 sram_saved;

	// Extra hardware
	bool has_rumble;
	bool has_sensor;
	bool has_battery;
	bool has_rtc;

	// MBC stuff
	int mbc;
	int bankmode;
	int enableram;
	int rombank;
	int rambank;
	gb_rtc_t rtc;

	// File descriptors that we keep open
	FILE *romFile;
	FILE *sramFile;
} gb_cart_t;

typedef struct
{
	int rate, cycles;
	byte wave[16];
	struct {
		uint on, pos;
		int cnt, encnt, swcnt;
		int len, enlen, swlen;
		int swfreq, freq;
		int envol, endir;
	} ch[4];
} gb_snd_t;

typedef struct
{
	byte y, x, pat, flags;
} gb_obj_t;

typedef struct
{
	int pat, x, v, pal, pri;
} gb_vs_t;

typedef struct
{
	byte vbank[2][8192];
	union {
		byte mem[256];
		gb_obj_t obj[40];
	} oam;
	byte pal[128];

	int BG[64];
	int WND[64];
	byte BUF[0x100];
	byte PRI[0x100];
	gb_vs_t VS[16];

	int S, T, U, V;
	int WX, WY, WT, WV;

	int cycles;

	// Fix for Fushigi no Dungeon - Fuurai no Shiren GB2 and Donkey Kong
	int enable_window_offset_hack;
} gb_lcd_t;

int  gnuboy_init(int samplerate, bool stereo, int pixformat, void *vblank_func);
int  gnuboy_load_bios(const char *file);
void gnuboy_free_bios(void);
int  gnuboy_load_rom(const char *file);
void gnuboy_free_rom(void);
int  gnuboy_load_state(const char *file);
int  gnuboy_save_state(const char *file);
int  gnuboy_load_sram(const char *file);
int  gnuboy_save_sram(const char *file, bool quick_save);
bool gnuboy_sram_dirty(void);
void gnuboy_reset(bool hard);
void gnuboy_run(bool draw);
void gnuboy_load_bank(int);
void gnuboy_set_pad(uint);

void gnuboy_get_time(int *day, int *hour, int *minute, int *second);
void gnuboy_set_time(int day, int hour, int minute, int second);
int  gnuboy_get_hwtype(void);
void gnuboy_set_hwtype(gb_hwtype_t type);
int  gnuboy_get_palette(void);
void gnuboy_set_palette(gb_palette_t pal);

void hw_emulate(int cycles);
void hw_write(uint a, byte b);
byte hw_read(uint a);

extern gb_host_t host;
extern gb_cart_t cart;
extern gb_hw_t hw;
