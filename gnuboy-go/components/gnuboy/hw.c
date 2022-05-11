#include <string.h>
#include <malloc.h>
#include "gnuboy.h"
#include "cpu.h"
#include "hw.h"
#include "tables/snd.h"
#include "tables/lcd.h"

gb_cart_t cart;
gb_hw_t hw;
gb_snd_t snd;
gb_lcd_t lcd;


/******************* BEGIN LCD *******************/

#define priused(attr) ({un32 *a = (un32*)(attr); (int)((a[0]|a[1]|a[2]|a[3]|a[4]|a[5]|a[6]|a[7])&0x80808080);})
#define blendcpy(dest, src, b, cnt) {					\
	byte *s = (src), *d = (dest), _b = (b), c = (cnt); 	\
	while(c--) *(d + c) = *(s + c) | _b; 				\
}

__attribute__((optimize("unroll-loops")))
static inline byte *get_patpix(int tile, int x)
{
	const byte *vram = lcd.vbank[0];
	static byte pix[8];

	if (tile & (1 << 11)) // Vertical Flip
		vram += ((tile & 0x3FF) << 4) | ((7 - x) << 1);
	else
		vram += ((tile & 0x3FF) << 4) | (x << 1);

	if (tile & (1 << 10)) // Horizontal Flip
		for (int k = 0; k < 8; ++k)
		{
			pix[k] = ((vram[0] >> k) & 1) | (((vram[1] >> k) & 1) << 1);
		}
	else
		for (int k = 0; k < 8; ++k)
		{
			pix[7 - k] = ((vram[0] >> k) & 1) | (((vram[1] >> k) & 1) << 1);
		}

	return pix;
}

static inline void tilebuf()
{
	int cnt, base;
	byte *tilemap, *attrmap;
	int *tilebuf;

	/* Background tiles */

	const int8_t wraptable[64] = {
		0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
		0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,-32
	};
	const int8_t *wrap = wraptable + lcd.S;

	base = ((R_LCDC&0x08)?0x1C00:0x1800) + (lcd.T<<5) + lcd.S;
	tilemap = lcd.vbank[0] + base;
	attrmap = lcd.vbank[1] + base;
	tilebuf = lcd.BG;
	cnt = ((lcd.WX + 7) >> 3) + 1;

	if (hw.hwtype == GB_HW_CGB)
	{
		if (R_LCDC & 0x10)
			for (int i = cnt; i > 0; i--)
			{
				*(tilebuf++) = *tilemap
					| (((int)*attrmap & 0x08) << 6)
					| (((int)*attrmap & 0x60) << 5);
				*(tilebuf++) = (((int)*attrmap & 0x07) << 2);
				attrmap += *wrap + 1;
				tilemap += *(wrap++) + 1;
			}
		else
			for (int i = cnt; i > 0; i--)
			{
				*(tilebuf++) = (0x100 + ((n8)*tilemap))
					| (((int)*attrmap & 0x08) << 6)
					| (((int)*attrmap & 0x60) << 5);
				*(tilebuf++) = (((int)*attrmap & 0x07) << 2);
				attrmap += *wrap + 1;
				tilemap += *(wrap++) + 1;
			}
	}
	else
	{
		if (R_LCDC & 0x10)
			for (int i = cnt; i > 0; i--)
			{
				*(tilebuf++) = *(tilemap++);
				tilemap += *(wrap++);
			}
		else
			for (int i = cnt; i > 0; i--)
			{
				*(tilebuf++) = (0x100 + ((n8)*(tilemap++)));
				tilemap += *(wrap++);
			}
	}

	if (lcd.WX >= 160) return;

	/* Window tiles */

	base = ((R_LCDC&0x40)?0x1C00:0x1800) + (lcd.WT<<5);
	tilemap = lcd.vbank[0] + base;
	attrmap = lcd.vbank[1] + base;
	tilebuf = lcd.WND;
	cnt = ((160 - lcd.WX) >> 3) + 1;

	if (hw.hwtype == GB_HW_CGB)
	{
		if (R_LCDC & 0x10)
			for (int i = cnt; i > 0; i--)
			{
				*(tilebuf++) = *(tilemap++)
					| (((int)*attrmap & 0x08) << 6)
					| (((int)*attrmap & 0x60) << 5);
				*(tilebuf++) = (((int)*(attrmap++)&0x7) << 2);
			}
		else
			for (int i = cnt; i > 0; i--)
			{
				*(tilebuf++) = (0x100 + ((n8)*(tilemap++)))
					| (((int)*attrmap & 0x08) << 6)
					| (((int)*attrmap & 0x60) << 5);
				*(tilebuf++) = (((int)*(attrmap++)&0x7) << 2);
			}
	}
	else
	{
		if (R_LCDC & 0x10)
			for (int i = cnt; i > 0; i--)
				*(tilebuf++) = *(tilemap++);
		else
			for (int i = cnt; i > 0; i--)
				*(tilebuf++) = (0x100 + ((n8)*(tilemap++)));
	}
}

static inline void bg_scan()
{
	int WX = lcd.WX;
	int cnt;
	byte *src, *dest;
	int *tile;

	if (WX <= 0) return;

	cnt = WX;
	tile = lcd.BG;
	dest = lcd.BUF;

	src = get_patpix(*(tile++), lcd.V) + lcd.U;
	memcpy(dest, src, 8-lcd.U);
	dest += 8-lcd.U;
	cnt -= 8-lcd.U;

	while (cnt >= 0)
	{
		src = get_patpix(*(tile++), lcd.V);
		memcpy(dest, src, 8);
		dest += 8;
		cnt -= 8;
	}
}

static inline void wnd_scan()
{
	int WX = lcd.WX;
	int cnt;
	byte *src, *dest;
	int *tile;

	cnt = 160 - WX;
	tile = lcd.WND;
	dest = lcd.BUF + WX;

	while (cnt >= 0)
	{
		src = get_patpix(*(tile++), lcd.WV);
		memcpy(dest, src, 8);
		dest += 8;
		cnt -= 8;
	}
}

static inline void bg_scan_pri()
{
	int WX = lcd.WX;
	int cnt, i;
	byte *src, *dest;

	if (WX <= 0) return;

	i = lcd.S;
	cnt = WX;
	dest = lcd.PRI;
	src = lcd.vbank[1] + ((R_LCDC&0x08)?0x1C00:0x1800) + (lcd.T<<5);

	if (!priused(src))
	{
		memset(dest, 0, cnt);
		return;
	}

	memset(dest, src[i++&31]&128, 8-lcd.U);
	dest += 8-lcd.U;
	cnt -= 8-lcd.U;

	if (cnt <= 0) return;

	while (cnt >= 8)
	{
		memset(dest, src[i++&31]&128, 8);
		dest += 8;
		cnt -= 8;
	}
	memset(dest, src[i&31]&128, cnt);
}

static inline void wnd_scan_pri()
{
	int WX = lcd.WX;
	int cnt, i;
	byte *src, *dest;

	if (WX >= 160) return;

	i = 0;
	cnt = 160 - WX;
	dest = lcd.PRI + WX;
	src = lcd.vbank[1] + ((R_LCDC&0x40)?0x1C00:0x1800) + (lcd.WT<<5);

	if (!priused(src))
	{
		memset(dest, 0, cnt);
		return;
	}

	while (cnt >= 8)
	{
		memset(dest, src[i++]&128, 8);
		dest += 8;
		cnt -= 8;
	}

	memset(dest, src[i]&128, cnt);
}

static inline void bg_scan_color()
{
	int WX = lcd.WX;
	int cnt;
	byte *src, *dest;
	int *tile;

	if (WX <= 0) return;

	cnt = WX;
	tile = lcd.BG;
	dest = lcd.BUF;

	src = get_patpix(*(tile++), lcd.V) + lcd.U;
	blendcpy(dest, src, *(tile++), 8-lcd.U);
	dest += 8-lcd.U;
	cnt -= 8-lcd.U;

	while (cnt >= 0)
	{
		src = get_patpix(*(tile++), lcd.V);
		blendcpy(dest, src, *(tile++), 8);
		dest += 8;
		cnt -= 8;
	}
}

static inline void wnd_scan_color()
{
	int WX = lcd.WX;
	int cnt;
	byte *src, *dest;
	int *tile;

	if (WX >= 160) return;

	cnt = 160 - WX;
	tile = lcd.WND;
	dest = lcd.BUF + WX;

	while (cnt >= 0)
	{
		src = get_patpix(*(tile++), lcd.WV);
		blendcpy(dest, src, *(tile++), 8);
		dest += 8;
		cnt -= 8;
	}
}

static inline int spr_enum()
{
	if (!(R_LCDC & 0x02))
		return 0;

	gb_vs_t ts[10];
	int line = R_LY;
	int NS = 0;

	for (int i = 0; i < 40; ++i)
	{
		gb_obj_t *obj = &lcd.oam.obj[i];
		int v, pat;

		if (line >= obj->y || line + 16 < obj->y)
			continue;
		if (line + 8 >= obj->y && !(R_LCDC & 0x04))
			continue;

		lcd.VS[NS].x = (int)obj->x - 8;
		v = line - (int)obj->y + 16;

		if (hw.hwtype == GB_HW_CGB)
		{
			pat = obj->pat | (((int)obj->flags & 0x60) << 5)
				| (((int)obj->flags & 0x08) << 6);
			lcd.VS[NS].pal = 32 + ((obj->flags & 0x07) << 2);
		}
		else
		{
			pat = obj->pat | (((int)obj->flags & 0x60) << 5);
			lcd.VS[NS].pal = 32 + ((obj->flags & 0x10) >> 2);
		}

		lcd.VS[NS].pri = (obj->flags & 0x80) >> 7;

		if ((R_LCDC & 0x04))
		{
			pat &= ~1;
			if (v >= 8)
			{
				v -= 8;
				pat++;
			}
			if (obj->flags & 0x40) pat ^= 1;
		}

		lcd.VS[NS].pat = pat;
		lcd.VS[NS].v = v;

		if (++NS == 10) break;
	}

	// Sort sprites
	if (hw.hwtype != GB_HW_CGB)
	{
		/* not quite optimal but it finally works! */
		for (int i = 0; i < NS; ++i)
		{
			int l = 0;
			int x = lcd.VS[0].x;
			for (int j = 1; j < NS; ++j)
			{
				if (lcd.VS[j].x < x)
				{
					l = j;
					x = lcd.VS[j].x;
				}
			}
			ts[i] = lcd.VS[l];
			lcd.VS[l].x = 160;
		}

		memcpy(lcd.VS, ts, sizeof(ts));
	}

	return NS;
}

static inline void spr_scan(int ns)
{
	byte *src, *dest, *bg, *pri;
	int i, b, x, pal;
	gb_vs_t *vs;
	byte bgdup[256];

	memcpy(bgdup, lcd.BUF, 256);

	vs = &lcd.VS[ns-1];

	for (; ns; ns--, vs--)
	{
		pal = vs->pal;
		x = vs->x;

		if (x >= 160 || x <= -8)
			continue;

		src = get_patpix(vs->pat, vs->v);
		dest = lcd.BUF;

		if (x < 0)
		{
			src -= x;
			i = 8 + x;
		}
		else
		{
			dest += x;
			if (x > 152) i = 160 - x;
			else i = 8;
		}

		if (vs->pri)
		{
			bg = bgdup + (dest - lcd.BUF);
			while (i--)
			{
				b = src[i];
				if (b && !(bg[i]&3)) dest[i] = pal|b;
			}
		}
		else if (hw.hwtype == GB_HW_CGB)
		{
			bg = bgdup + (dest - lcd.BUF);
			pri = lcd.PRI + (dest - lcd.BUF);
			while (i--)
			{
				b = src[i];
				if (b && (!pri[i] || !(bg[i]&3)))
					dest[i] = pal|b;
			}
		}
		else
		{
			while (i--) if (src[i]) dest[i] = pal|src[i];
		}
	}
}

static void lcd_reset(bool hard)
{
	if (hard)
	{
		memset(&lcd.vbank, 0, sizeof(lcd.vbank));
		memset(&lcd.oam, 0, sizeof(lcd.oam));
		memset(&lcd.pal, 0, sizeof(lcd.pal));
		memset(&lcd.pal, 0, sizeof(lcd.pal));
	}

	memset(lcd.BG, 0, sizeof(lcd.BG));
	memset(lcd.WND, 0, sizeof(lcd.WND));
	memset(lcd.BUF, 0, sizeof(lcd.BUF));
	memset(lcd.PRI, 0, sizeof(lcd.PRI));
	memset(lcd.VS, 0, sizeof(lcd.VS));

	lcd.WX = lcd.WT = lcd.WV = 0;
	lcd.S = lcd.T = lcd.U = lcd.V = 0;
	lcd.WY = R_WY;

	lcd_rebuildpal();
}

static inline void pal_update(byte i)
{
#ifndef IS_BIG_ENDIAN
	un32 c = ((un16*)lcd.pal)[i];
#else
	un32 c = ((lcd.pal[i << 1]) | ((lcd.pal[(i << 1) | 1]) << 8));
#endif
	un32 r = c & 0x1f;         // bit 0-4 red
	un32 g = (c >> 5) & 0x1f;  // bit 5-9 green
	un32 b = (c >> 10) & 0x1f; // bit 10-14 blue

	un32 out = (r << 11) | (g << (5 + 1)) | (b);

	if (host.lcd.format == GB_PIXEL_565_BE) {
		out = (out << 8) | (out >> 8);
	}

	host.lcd.cgb_pal[i] = out;
}

static void pal_write_cgb(byte i, byte b)
{
	if (lcd.pal[i] == b) return;
	lcd.pal[i] = b;
	pal_update(i >> 1);
}

static void pal_write_dmg(byte i, byte mapnum, byte d)
{
	un16 *map = host.lcd.dmg_pal[mapnum & 3];

	for (int j = 0; j < 8; j += 2)
	{
		int c = map[(d >> j) & 3];
		/* FIXME - handle directly without faking cgb */
		pal_write_cgb(i+j, c & 0xff);
		pal_write_cgb(i+j+1, c >> 8);
	}
}

void lcd_rebuildpal()
{
	if (hw.hwtype != GB_HW_CGB)
	{
		const uint16_t *bgp, *obp0, *obp1;

		int palette = host.lcd.colorize % GB_PALETTE_COUNT;

		if (palette == GB_PALETTE_GBC && cart.colorize)
		{
			uint8_t palette = cart.colorize & 0x1F;
			uint8_t flags = (cart.colorize & 0xE0) >> 5;

			bgp  = colorization_palettes[palette][2];
			obp0 = colorization_palettes[palette][(flags & 1) ? 0 : 1];
			obp1 = colorization_palettes[palette][(flags & 2) ? 0 : 1];

			if (!(flags & 4)) {
				obp1 = colorization_palettes[palette][2];
			}

			MESSAGE_INFO("Using GBC palette %d\n", palette);
		}
		else if (palette == GB_PALETTE_SGB)
		{
			bgp = obp0 = obp1 = custom_palettes[0];
			MESSAGE_INFO("Using SGB palette %d\n", palette);
		}
		else
		{
			bgp = obp0 = obp1 = custom_palettes[palette];
			MESSAGE_INFO("Using Built-in palette %d\n", palette);
		}

		memcpy(&host.lcd.dmg_pal[0], bgp, 8);
		memcpy(&host.lcd.dmg_pal[1], bgp, 8);
		memcpy(&host.lcd.dmg_pal[2], obp0, 8);
		memcpy(&host.lcd.dmg_pal[3], obp1, 8);

		pal_write_dmg(0, 0, R_BGP);
		pal_write_dmg(8, 1, R_BGP);
		pal_write_dmg(64, 2, R_OBP0);
		pal_write_dmg(72, 3, R_OBP1);
	}

	for (int i = 0; i < 64; i++)
	{
		pal_update(i);
	}
}


/**
 * LCD Controller routines
 */


/*
 * lcd_stat_trigger updates the STAT interrupt line to reflect whether any
 * of the conditions set to be tested (by bits 3-6 of R_STAT) are met.
 * This function should be called whenever any of the following occur:
 * 1) LY or LYC changes.
 * 2) A state transition affects the low 2 bits of R_STAT (see below).
 * 3) The program writes to the upper bits of R_STAT.
 * lcd_stat_trigger also updates bit 2 of R_STAT to reflect whether LY=LYC.
 */
static void lcd_stat_trigger()
{
	int condbits[4] = { 0x08, 0x10, 0x20, 0x00 };
	int mask = condbits[R_STAT & 3];

	if (R_LY == R_LYC)
		R_STAT |= 0x04;
	else
		R_STAT &= ~0x04;

	hw_interrupt(IF_STAT, (R_LCDC & 0x80) && ((R_STAT & 0x44) == 0x44 || (R_STAT & mask)));
}

/*
 * stat_change is called when a transition results in a change to the
 * LCD STAT condition (the low 2 bits of R_STAT).  It raises or lowers
 * the VBLANK interrupt line appropriately and calls lcd_stat_trigger to
 * update the STAT interrupt line.
 * FIXME: function now will only lower vblank interrupt, description does not match anymore
 */
static void inline stat_change(int stat)
{
	R_STAT = (R_STAT & 0x7C) | (stat & 3);
	if (stat != 1)
		hw_interrupt(IF_VBLANK, 0);
	lcd_stat_trigger();
}


static void lcd_lcdc_change(byte b)
{
	byte old = R_LCDC;
	R_LCDC = b;
	if ((R_LCDC ^ old) & 0x80) /* lcd on/off change */
	{
		R_LY = 0;
		stat_change(2);
		lcd.cycles = 40;  // Correct value seems to be 38
		lcd.WY = R_WY;
	}
}


static void lcd_hdma_cont()
{
	uint src = (R_HDMA1 << 8) | (R_HDMA2 & 0xF0);
	uint dst = 0x8000 | ((R_HDMA3 & 0x1F) << 8) | (R_HDMA4 & 0xF0);
	uint cnt = 16;

	// if (!(hw.hdma & 0x80))
	// 	return;

	while (cnt--)
		writeb(dst++, readb(src++));

	R_HDMA1 = src >> 8;
	R_HDMA2 = src & 0xF0;
	R_HDMA3 = (dst >> 8) & 0x1F;
	R_HDMA4 = dst & 0xF0;
	R_HDMA5--;
	hw.hdma--;
}

/*
	LCD controller operates with 154 lines per frame, of which lines
	#0..#143 are visible and lines #144..#153 are processed in vblank
	state.

	lcd_emulate() performs cyclic switching between lcdc states (OAM
	search/data transfer/hblank/vblank), updates system state and time
	counters accordingly. Control is returned to the caller immediately
	after a step that sets LCDC ahead of CPU, so that LCDC is always
	ahead of CPU by one state change. Once CPU reaches same point in
	time, LCDC is advanced through the next step.

	For each visible line LCDC goes through states 2 (search), 3
	(transfer) and then 0 (hblank). At the first line of vblank LCDC
	is switched to state 1 (vblank) and remains there till line #0 is
	reached (execution is still interrupted after each line so that
	function could return if it ran out of time).

	Irregardless of state switches per line, time spent in each line
	adds up to exactly 228 double-speed cycles (109us).

	LCDC emulation begins with R_LCDC set to "operation enabled", R_LY
	set to line #0 and R_STAT set to state-hblank. lcd.cycles is also
	set to zero, to begin emulation we call lcd_emulate() once to
	force-advance LCD through the first iteration.

	Docs aren't entirely accurate about time intervals within single
	line; besides that, intervals will vary depending on number of
	sprites on the line and probably other factors. States 1, 2 and 3
	do not require precise sub-line CPU-LCDC sync, but state 0 might do.
*/
static inline void lcd_renderline()
{
	if (!host.lcd.enabled || !host.lcd.buffer)
		return;

	int SL = R_LY;
	int SX = R_SCX;
	int SY = (R_SCY + SL) & 0xff;
	int WX = R_WX - 7;
	int WY = lcd.WY;
	int NS;

	if (WY>SL || WY<0 || WY>143 || WX<-7 || WX>160 || !(R_LCDC&0x20))
		lcd.WX = WX = 160;

	lcd.S = SX >> 3;
	lcd.T = SY >> 3;
	lcd.U = SX & 7;
	lcd.V = SY & 7;
	lcd.WT = (SL - WY) >> 3;
	lcd.WV = (SL - WY) & 7;

	// Fix for Fushigi no Dungeon - Fuurai no Shiren GB2 and Donkey Kong
	// This is a hack, the real problem is elsewhere
	if (lcd.enable_window_offset_hack && (R_LCDC & 0x20))
	{
		lcd.WT %= 12;
	}

	NS = spr_enum();
	tilebuf();

	if (hw.hwtype == GB_HW_CGB)
	{
		bg_scan_color();
		wnd_scan_color();
		if (NS)
		{
			bg_scan_pri();
			wnd_scan_pri();
		}
	}
	else
	{
		bg_scan();
		wnd_scan();
		blendcpy(lcd.BUF+WX, lcd.BUF+WX, 0x04, 160-WX);
	}

	spr_scan(NS);

	if (host.lcd.format == GB_PIXEL_PALETTED)
	{
		memcpy(host.lcd.buffer + SL * 160 , lcd.BUF, 160);
	}
	else
	{
		un16 *dst = (un16*)host.lcd.buffer + SL * 160;
		un16 *pal = (un16*)host.lcd.cgb_pal;

		for (int i = 0; i < 160; ++i)
			dst[i] = pal[lcd.BUF[i]];
	}
}

void lcd_emulate(int cycles)
{
	lcd.cycles -= cycles;

	if (lcd.cycles > 0)
		return;

	/* LCD disabled */
	if (!(R_LCDC & 0x80))
	{
		/* LCDC operation disabled (short route) */
		while (lcd.cycles <= 0)
		{
			switch (R_STAT & 3)
			{
			case 0: /* hblank */
			case 1: /* vblank */
				// lcd_renderline();
				stat_change(2);
				lcd.cycles += 40;
				break;
			case 2: /* search */
				stat_change(3);
				lcd.cycles += 86;
				break;
			case 3: /* transfer */
				stat_change(0);
				/* FIXME: check docs; HDMA might require operating LCDC */
				if (hw.hdma & 0x80)
					lcd_hdma_cont();
				else
					lcd.cycles += 102;
				break;
			}
			return;
		}
	}

	while (lcd.cycles <= 0)
	{
		switch (R_STAT & 3)
		{
		case 0:
			/* hblank -> */
			if (++R_LY >= 144)
			{
				/* FIXME: pick _one_ place to trigger vblank interrupt
				this better be done here or within stat_change(),
				otherwise CPU will have a chance to run	for some time
				before interrupt is triggered */
				if (cpu.halted)
				{
					hw_interrupt(IF_VBLANK, 1);
					lcd.cycles += 228;
				}
				else lcd.cycles += 10;
				stat_change(1); /* -> vblank */
				break;
			}

			// Hack for Worms Armageddon
			if (R_STAT == 0x48)
				hw_interrupt(IF_STAT, 0);

			stat_change(2); /* -> search */
			lcd.cycles += 40;
			break;
		case 1:
			/* vblank -> */
			if (!(hw.ilines & IF_VBLANK))
			{
				hw_interrupt(IF_VBLANK, 1);
				lcd.cycles += 218;
				break;
			}
			if (R_LY == 0)
			{
				lcd.WY = R_WY;
				stat_change(2); /* -> search */
				lcd.cycles += 40;
				break;
			}
			else if (R_LY < 152)
				lcd.cycles += 228;
			else if (R_LY == 152)
				/* Handling special case on the last line; see
				docs/HACKING */
				lcd.cycles += 28;
			else
			{
				R_LY = -1;
				lcd.cycles += 200;
			}
			R_LY++;
			lcd_stat_trigger();
			break;
		case 2:
			/* search -> */
			lcd_renderline();
			stat_change(3); /* -> transfer */
			lcd.cycles += 86;
			break;
		case 3:
			/* transfer -> */
			stat_change(0); /* -> hblank */
			if (hw.hdma & 0x80)
				lcd_hdma_cont();
			/* FIXME -- how much of the hblank does hdma use?? */
			/* else */
			lcd.cycles += 102;
			break;
		}
	}
}

/******************* END LCD *******************/


/******************* BEGIN SOUND *******************/

#define S1 (snd.ch[0])
#define S2 (snd.ch[1])
#define S3 (snd.ch[2])
#define S4 (snd.ch[3])

#define s1_freq() {int d = 2048 - (((R_NR14&7)<<8) + R_NR13); S1.freq = (snd.rate > (d<<4)) ? 0 : (snd.rate << 17)/d;}
#define s2_freq() {int d = 2048 - (((R_NR24&7)<<8) + R_NR23); S2.freq = (snd.rate > (d<<4)) ? 0 : (snd.rate << 17)/d;}
#define s3_freq() {int d = 2048 - (((R_NR34&7)<<8) + R_NR33); S3.freq = (snd.rate > (d<<3)) ? 0 : (snd.rate << 21)/d;}
#define s4_freq() {S4.freq = (freqtab[R_NR43&7] >> (R_NR43 >> 4)) * snd.rate; if (S4.freq >> 18) S4.freq = 1<<18;}

static void sound_off(void)
{
	memset(&S1, 0, sizeof S1);
	memset(&S2, 0, sizeof S2);
	memset(&S3, 0, sizeof S3);
	memset(&S4, 0, sizeof S4);
	R_NR10 = 0x80;
	R_NR11 = 0xBF;
	R_NR12 = 0xF3;
	R_NR14 = 0xBF;
	R_NR21 = 0x3F;
	R_NR22 = 0x00;
	R_NR24 = 0xBF;
	R_NR30 = 0x7F;
	R_NR31 = 0xFF;
	R_NR32 = 0x9F;
	R_NR34 = 0xBF;
	R_NR41 = 0xFF;
	R_NR42 = 0x00;
	R_NR43 = 0x00;
	R_NR44 = 0xBF;
	R_NR50 = 0x77;
	R_NR51 = 0xF3;
	R_NR52 = 0x70;
	sound_dirty();
}

void sound_dirty()
{
	S1.swlen = ((R_NR10>>4) & 7) << 14;
	S1.len = (64-(R_NR11&63)) << 13;
	S1.envol = R_NR12 >> 4;
	S1.endir = (R_NR12>>3) & 1;
	S1.endir |= S1.endir - 1;
	S1.enlen = (R_NR12 & 7) << 15;
	s1_freq();

	S2.len = (64-(R_NR21&63)) << 13;
	S2.envol = R_NR22 >> 4;
	S2.endir = (R_NR22>>3) & 1;
	S2.endir |= S2.endir - 1;
	S2.enlen = (R_NR22 & 7) << 15;
	s2_freq();

	S3.len = (256-R_NR31) << 20;
	s3_freq();

	S4.len = (64-(R_NR41&63)) << 13;
	S4.envol = R_NR42 >> 4;
	S4.endir = (R_NR42>>3) & 1;
	S4.endir |= S4.endir - 1;
	S4.enlen = (R_NR42 & 7) << 15;
	s4_freq();
}

static void sound_reset(bool hard)
{
	memset(snd.ch, 0, sizeof(snd.ch));
	memcpy(snd.wave, hw.hwtype == GB_HW_CGB ? cgbwave : dmgwave, 16);
	memcpy(hw.ioregs + 0x30, snd.wave, 16);
	snd.rate = (int)(((1<<21) / (double)host.snd.samplerate) + 0.5);
	snd.cycles = 0;
	host.snd.pos = 0;
	sound_off();
	R_NR52 = 0xF1;
}

static void sound_emulate(void)
{
	if (!snd.rate || snd.cycles < snd.rate)
		return;

	for (; snd.cycles >= snd.rate; snd.cycles -= snd.rate)
	{
		int l = 0;
		int r = 0;

		if (S1.on)
		{
			int s = sqwave[R_NR11>>6][(S1.pos>>18)&7] & S1.envol;
			S1.pos += S1.freq;

			if ((R_NR14 & 64) && ((S1.cnt += snd.rate) >= S1.len))
				S1.on = 0;

			if (S1.enlen && (S1.encnt += snd.rate) >= S1.enlen)
			{
				S1.encnt -= S1.enlen;
				S1.envol += S1.endir;
				if (S1.envol < 0) S1.envol = 0;
				if (S1.envol > 15) S1.envol = 15;
			}

			if (S1.swlen && (S1.swcnt += snd.rate) >= S1.swlen)
			{
				S1.swcnt -= S1.swlen;
				int f = S1.swfreq;

				if (R_NR10 & 8)
					f -= (f >> (R_NR10 & 7));
				else
					f += (f >> (R_NR10 & 7));

				if (f > 2047)
					S1.on = 0;
				else
				{
					S1.swfreq = f;
					R_NR13 = f;
					R_NR14 = (R_NR14 & 0xF8) | (f>>8);
					s1_freq();
				}
			}
			s <<= 2;
			if (R_NR51 & 1) r += s;
			if (R_NR51 & 16) l += s;
		}

		if (S2.on)
		{
			int s = sqwave[R_NR21>>6][(S2.pos>>18)&7] & S2.envol;
			S2.pos += S2.freq;

			if ((R_NR24 & 64) && ((S2.cnt += snd.rate) >= S2.len))
				S2.on = 0;

			if (S2.enlen && (S2.encnt += snd.rate) >= S2.enlen)
			{
				S2.encnt -= S2.enlen;
				S2.envol += S2.endir;
				if (S2.envol < 0) S2.envol = 0;
				if (S2.envol > 15) S2.envol = 15;
			}
			s <<= 2;
			if (R_NR51 & 2) r += s;
			if (R_NR51 & 32) l += s;
		}

		if (S3.on)
		{
			int s = snd.wave[(S3.pos>>22) & 15];

			if (S3.pos & (1<<21))
				s &= 15;
			else
				s >>= 4;

			s -= 8;
			S3.pos += S3.freq;

			if ((R_NR34 & 64) && ((S3.cnt += snd.rate) >= S3.len))
				S3.on = 0;

			if (R_NR32 & 96)
				s <<= (3 - ((R_NR32>>5)&3));
			else
				s = 0;

			if (R_NR51 & 4) r += s;
			if (R_NR51 & 64) l += s;
		}

		if (S4.on)
		{
			int s;

			if (R_NR43 & 8)
				s = 1 & (noise7[(S4.pos>>20)&15] >> (7-((S4.pos>>17)&7)));
			else
				s = 1 & (noise15[(S4.pos>>20)&4095] >> (7-((S4.pos>>17)&7)));

			s = (-s) & S4.envol;
			S4.pos += S4.freq;

			if ((R_NR44 & 64) && ((S4.cnt += snd.rate) >= S4.len))
				S4.on = 0;

			if (S4.enlen && (S4.encnt += snd.rate) >= S4.enlen)
			{
				S4.encnt -= S4.enlen;
				S4.envol += S4.endir;
				if (S4.envol < 0) S4.envol = 0;
				if (S4.envol > 15) S4.envol = 15;
			}

			s += s << 1;

			if (R_NR51 & 8) r += s;
			if (R_NR51 & 128) l += s;
		}

		l *= (R_NR50 & 0x07);
		r *= ((R_NR50 & 0x70)>>4);

		l <<= 4;
		r <<= 4;

		if (host.snd.buffer == NULL)
		{
			MESSAGE_DEBUG("no audio buffer... (output.len=%d)\n", host.snd.len);
		}
		else if (host.snd.pos >= host.snd.len)
		{
			MESSAGE_ERROR("buffer overflow. (output.len=%d)\n", host.snd.len);
			host.snd.pos = 0;
		}
		else if (host.snd.stereo)
		{
			host.snd.buffer[host.snd.pos++] = (n16)l; //+128;
			host.snd.buffer[host.snd.pos++] = (n16)r; //+128;
		}
		else
		{
			host.snd.buffer[host.snd.pos++] = (n16)((l+r)>>1); //+128;
		}
	}
	R_NR52 = (R_NR52&0xf0) | S1.on | (S2.on<<1) | (S3.on<<2) | (S4.on<<3);
}

static void sound_write(byte r, byte b)
{
	if (!(R_NR52 & 128) && r != RI_NR52)
		return;

	if ((r & 0xF0) == 0x30)
	{
		if (S3.on && snd.cycles >= snd.rate)
			sound_emulate();
		if (!S3.on)
			snd.wave[r-0x30] = hw.ioregs[r] = b;
		return;
	}

	if (snd.cycles >= snd.rate)
		sound_emulate();

	switch (r)
	{
	case RI_NR10:
		R_NR10 = b;
		S1.swlen = ((R_NR10>>4) & 7) << 14;
		S1.swfreq = ((R_NR14&7)<<8) + R_NR13;
		break;
	case RI_NR11:
		R_NR11 = b;
		S1.len = (64-(R_NR11&63)) << 13;
		break;
	case RI_NR12:
		R_NR12 = b;
		S1.envol = R_NR12 >> 4;
		S1.endir = (R_NR12>>3) & 1;
		S1.endir |= S1.endir - 1;
		S1.enlen = (R_NR12 & 7) << 15;
		break;
	case RI_NR13:
		R_NR13 = b;
		s1_freq();
		break;
	case RI_NR14:
		R_NR14 = b;
		s1_freq();
		if (b & 0x80)
		{
			S1.swcnt = 0;
			S1.swfreq = ((R_NR14&7)<<8) + R_NR13;
			S1.envol = R_NR12 >> 4;
			S1.endir = (R_NR12>>3) & 1;
			S1.endir |= S1.endir - 1;
			S1.enlen = (R_NR12 & 7) << 15;
			if (!S1.on) S1.pos = 0;
			S1.on = 1;
			S1.cnt = 0;
			S1.encnt = 0;
		}
		break;
	case RI_NR21:
		R_NR21 = b;
		S2.len = (64-(R_NR21&63)) << 13;
		break;
	case RI_NR22:
		R_NR22 = b;
		S2.envol = R_NR22 >> 4;
		S2.endir = (R_NR22>>3) & 1;
		S2.endir |= S2.endir - 1;
		S2.enlen = (R_NR22 & 7) << 15;
		break;
	case RI_NR23:
		R_NR23 = b;
		s2_freq();
		break;
	case RI_NR24:
		R_NR24 = b;
		s2_freq();
		if (b & 0x80)
		{
			S2.envol = R_NR22 >> 4;
			S2.endir = (R_NR22>>3) & 1;
			S2.endir |= S2.endir - 1;
			S2.enlen = (R_NR22 & 7) << 15;
			if (!S2.on) S2.pos = 0;
			S2.on = 1;
			S2.cnt = 0;
			S2.encnt = 0;
		}
		break;
	case RI_NR30:
		R_NR30 = b;
		if (!(b & 128)) S3.on = 0;
		break;
	case RI_NR31:
		R_NR31 = b;
		S3.len = (256-R_NR31) << 13;
		break;
	case RI_NR32:
		R_NR32 = b;
		break;
	case RI_NR33:
		R_NR33 = b;
		s3_freq();
		break;
	case RI_NR34:
		R_NR34 = b;
		s3_freq();
		if (b & 0x80)
		{
			if (!S3.on) S3.pos = 0;
			S3.cnt = 0;
			S3.on = R_NR30 >> 7;
			if (!S3.on) return;
			for (int i = 0; i < 16; i++)
				hw.ioregs[i+0x30] = 0x13 ^ hw.ioregs[i+0x31];
		}
		break;
	case RI_NR41:
		R_NR41 = b;
		S4.len = (64-(R_NR41&63)) << 13;
		break;
	case RI_NR42:
		R_NR42 = b;
		S4.envol = R_NR42 >> 4;
		S4.endir = (R_NR42>>3) & 1;
		S4.endir |= S4.endir - 1;
		S4.enlen = (R_NR42 & 7) << 15;
		break;
	case RI_NR43:
		R_NR43 = b;
		s4_freq();
		break;
	case RI_NR44:
		R_NR44 = b;
		if (b & 0x80)
		{
			S4.envol = R_NR42 >> 4;
			S4.endir = (R_NR42>>3) & 1;
			S4.endir |= S4.endir - 1;
			S4.enlen = (R_NR42 & 7) << 15;
			S4.on = 1;
			S4.pos = 0;
			S4.cnt = 0;
			S4.encnt = 0;
		}
		break;
	case RI_NR50:
		R_NR50 = b;
		break;
	case RI_NR51:
		R_NR51 = b;
		break;
	case RI_NR52:
		R_NR52 = b;
		if (!(R_NR52 & 128))
			sound_off();
		break;
	default:
		return;
	}
}

/******************* END SOUND *******************/


/******************* BEGIN RTC *******************/

static void rtc_latch(byte b)
{
	if ((cart.rtc.latch ^ b) & b & 1)
	{
		cart.rtc.regs[0] = cart.rtc.s;
		cart.rtc.regs[1] = cart.rtc.m;
		cart.rtc.regs[2] = cart.rtc.h;
		cart.rtc.regs[3] = cart.rtc.d;
		cart.rtc.regs[4] = cart.rtc.flags;
	}
	cart.rtc.latch = b & 1;
}


static void rtc_write(byte b)
{
	switch (cart.rtc.sel & 0xf)
	{
	case 0x8: // Seconds
		cart.rtc.regs[0] = b;
		cart.rtc.s = b % 60;
		break;
	case 0x9: // Minutes
		cart.rtc.regs[1] = b;
		cart.rtc.m = b % 60;
		break;
	case 0xA: // Hours
		cart.rtc.regs[2] = b;
		cart.rtc.h = b % 24;
		break;
	case 0xB: // Days (lower 8 bits)
		cart.rtc.regs[3] = b;
		cart.rtc.d = ((cart.rtc.d & 0x100) | b) % 365;
		break;
	case 0xC: // Flags (days upper 1 bit, carry, stop)
		cart.rtc.regs[4] = b;
		cart.rtc.flags = b;
		cart.rtc.d = ((cart.rtc.d & 0xff) | ((b&1)<<9)) % 365;
		break;
	}
}


static void rtc_tick()
{
	if ((cart.rtc.flags & 0x40))
		return; // rtc stop

	if (++cart.rtc.ticks >= 60)
	{
		if (++cart.rtc.s >= 60)
		{
			if (++cart.rtc.m >= 60)
			{
				if (++cart.rtc.h >= 24)
				{
					if (++cart.rtc.d >= 365)
					{
						cart.rtc.d = 0;
						cart.rtc.flags |= 0x80;
					}
					cart.rtc.h = 0;
				}
				cart.rtc.m = 0;
			}
			cart.rtc.s = 0;
		}
		cart.rtc.ticks = 0;
	}
}

/******************* END RTC *******************/

/*
 * hw_interrupt changes the virtual interrupt line(s) defined by i
 * The interrupt fires (added to R_IF) when the line transitions from 0 to 1.
 * It does not refire if the line was already high.
 */
void hw_interrupt(byte i, int level)
{
	if (level == 0)
	{
		hw.ilines &= ~i;
	}
	else if ((hw.ilines & i) == 0)
	{
		hw.ilines |= i;
		R_IF |= i; // Fire!

		if ((R_IE & i) != 0)
		{
			// Wake up the CPU when an enabled interrupt occurs
			// IME doesn't matter at this point, only IE
			cpu.halted = 0;
		}
	}
}


/*
 * hw_dma performs plain old memory-to-oam dma, the original dmg
 * dma. Although on the hardware it takes a good deal of time, the cpu
 * continues running during this mode of dma, so no special tricks to
 * stall the cpu are necessary.
 */
static void hw_dma(uint b)
{
	uint a = b << 8;
	for (int i = 0; i < 160; i++, a++)
		lcd.oam.mem[i] = readb(a);
}


static void hw_hdma(byte c)
{
	/* Begin or cancel HDMA */
	if ((hw.hdma|c) & 0x80)
	{
		hw.hdma = c;
		R_HDMA5 = c & 0x7f;
		return;
	}

	/* Perform GDMA */
	uint src = (R_HDMA1 << 8) | (R_HDMA2 & 0xF0);
	uint dst = 0x8000 | ((R_HDMA3 & 0x1F) << 8) | (R_HDMA4 & 0xF0);
	uint cnt = c + 1;

	/* FIXME - this should use cpu time! */
	/*cpu_timers(102 * cnt);*/

	cnt <<= 4;

	while (cnt--)
		writeb(dst++, readb(src++));

	R_HDMA1 = src >> 8;
	R_HDMA2 = src & 0xF0;
	R_HDMA3 = (dst >> 8) & 0x1F;
	R_HDMA4 = dst & 0xF0;
	R_HDMA5 = 0xFF;
}

/*
 * pad_refresh updates the P1 register from the pad states, generating
 * the appropriate interrupts (by quickly raising and lowering the
 * interrupt line) if a transition has been made.
 */
static inline void pad_refresh()
{
	byte oldp1 = R_P1;
	R_P1 &= 0x30;
	R_P1 |= 0xc0;
	if (!(R_P1 & 0x10)) R_P1 |= (hw.pad & 0x0F);
	if (!(R_P1 & 0x20)) R_P1 |= (hw.pad >> 4);
	R_P1 ^= 0x0F;
	if (oldp1 & ~R_P1 & 0x0F)
	{
		hw_interrupt(IF_PAD, 1);
		hw_interrupt(IF_PAD, 0);
	}
}


/*
 * hw_setpad updates the state of one or more buttons on the pad and calls
 * pad_refresh() to fire an interrupt if the pad changed.
 */
void hw_setpad(un32 new_pad)
{
	hw.pad = new_pad & 0xFF;
	pad_refresh();
}


void hw_reset(bool hard)
{
	hw.ilines = 0;
	hw.serial = 0;
	hw.hdma = 0;
	hw.pad = 0;

	memset(hw.ioregs, 0, sizeof(hw.ioregs));
	R_P1 = 0xFF;
	R_LCDC = 0x91;
	R_BGP = 0xFC;
	R_OBP0 = 0xFF;
	R_OBP1 = 0xFF;
	R_SVBK = 0xF9;
	R_HDMA5 = 0xFF;
	R_VBK = 0xFE;

	if (hard)
	{
		memset(hw.rambanks, 0xff, 4096 * 8);
		memset(cart.rambanks, 0xff, 8192 * cart.ramsize);
		memset(&cart.rtc, 0, sizeof(cart.rtc));
	}

	memset(hw.rmap, 0, sizeof(hw.rmap));
	memset(hw.wmap, 0, sizeof(hw.wmap));

	cart.sram_dirty = 0;
	cart.bankmode = 0;
	cart.rombank = 1;
	cart.rambank = 0;
	cart.enableram = 0;
	hw_updatemap();

	lcd_reset(hard);
	sound_reset(hard);
}


/*
 * hw_vblank is called once per frame at vblank and should take care
 * of things like rtc/sound/serial advance, emulation throttling, etc.
 */
void hw_vblank(void)
{
	hw.frames++;
	rtc_tick();
	sound_emulate();

	if (host.lcd.enabled && host.lcd.vblank)
		(host.lcd.vblank)();
}


/*
 * In order to make reads and writes efficient, we keep tables
 * (indexed by the high nibble of the address) specifying which
 * regions can be read/written without a function call. For such
 * ranges, the pointer in the map table points to the base of the
 * region in host system memory. For ranges that require special
 * processing, the pointer is NULL.
 *
 * hw_updatemap is called whenever bank changes or other operations
 * make the old maps potentially invalid.
 */
void hw_updatemap(void)
{
	int rombank = cart.rombank & (cart.romsize - 1);

	if (cart.rombanks[rombank] == NULL)
	{
		gnuboy_load_bank(rombank);
	}

	// ROM
	hw.rmap[0x0] = cart.rombanks[0];
	hw.rmap[0x1] = cart.rombanks[0];
	hw.rmap[0x2] = cart.rombanks[0];
	hw.rmap[0x3] = cart.rombanks[0];

	// Force bios to go through hw_read (speed doesn't really matter here)
	if (hw.bios && (R_BIOS & 1) == 0)
	{
		hw.rmap[0x0] = NULL;
	}

	// Cartridge ROM
	hw.rmap[0x4] = cart.rombanks[rombank] - 0x4000;
	hw.rmap[0x5] = hw.rmap[0x4];
	hw.rmap[0x6] = hw.rmap[0x4];
	hw.rmap[0x7] = hw.rmap[0x4];

	// Video RAM
	hw.rmap[0x8] = hw.wmap[0x8] = lcd.vbank[R_VBK & 1] - 0x8000;
	hw.rmap[0x9] = hw.wmap[0x9] = lcd.vbank[R_VBK & 1] - 0x8000;

	// Cartridge RAM
	hw.rmap[0xA] = hw.wmap[0xA] = NULL;
	hw.rmap[0xB] = hw.wmap[0xB] = NULL;

	// Work RAM
	hw.rmap[0xC] = hw.wmap[0xC] = hw.rambanks[0] - 0xC000;

	// Work RAM (GBC)
	if (hw.hwtype == GB_HW_CGB)
		hw.rmap[0xD] = hw.wmap[0xD] = hw.rambanks[(R_SVBK & 0x7) ?: 1] - 0xD000;

	// Mirror of 0xC000
	hw.rmap[0xE] = hw.wmap[0xE] = hw.rambanks[0] - 0xE000;

	// IO port and registers
	hw.rmap[0xF] = hw.wmap[0xF] = NULL;
}


/*
 * Memory bank controllers typically intercept write attempts to
 * 0000-7FFF, using the address and byte written as instructions to
 * change cart or sram banks, control special hardware, etc.
 *
 * mbc_write takes an address (which should be in the proper range)
 * and a byte value written to the address.
 */
static inline void mbc_write(uint a, byte b)
{
	MESSAGE_DEBUG("mbc %d: cart bank %02X -[%04X:%02X]-> ", cart.mbc, cart.rombank, a, b);

	switch (cart.mbc)
	{
	case MBC_MBC1:
		switch (a & 0xE000)
		{
		case 0x0000:
			cart.enableram = ((b & 0x0F) == 0x0A);
			break;
		case 0x2000:
			if ((b & 0x1F) == 0) b = 0x01;
			cart.rombank = (cart.rombank & 0x60) | (b & 0x1F);
			break;
		case 0x4000:
			if (cart.bankmode)
				cart.rambank = b & 0x03;
			else
				cart.rombank = (cart.rombank & 0x1F) | ((int)(b&3)<<5);
			break;
		case 0x6000:
			cart.bankmode = b & 0x1;
			break;
		}
		break;

	case MBC_MBC2:
		// 0x0000 - 0x3FFF, bit 0x0100 clear = RAM, set = ROM
		if ((a & 0xC100) == 0x0000)
			cart.enableram = ((b & 0x0F) == 0x0A);
		else if ((a & 0xC100) == 0x0100)
			cart.rombank = b & 0x0F;
		break;

	case MBC_HUC3:
		// FIX ME: This isn't right but the previous implementation was wronger...
	case MBC_MBC3:
		switch (a & 0xE000)
		{
		case 0x0000:
			cart.enableram = ((b & 0x0F) == 0x0A);
			break;
		case 0x2000:
			if ((b & 0x7F) == 0) b = 0x01;
			cart.rombank = b & 0x7F;
			break;
		case 0x4000:
			cart.rtc.sel = b & 0x0f;
			cart.rambank = b & 0x03;
			break;
		case 0x6000:
			rtc_latch(b);
			break;
		}
		break;

	case MBC_MBC5:
		switch (a & 0x7000)
		{
		case 0x0000:
		case 0x1000:
			cart.enableram = ((b & 0x0F) == 0x0A);
			break;
		case 0x2000:
			cart.rombank = (cart.rombank & 0x100) | (b);
			break;
		case 0x3000:
			cart.rombank = (cart.rombank & 0xFF) | ((int)(b & 1) << 8);
			break;
		case 0x4000:
		case 0x5000:
			cart.rambank = b & (cart.has_rumble ? 0x7 : 0xF);
			cart.rambank &= (cart.ramsize - 1);
			break;
		case 0x6000:
		case 0x7000:
			// Nothing but Radikal Bikers tries to access it.
			break;
		default:
			MESSAGE_ERROR("MBC_MBC5: invalid write to 0x%x (0x%x)\n", a, b);
			break;
		}
		break;

	case MBC_HUC1: /* FIXME - this is all guesswork -- is it right??? */
		switch (a & 0xE000)
		{
		case 0x0000:
			cart.enableram = ((b & 0x0F) == 0x0A);
			break;
		case 0x2000:
			if ((b & 0x1F) == 0) b = 0x01;
			cart.rombank = (cart.rombank & 0x60) | (b & 0x1F);
			break;
		case 0x4000:
			if (cart.bankmode)
				cart.rambank = b & 0x03 & (cart.ramsize - 1);
			else
				cart.rombank = (cart.rombank & 0x1F) | ((int)(b&3)<<5);
			break;
		case 0x6000:
			cart.bankmode = b & 0x1;
			break;
		}
		break;
	}

	MESSAGE_DEBUG("%02X\n", cart.rombank);

	hw_updatemap();
}


/*
 * hw_write is the basic write function. Although it should only be
 * called when the write map contains a NULL for the requested address
 * region, it accepts writes to any address.
 */
void hw_write(uint a, byte b)
{
	MESSAGE_DEBUG("write to 0x%04X: 0x%02X\n", a, b);

	switch (a & 0xE000)
	{
	case 0x0000: // MBC control (Cart ROM address space)
	case 0x2000:
	case 0x4000:
	case 0x6000:
		mbc_write(a, b);
		break;

	case 0x8000: // Video RAM
		lcd.vbank[R_VBK&1][a & 0x1FFF] = b;
		break;

	case 0xA000: // Save RAM or RTC
		if (!cart.enableram)
			break;

		if (cart.rtc.sel & 8)
		{
			rtc_write(b);
		}
		else
		{
			if (cart.rambanks[cart.rambank][a & 0x1FFF] != b)
			{
				cart.rambanks[cart.rambank][a & 0x1FFF] = b;
				cart.sram_dirty |= (1 << cart.rambank);
			}
		}
		break;

	case 0xC000: // System RAM
		if ((a & 0xF000) == 0xC000)
			hw.rambanks[0][a & 0x0FFF] = b;
		else
			hw.rambanks[(R_SVBK & 0x7) ?: 1][a & 0x0FFF] = b;
		break;

	case 0xE000: // Memory mapped IO
		// Mirror RAM: 0xE000 - 0xFDFF
		if (a < 0xFE00)
		{
			hw.rambanks[(a >> 12) & 1][a & 0xFFF] = b;
		}
		// Video: 0xFE00 - 0xFE9F
		else if ((a & 0xFF00) == 0xFE00)
		{
			if (a < 0xFEA0) lcd.oam.mem[a & 0xFF] = b;
		}
		// Sound: 0xFF10 - 0xFF3F
		else if (a >= 0xFF10 && a <= 0xFF3F)
		{
			sound_write(a & 0xFF, b);
		}
		// High RAM: 0xFF80 - 0xFFFE
		else if (a >= 0xFF80 && a <= 0xFFFE)
		{
			REG(a & 0xFF) = b;
		}
		// Hardware registers: 0xFF00 - 0xFF7F and 0xFFFF
		else
		{
			int r = a & 0xFF;
			if (hw.hwtype != GB_HW_CGB)
			{
				switch (r)
				{
				case RI_BGP:
					pal_write_dmg(0, 0, b);
					pal_write_dmg(8, 1, b);
					break;
				case RI_OBP0:
					pal_write_dmg(64, 2, b);
					break;
				case RI_OBP1:
					pal_write_dmg(72, 3, b);
					break;

				// These don't exist on DMG:
				case RI_VBK:
				case RI_BCPS:
				case RI_OCPS:
				case RI_BCPD:
				case RI_OCPD:
				case RI_SVBK:
				case RI_KEY1:
				case RI_HDMA1:
				case RI_HDMA2:
				case RI_HDMA3:
				case RI_HDMA4:
				case RI_HDMA5:
					return;
				}
			}

			switch(r)
			{
			case RI_TIMA:
			case RI_TMA:
			case RI_TAC:
			case RI_SCY:
			case RI_SCX:
			case RI_WY:
			case RI_WX:
			case RI_BGP:
			case RI_OBP0:
			case RI_OBP1:
				REG(r) = b;
				break;
			case RI_IF:
			case RI_IE:
				REG(r) = b & 0x1F;
				break;
			case RI_P1:
				REG(r) = b;
				pad_refresh();
				break;
			case RI_SC:
				if ((b & 0x81) == 0x81)
					hw.serial = 1952; // 8 * 122us;
				else
					hw.serial = 0;
				R_SC = b; /* & 0x7f; */
				break;
			case RI_SB:
				REG(r) = b;
				break;
			case RI_DIV:
				REG(r) = 0;
				break;
			case RI_LCDC:
				lcd_lcdc_change(b);
				break;
			case RI_STAT:
				R_STAT = (R_STAT & 0x07) | (b & 0x78);
				if (hw.hwtype != GB_HW_CGB && !(R_STAT & 2)) /* DMG STAT write bug => interrupt */
					hw_interrupt(IF_STAT, 1);
				lcd_stat_trigger();
				break;
			case RI_LYC:
				REG(r) = b;
				lcd_stat_trigger();
				break;
			case RI_VBK:
				REG(r) = b | 0xFE;
				hw_updatemap();
				break;
			case RI_BCPS:
				R_BCPS = b & 0xBF;
				R_BCPD = lcd.pal[b & 0x3F];
				break;
			case RI_OCPS:
				R_OCPS = b & 0xBF;
				R_OCPD = lcd.pal[64 + (b & 0x3F)];
				break;
			case RI_BCPD:
				R_BCPD = b;
				pal_write_cgb(R_BCPS & 0x3F, b);
				if (R_BCPS & 0x80) R_BCPS = (R_BCPS+1) & 0xBF;
				break;
			case RI_OCPD:
				R_OCPD = b;
				pal_write_cgb(64 + (R_OCPS & 0x3F), b);
				if (R_OCPS & 0x80) R_OCPS = (R_OCPS+1) & 0xBF;
				break;
			case RI_SVBK:
				REG(r) = b | 0xF8;
				hw_updatemap();
				break;
			case RI_DMA:
				hw_dma(b);
				break;
			case RI_KEY1:
				REG(r) = (REG(r) & 0x80) | (b & 0x01);
				break;
			case RI_BIOS:
				REG(r) = b;
				hw_updatemap();
				break;
			case RI_HDMA1:
			case RI_HDMA2:
			case RI_HDMA3:
			case RI_HDMA4:
				REG(r) = b;
				break;
			case RI_HDMA5:
				hw_hdma(b);
				break;
			}
		}
	}
}


/*
 * hw_read is the basic read function...not useful for much anymore
 * with the read map, but it's still necessary for the final messy
 * region.
 */
byte hw_read(uint a)
{
	MESSAGE_DEBUG("read %04x\n", a);

	switch (a & 0xE000)
	{
	case 0x0000: // Cart ROM bank 0
		// BIOS is overlayed part of bank 0
		if (a < 0x900 && (R_BIOS & 1) == 0)
		{
			if (a < 0x100)
				return hw.bios[a];
			if (a >= 0x200 && hw.hwtype == GB_HW_CGB)
				return hw.bios[a];
		}
		// fall through
	case 0x2000:
		return cart.rombanks[0][a & 0x3fff];

	case 0x4000: // Cart ROM
	case 0x6000:
		return cart.rombanks[cart.rombank][a & 0x3FFF];

	case 0x8000: // Video RAM
		return lcd.vbank[R_VBK&1][a & 0x1FFF];

	case 0xA000: // Cart RAM or RTC
		if (!cart.enableram)
			return 0xFF;
		if (cart.rtc.sel & 8)
			return cart.rtc.regs[cart.rtc.sel & 7];
		return cart.rambanks[cart.rambank][a & 0x1FFF];

	case 0xC000: // System RAM
		if ((a & 0xF000) == 0xC000)
			return hw.rambanks[0][a & 0xFFF];
		return hw.rambanks[(R_SVBK & 0x7) ?: 1][a & 0xFFF];

	case 0xE000: // Memory mapped IO
		// Mirror RAM: 0xE000 - 0xFDFF
		if (a < 0xFE00)
		{
			return hw.rambanks[(a >> 12) & 1][a & 0xFFF];
		}
		// Video: 0xFE00 - 0xFE9F
		else if ((a & 0xFF00) == 0xFE00)
		{
			return (a < 0xFEA0) ? lcd.oam.mem[a & 0xFF] : 0xFF;
		}
		// Sound: 0xFF10 - 0xFF3F
		else if (a >= 0xFF10 && a <= 0xFF3F)
		{
			// Make sure sound emulation is all caught up
			sound_emulate();
		}
		// High RAM: 0xFF80 - 0xFFFE
		// else if ((a & 0xFF80) == 0xFF80)
		// {
		// 	return REG(a & 0xFF);
		// }
		// Hardware registers: 0xFF00 - 0xFF7F and 0xFFFF
		// else
		// {

			// We should check that the reg is valid, otherwise return 0xFF.
			// However invalid address should already contain 0xFF (unless incorrectly overwritten)
			// So, for speed, we'll assume that this is correct and always return REG(a)
			return REG(a & 0xFF);
		// }
	}
	return 0xFF;
}
