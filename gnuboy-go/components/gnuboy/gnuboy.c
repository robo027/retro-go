#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "gnuboy.h"
#include "internal.h"
#include "tables/snd.h"
#include "tables/lcd.h"
#include "tables/cpu.h"

static gb_snd_t snd;
static gb_lcd_t lcd;
static gb_cart_t cart;
static gb_hw_t hw;
static cpu_t cpu;
gb_host_t host;

static void hw_hdma(int c);
static void hw_interrupt(byte i, int level);


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

static void lcd_rebuildpal()
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
		pal_update(i);
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

	/* set lcdc ahead of cpu by 19us; see A
			Set lcdc ahead of cpu by 19us (matches minimal hblank duration according
			to some docs). Value from lcd.cycles (when positive) is used to drive CPU,
			setting some ahead-time at startup is necessary to begin emulation.
	FIXME: leave value at 0, use lcd_emulate() to actually send lcdc ahead
	*/
	lcd.cycles = 40;
}

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
static void lcd_renderline(void)
{
	if (!host.lcd.enabled || !host.lcd.buffer)
		return;

	int SL = R_LY;
	int SX = R_SCX;
	int SY = (R_SCY + SL) & 0xff;
	int WX = R_WX - 7;
	int WY = lcd.WY;
	int NS = 0;

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

	// Sprites enum
	if ((R_LCDC & 0x02))
	{
		for (int i = 0; i < 40; ++i)
		{
			gb_obj_t *obj = &lcd.oam.obj[i];
			int v, pat;

			if (SL >= obj->y || SL + 16 < obj->y)
				continue;
			if (SL + 8 >= obj->y && !(R_LCDC & 0x04))
				continue;

			lcd.VS[NS].x = (int)obj->x - 8;
			v = SL - (int)obj->y + 16;

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
			gb_vs_t ts[10];
			for (int i = 0; i < NS; ++i)
			{
				int l = 0, x = lcd.VS[0].x;
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
	}

	// bg tiles buffering
	{
		const int8_t wraptable[64] = {
			0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
			0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,-32
		};
		const int8_t *wrap = wraptable + lcd.S;

		int base = ((R_LCDC&0x08)?0x1C00:0x1800) + (lcd.T<<5) + lcd.S;
		byte *tilemap = lcd.vbank[0] + base;
		byte *attrmap = lcd.vbank[1] + base;
		int *tilebuf = lcd.BG;
		int cnt = ((WX + 7) >> 3) + 1;

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
	}

	// window tiles buffering
	if (WX < 160)
	{
		int base = ((R_LCDC&0x40)?0x1C00:0x1800) + (lcd.WT<<5);
		byte *tilemap = lcd.vbank[0] + base;
		byte *attrmap = lcd.vbank[1] + base;
		int *tilebuf = lcd.WND;
		int cnt = ((160 - WX) >> 3) + 1;

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

	if (hw.hwtype == GB_HW_CGB)
	{
		// bg scan color
		if (WX > 0)
		{
			int cnt = WX;
			int *tile = lcd.BG;
			byte *dest = lcd.BUF;
			byte *src = get_patpix(*(tile++), lcd.V) + lcd.U;
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

		// window scan color
		if (WX < 160)
		{
			int cnt = 160 - WX;
			int *tile = lcd.WND;
			byte *dest = lcd.BUF + WX;
			byte *src;

			while (cnt >= 0)
			{
				src = get_patpix(*(tile++), lcd.WV);
				blendcpy(dest, src, *(tile++), 8);
				dest += 8;
				cnt -= 8;
			}
		}

		// bg priority rescan
		if (NS && WX > 0)
		{
			int cnt = WX;
			int i = lcd.S;
			byte *dest = lcd.PRI;
			byte *src = lcd.vbank[1] + ((R_LCDC&0x08)?0x1C00:0x1800) + (lcd.T<<5);

			if (priused(src))
			{
				memset(dest, src[i++&31]&128, 8-lcd.U);
				dest += 8-lcd.U;
				cnt -= 8-lcd.U;

				while (cnt >= 8)
				{
					memset(dest, src[i++&31]&128, 8);
					dest += 8;
					cnt -= 8;
				}
				if (cnt > 0)
					memset(dest, src[i&31]&128, cnt);
			}
			else
			{
				memset(dest, 0, cnt);
			}
		}

		// window priority rescan
		if (NS && WX < 160)
		{
			int cnt = 160 - WX;
			int i = 0;
			byte *dest = lcd.PRI + WX;
			byte *src = lcd.vbank[1] + ((R_LCDC&0x40)?0x1C00:0x1800) + (lcd.WT<<5);

			if (priused(src))
			{
				while (cnt >= 8)
				{
					memset(dest, src[i++]&128, 8);
					dest += 8;
					cnt -= 8;
				}
				if (cnt > 0)
					memset(dest, src[i]&128, cnt);
			}
			else
			{
				memset(dest, 0, cnt);
			}
		}
	}
	else
	{
		// bg scan
		if (WX > 0)
		{
			int cnt = WX;
			int *tile = lcd.BG;
			byte *dest = lcd.BUF;
			byte *src = get_patpix(*(tile++), lcd.V) + lcd.U;

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

		// window scan
		if (WX < 160)
		{
			int cnt = 160 - WX;
			int *tile = lcd.WND;
			byte *dest = lcd.BUF + WX;
			byte *src;

			while (cnt >= 0)
			{
				src = get_patpix(*(tile++), lcd.WV);
				memcpy(dest, src, 8);
				dest += 8;
				cnt -= 8;
			}
		}
		blendcpy(lcd.BUF+WX, lcd.BUF+WX, 0x04, 160-WX);
	}

	// Sprites scan
	byte bgdup[256];

	memcpy(bgdup, lcd.BUF, 256);

	for (int ns = NS; ns > 0; --ns)
	{
		gb_vs_t *vs = &lcd.VS[ns-1];
		int pal = vs->pal;
		int x = vs->x;
		int i, b;

		if (x >= 160 || x <= -8)
			continue;

		byte *src = get_patpix(vs->pat, vs->v);
		byte *dest = lcd.BUF;

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
			byte *bg = bgdup + (dest - lcd.BUF);
			while (i--)
			{
				b = src[i];
				if (b && !(bg[i]&3)) dest[i] = pal|b;
			}
		}
		else if (hw.hwtype == GB_HW_CGB)
		{
			byte *bg = bgdup + (dest - lcd.BUF);
			byte *pri = lcd.PRI + (dest - lcd.BUF);
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

static void lcd_emulate(int cycles)
{
	lcd.cycles -= cycles;

	/* LCDC operation disabled (short route) */
	while (lcd.cycles <= 0 && !(R_LCDC & 0x80))
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
				hw_hdma(-1);
			else
				lcd.cycles += 102;
			break;
		}
	}

	// Normal LCD emulation
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
			if (!(hw.interrupts & IF_VBLANK))
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
				/* Handling special case on the last line; see docs/HACKING */
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
				hw_hdma(-1);
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

static void sound_dirty()
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


/******************* BEGIN CONSOLE *******************/

/*
 * hw_interrupt changes the virtual interrupt line(s) defined by i
 * The interrupt fires (added to R_IF) when the line transitions from 0 to 1.
 * It does not refire if the line was already high.
 */
static void hw_interrupt(byte i, int level)
{
	if (level == 0)
	{
		hw.interrupts &= ~i;
	}
	else if ((hw.interrupts & i) == 0)
	{
		hw.interrupts |= i;
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
static void hw_updatemap(void)
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
			if ((cart.rtc.latch ^ b) & b & 1)
			{
				cart.rtc.regs[0] = cart.rtc.s;
				cart.rtc.regs[1] = cart.rtc.m;
				cart.rtc.regs[2] = cart.rtc.h;
				cart.rtc.regs[3] = cart.rtc.d;
				cart.rtc.regs[4] = cart.rtc.flags;
			}
			cart.rtc.latch = b & 1;
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

static void hw_refresh(void)
{
	lcd_rebuildpal();
	sound_dirty();
	hw_updatemap();
}

static void hw_emulate(int cycles)
{
	if (hw.serial > 0)
	{
		hw.serial -= cycles << 1;
		if (hw.serial <= 0)
		{
			R_SB = 0xFF;
			R_SC &= 0x7f;
			hw.serial = 0;
			hw_interrupt(IF_SERIAL, 1);
			hw_interrupt(IF_SERIAL, 0);
		}
	}

	// static inline void timer_advance(int cycles)
	{
		hw.timer_div += (cycles << 2);

		R_DIV += (hw.timer_div >> 8);

		hw.timer_div &= 0xff;

		if (R_TAC & 0x04)
		{
			hw.timer += (cycles << ((((-R_TAC) & 3) << 1) + 1));

			if (hw.timer >= 512)
			{
				int tima = R_TIMA + (hw.timer >> 9);
				hw.timer &= 0x1ff;
				if (tima >= 256)
				{
					hw_interrupt(IF_TIMER, 1);
					hw_interrupt(IF_TIMER, 0);
					tima = R_TMA;
				}
				R_TIMA = tima;
			}
		}
	}

	if (!cpu.double_speed)
		cycles <<= 1;

	lcd_emulate(cycles);
	snd.cycles += cycles; // sound_emulate(cycles);
}

static byte hw_read(uint a)
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

static void hw_write(uint a, byte b)
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
				R_P1 = (b & 0x30) | 0xC0;
				if (!(R_P1 & 0x10)) R_P1 |= (hw.joypad & 0x0F);
				if (!(R_P1 & 0x20)) R_P1 |= (hw.joypad >> 4);
				R_P1 ^= 0x0F;
				// Skipping the interrupt doesn't seem to cause much issue
				// Not sure it should even trigger from here?
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
				for (size_t a = (size_t)b << 8, i = 0; i < 160; i++, a++)
					// just called hw_read(a) should be fine instead...
					lcd.oam.mem[i] = ({byte *p = hw.rmap[a>>12]; p ? p[a] : hw_read(a);});
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

static void hw_hdma(int c)
{
	uint src = (R_HDMA1 << 8) | (R_HDMA2 & 0xF0);
	uint dst = 0x8000 | ((R_HDMA3 & 0x1F) << 8) | (R_HDMA4 & 0xF0);
	uint cnt = 0;

	/* Continue transfer */
	if (c == -1)
	{
		cnt = 16;
		hw.hdma--;
		R_HDMA5--;
	}
	/* Queue or cancel HDMA */
	else if ((hw.hdma|c) & 0x80)
	{
		hw.hdma = c;
		R_HDMA5 = c & 0x7f;
		return;
	}
	/* Do the whole transfer now */
	else
	{
		cnt = (c + 1) << 4;
		R_HDMA5 = 0xFF;
	}

	// if (!(hw.hdma & 0x80))
	// 	return;

	/* FIXME - this should use cpu time! */
	while (cnt--)
	{
		uint d = dst++, s = src++;
		byte *sp = hw.rmap[d>>12];
		byte *dp = hw.wmap[s>>12];
		uint b = sp ? sp[s] : hw_read(s);
		if (dp) dp[d] = b; else hw_write(d, b);
	}

	R_HDMA1 = src >> 8;
	R_HDMA2 = src & 0xF0;
	R_HDMA3 = (dst >> 8) & 0x1F;
	R_HDMA4 = dst & 0xF0;
}

static inline byte readb(uint a)
{
	byte *p = hw.rmap[a>>12];
	return p ? p[a] : hw_read(a);
}

static inline void writeb(uint a, byte b)
{
	byte *p = hw.wmap[a>>12];
	if (p) p[a] = b;
	else hw_write(a, b);
}

static inline un16 readw(uint a)
{
	byte *p = hw.rmap[a >> 12];
	if (!p || (a & 1))
	{
		return readb(a) | (readb(a + 1) << 8);
	}
	return *(un16 *)(p + a);
}

static inline void writew(uint a, un16 w)
{
	byte *p = hw.wmap[a >> 12];
	if (!p || (a & 1))
	{
		writeb(a, w);
		writeb(a + 1, w >> 8);
		return;
	}
	*(un16 *)(p + a) = w;
}

/******************* END CONSOLE *******************/


/******************* BEGIN CPU *******************/

// For cycle accurate emulation this needs to be 1
// Anything above 10 have diminishing returns
#define COUNTERS_TICK_PERIOD 8

#define ZFLAG(n) ( (n) ? 0 : FZ )

#define PUSH(w) ( (SP -= 2), (writew(SP, (w))) )
#define POP(w)  ( ((w) = readw(SP)), (SP += 2) )

#define FETCH (readb(PC++))

#define INC(r) { ((r)++); F = (F & (FL|FC)) | incflag_table[(r)]; }
#define DEC(r) { ((r)--); F = (F & (FL|FC)) | decflag_table[(r)]; }

#define INCW(r) ( (r)++ )
#define DECW(r) ( (r)-- )

#define ADD(n) { \
W(acc) = (un16)A + (un16)(n); \
F = (ZFLAG(LB(acc))) | (FH & ((A ^ (n) ^ LB(acc)) << 1)) | (HB(acc) << 4); \
A = LB(acc); }

#define ADC(n) { \
W(acc) = (un16)A + (un16)(n) + (un16)((F&FC)>>4); \
F = (ZFLAG(LB(acc))) | (FH & ((A ^ (n) ^ LB(acc)) << 1)) | (HB(acc) << 4); \
A = LB(acc); }

#define ADDW(n) { \
temp = (un32)HL + (un32)(n); \
F &= FZ; \
if (0xFFFF - (n) < HL) F |= FC; \
if ((HL & 0xFFF) + ((n) & 0xFFF) > 0xFFF) F |= FH; \
HL = temp; }

#define CP(n) { \
W(acc) = (un16)A - (un16)(n); \
F = FN \
| (ZFLAG(LB(acc))) \
| (FH & ((A ^ (n) ^ LB(acc)) << 1)) \
| ((un8)(-(n8)HB(acc)) << 4); }

#define SUB(n) { CP((n)); A = LB(acc); }

#define SBC(n) { \
W(acc) = (un16)A - (un16)(n) - (un16)((F&FC)>>4); \
F = FN \
| (ZFLAG((n8)LB(acc))) \
| (FH & ((A ^ (n) ^ LB(acc)) << 1)) \
| ((un8)(-(n8)HB(acc)) << 4); \
A = LB(acc); }

#define AND(n) { A &= (n); F = ZFLAG(A) | FH; }
#define XOR(n) { A ^= (n); F = ZFLAG(A); }
#define OR(n)  { A |= (n); F = ZFLAG(A); }

#define RLCA(r) { (r) = ((r)>>7) | ((r)<<1); F = (((r)&0x01)<<4); }
#define RRCA(r) { (r) = ((r)<<7) | ((r)>>1); F = (((r)&0x80)>>3); }
#define RLA(r) { LB(acc) = (((r)&0x80)>>3); (r) = ((r)<<1) | ((F&FC)>>4); F = LB(acc); }
#define RRA(r) { LB(acc) = (((r)&0x01)<<4); (r) = ((r)>>1) | ((F&FC)<<3); F = LB(acc); }

#define RLC(r) { RLCA(r); F |= ZFLAG(r); }
#define RRC(r) { RRCA(r); F |= ZFLAG(r); }
#define RL(r)  { RLA(r); F |= ZFLAG(r); }
#define RR(r)  { RRA(r); F |= ZFLAG(r); }

#define SLA(r) { LB(acc) = (((r)&0x80)>>3); (r) <<= 1; F = ZFLAG((r)) | LB(acc); }
#define SRA(r) { LB(acc) = (((r)&0x01)<<4); (r) = (un8)(((n8)(r))>>1); F = ZFLAG((r)) | LB(acc); }
#define SRL(r) { LB(acc) = (((r)&0x01)<<4); (r) >>= 1; F = ZFLAG((r)) | LB(acc); }

#define CPL(r) { (r) = ~(r); F |= (FH|FN); }

#define SCF { F = (F & (FZ)) | FC; }
#define CCF { F = (F & (FZ|FC)) ^ FC; }

#define SWAP(r) { (r) = swap_table[(r)]; F = ZFLAG((r)); }

#define BIT(n,r) { F = (F & FC) | ZFLAG(((r) & (1 << (n)))) | FH; }
#define RES(n,r) { (r) &= ~(1 << (n)); }
#define SET(n,r) { (r) |= (1 << (n)); }

#define JR ( PC += 1+(n8)readb(PC) )
#define JP ( PC = readw(PC) )

#define NOJR   ( clen--,  PC++ )
#define NOJP   ( clen--,  PC+=2 )
#define NOCALL ( clen-=3, PC+=2 )
#define NORET  ( clen-=3 )

#define RST(n) { PUSH(PC); PC = (n); }

#define CALL ( PUSH(PC+2), JP )
#define RET ( POP(PC) )

#define EI ( IMA = 1 )
#define DI ( IMA = IME = cpu.halted =  0 )

#define COND_EXEC_INT(i, n) if (temp & i) { DI; PUSH(PC); R_IF &= ~i; PC = 0x40+((n)<<3); clen = 5; goto _skip; }

#define CB_REG_CASES(r, n) \
case 0x00|(n): RLC(r); break; \
case 0x08|(n): RRC(r); break; \
case 0x10|(n): RL(r); break; \
case 0x18|(n): RR(r); break; \
case 0x20|(n): SLA(r); break; \
case 0x28|(n): SRA(r); break; \
case 0x30|(n): SWAP(r); break; \
case 0x38|(n): SRL(r); break; \
case 0x40|(n): BIT(0, r); break; \
case 0x48|(n): BIT(1, r); break; \
case 0x50|(n): BIT(2, r); break; \
case 0x58|(n): BIT(3, r); break; \
case 0x60|(n): BIT(4, r); break; \
case 0x68|(n): BIT(5, r); break; \
case 0x70|(n): BIT(6, r); break; \
case 0x78|(n): BIT(7, r); break; \
case 0x80|(n): RES(0, r); break; \
case 0x88|(n): RES(1, r); break; \
case 0x90|(n): RES(2, r); break; \
case 0x98|(n): RES(3, r); break; \
case 0xA0|(n): RES(4, r); break; \
case 0xA8|(n): RES(5, r); break; \
case 0xB0|(n): RES(6, r); break; \
case 0xB8|(n): RES(7, r); break; \
case 0xC0|(n): SET(0, r); break; \
case 0xC8|(n): SET(1, r); break; \
case 0xD0|(n): SET(2, r); break; \
case 0xD8|(n): SET(3, r); break; \
case 0xE0|(n): SET(4, r); break; \
case 0xE8|(n): SET(5, r); break; \
case 0xF0|(n): SET(6, r); break; \
case 0xF8|(n): SET(7, r); break;


#define ALU_CASES(base, imm, op, label) \
case (imm): b = FETCH; goto label; \
case (base): b = B; goto label; \
case (base)+1: b = C; goto label; \
case (base)+2: b = D; goto label; \
case (base)+3: b = E; goto label; \
case (base)+4: b = H; goto label; \
case (base)+5: b = L; goto label; \
case (base)+6: b = readb(HL); goto label; \
case (base)+7: b = A; \
label: op(b); break;


static void cpu_reset(bool hard)
{
	cpu.double_speed = 0;
	cpu.halted = 0;
	IME = 0;
	IMA = 0;
	PC = hw.bios ? 0x0000 : 0x0100;
	SP = 0xFFFE;
	AF = (hw.hwtype == GB_HW_CGB) ? 0x11B0 : 0x01B0;
	BC = 0x0013;
	DE = 0x00D8;
	HL = 0x014D;
}

/* cpu_emulate()
	Emulate CPU for time no less than specified

	cycles - time to emulate, expressed in double-speed cycles
	returns number of cycles emulated

	Might emulate up to cycles+(11) time units (longest op takes 12
	cycles in single-speed mode)
*/
static IRAM_ATTR int cpu_emulate(int cycles)
{
	int clen, temp;
	int remaining = cycles;
	int count = 0;
	byte op, b;
	cpu_reg_t acc;

	if (!cpu.double_speed)
		remaining >>= 1;

next:
	/* Skip idle cycles */
	if (cpu.halted) {
		clen = 1;
		goto _skip;
	}

	/* Handle interrupts */
	if (IME && (temp = R_IF & R_IE))
	{
		COND_EXEC_INT(IF_VBLANK, 0);
		COND_EXEC_INT(IF_STAT, 1);
		COND_EXEC_INT(IF_TIMER, 2);
		COND_EXEC_INT(IF_SERIAL, 3);
		COND_EXEC_INT(IF_PAD, 4);
	}
	IME = IMA;

	// if (cpu.disassemble)
	// 	debug_disassemble(PC, 1);

	op = FETCH;
	clen = cycles_table[op];

	switch(op)
	{
	case 0x00: /* NOP */
	case 0x40: /* LD B,B */
	case 0x49: /* LD C,C */
	case 0x52: /* LD D,D */
	case 0x5B: /* LD E,E */
	case 0x64: /* LD H,H */
	case 0x6D: /* LD L,L */
	case 0x7F: /* LD A,A */
		break;

	case 0x41: /* LD B,C */
		B = C; break;
	case 0x42: /* LD B,D */
		B = D; break;
	case 0x43: /* LD B,E */
		B = E; break;
	case 0x44: /* LD B,H */
		B = H; break;
	case 0x45: /* LD B,L */
		B = L; break;
	case 0x46: /* LD B,(HL) */
		B = readb(HL); break;
	case 0x47: /* LD B,A */
		B = A; break;

	case 0x48: /* LD C,B */
		C = B; break;
	case 0x4A: /* LD C,D */
		C = D; break;
	case 0x4B: /* LD C,E */
		C = E; break;
	case 0x4C: /* LD C,H */
		C = H; break;
	case 0x4D: /* LD C,L */
		C = L; break;
	case 0x4E: /* LD C,(HL) */
		C = readb(HL); break;
	case 0x4F: /* LD C,A */
		C = A; break;

	case 0x50: /* LD D,B */
		D = B; break;
	case 0x51: /* LD D,C */
		D = C; break;
	case 0x53: /* LD D,E */
		D = E; break;
	case 0x54: /* LD D,H */
		D = H; break;
	case 0x55: /* LD D,L */
		D = L; break;
	case 0x56: /* LD D,(HL) */
		D = readb(HL); break;
	case 0x57: /* LD D,A */
		D = A; break;

	case 0x58: /* LD E,B */
		E = B; break;
	case 0x59: /* LD E,C */
		E = C; break;
	case 0x5A: /* LD E,D */
		E = D; break;
	case 0x5C: /* LD E,H */
		E = H; break;
	case 0x5D: /* LD E,L */
		E = L; break;
	case 0x5E: /* LD E,(HL) */
		E = readb(HL); break;
	case 0x5F: /* LD E,A */
		E = A; break;

	case 0x60: /* LD H,B */
		H = B; break;
	case 0x61: /* LD H,C */
		H = C; break;
	case 0x62: /* LD H,D */
		H = D; break;
	case 0x63: /* LD H,E */
		H = E; break;
	case 0x65: /* LD H,L */
		H = L; break;
	case 0x66: /* LD H,(HL) */
		H = readb(HL); break;
	case 0x67: /* LD H,A */
		H = A; break;

	case 0x68: /* LD L,B */
		L = B; break;
	case 0x69: /* LD L,C */
		L = C; break;
	case 0x6A: /* LD L,D */
		L = D; break;
	case 0x6B: /* LD L,E */
		L = E; break;
	case 0x6C: /* LD L,H */
		L = H; break;
	case 0x6E: /* LD L,(HL) */
		L = readb(HL); break;
	case 0x6F: /* LD L,A */
		L = A; break;

	case 0x70: /* LD (HL),B */
		writeb(HL, B); break;
	case 0x71: /* LD (HL),C */
		writeb(HL, C); break;
	case 0x72: /* LD (HL),D */
		writeb(HL, D); break;
	case 0x73: /* LD (HL),E */
		writeb(HL, E); break;
	case 0x74: /* LD (HL),H */
		writeb(HL, H); break;
	case 0x75: /* LD (HL),L */
		writeb(HL, L); break;
	case 0x77: /* LD (HL),A */
		writeb(HL, A); break;

	case 0x78: /* LD A,B */
		A = B; break;
	case 0x79: /* LD A,C */
		A = C; break;
	case 0x7A: /* LD A,D */
		A = D; break;
	case 0x7B: /* LD A,E */
		A = E; break;
	case 0x7C: /* LD A,H */
		A = H; break;
	case 0x7D: /* LD A,L */
		A = L; break;
	case 0x7E: /* LD A,(HL) */
		A = readb(HL); break;

	case 0x01: /* LD BC,imm */
		BC = readw(PC); PC += 2; break;
	case 0x11: /* LD DE,imm */
		DE = readw(PC); PC += 2; break;
	case 0x21: /* LD HL,imm */
		HL = readw(PC); PC += 2; break;
	case 0x31: /* LD SP,imm */
		SP = readw(PC); PC += 2; break;

	case 0x02: /* LD (BC),A */
		writeb(BC, A); break;
	case 0x0A: /* LD A,(BC) */
		A = readb(BC); break;
	case 0x12: /* LD (DE),A */
		writeb(DE, A); break;
	case 0x1A: /* LD A,(DE) */
		A = readb(DE); break;

	case 0x22: /* LDI (HL),A */
		writeb(HL, A); HL++; break;
	case 0x2A: /* LDI A,(HL) */
		A = readb(HL); HL++; break;
	case 0x32: /* LDD (HL),A */
		writeb(HL, A); HL--; break;
	case 0x3A: /* LDD A,(HL) */
		A = readb(HL); HL--; break;

	case 0x06: /* LD B,imm */
		B = FETCH; break;
	case 0x0E: /* LD C,imm */
		C = FETCH; break;
	case 0x16: /* LD D,imm */
		D = FETCH; break;
	case 0x1E: /* LD E,imm */
		E = FETCH; break;
	case 0x26: /* LD H,imm */
		H = FETCH; break;
	case 0x2E: /* LD L,imm */
		L = FETCH; break;
	case 0x36: /* LD (HL),imm */
		writeb(HL, FETCH); break;
	case 0x3E: /* LD A,imm */
		A = FETCH; break;

	case 0x08: /* LD (imm),SP */
		writew(readw(PC), SP); PC += 2; break;
	case 0xEA: /* LD (imm),A */
		writeb(readw(PC), A); PC += 2; break;

	case 0xE0: /* LDH (imm),A */
		writeb(0xff00 + FETCH, A); break;
	case 0xE2: /* LDH (C),A */
		writeb(0xff00 + C, A); break;
	case 0xF0: /* LDH A,(imm) */
		A = readb(0xff00 + FETCH); break;
	case 0xF2: /* LDH A,(C) (undocumented) */
		A = readb(0xff00 + C); break;

	case 0xF8: /* LD HL,SP+imm */
		// https://gammpei.github.io/blog/posts/2018-03-04/how-to-write-a-game-boy-emulator-part-8-blarggs-cpu-test-roms-1-3-4-5-7-8-9-10-11.html
		b = FETCH;
		temp = (int)(SP) + (signed char)b;

		F &= ~(FZ | FN | FH | FC);

		if (((SP & 0xff) ^ b ^ temp) & 0x10) F |= FH;
		if ((SP & 0xff) + b > 0xff) F |= FC;

		HL = temp;
		break;
	case 0xF9: /* LD SP,HL */
		SP = HL; break;
	case 0xFA: /* LD A,(imm) */
		A = readb(readw(PC)); PC += 2; break;

		ALU_CASES(0x80, 0xC6, ADD, __ADD)
		ALU_CASES(0x88, 0xCE, ADC, __ADC)
		ALU_CASES(0x90, 0xD6, SUB, __SUB)
		ALU_CASES(0x98, 0xDE, SBC, __SBC)
		ALU_CASES(0xA0, 0xE6, AND, __AND)
		ALU_CASES(0xA8, 0xEE, XOR, __XOR)
		ALU_CASES(0xB0, 0xF6, OR, __OR)
		ALU_CASES(0xB8, 0xFE, CP, __CP)

	case 0x09: /* ADD HL,BC */
		ADDW(BC); break;
	case 0x19: /* ADD HL,DE */
		ADDW(DE); break;
	case 0x39: /* ADD HL,SP */
		ADDW(SP); break;
	case 0x29: /* ADD HL,HL */
		ADDW(HL); break;

	case 0x04: /* INC B */
		INC(B); break;
	case 0x0C: /* INC C */
		INC(C); break;
	case 0x14: /* INC D */
		INC(D); break;
	case 0x1C: /* INC E */
		INC(E); break;
	case 0x24: /* INC H */
		INC(H); break;
	case 0x2C: /* INC L */
		INC(L); break;
	case 0x34: /* INC (HL) */
		b = readb(HL);
		INC(b);
		writeb(HL, b);
		break;
	case 0x3C: /* INC A */
		INC(A); break;

	case 0x03: /* INC BC */
		INCW(BC); break;
	case 0x13: /* INC DE */
		INCW(DE); break;
	case 0x23: /* INC HL */
		INCW(HL); break;
	case 0x33: /* INC SP */
		INCW(SP); break;

	case 0x05: /* DEC B */
		DEC(B); break;
	case 0x0D: /* DEC C */
		DEC(C); break;
	case 0x15: /* DEC D */
		DEC(D); break;
	case 0x1D: /* DEC E */
		DEC(E); break;
	case 0x25: /* DEC H */
		DEC(H); break;
	case 0x2D: /* DEC L */
		DEC(L); break;
	case 0x35: /* DEC (HL) */
		b = readb(HL);
		DEC(b);
		writeb(HL, b);
		break;
	case 0x3D: /* DEC A */
		DEC(A); break;

	case 0x0B: /* DEC BC */
		DECW(BC); break;
	case 0x1B: /* DEC DE */
		DECW(DE); break;
	case 0x2B: /* DEC HL */
		DECW(HL); break;
	case 0x3B: /* DEC SP */
		DECW(SP); break;

	case 0x07: /* RLCA */
		RLCA(A); break;
	case 0x0F: /* RRCA */
		RRCA(A); break;
	case 0x17: /* RLA */
		RLA(A); break;
	case 0x1F: /* RRA */
		RRA(A); break;

	case 0x27: /* DAA */
		//http://forums.nesdev.com/viewtopic.php?t=9088
		temp = A;

		if (F & FN)
		{
			if (F & FH)	temp = (temp - 6) & 0xff;
			if (F & FC) temp -= 0x60;
		}
		else
		{
			if ((F & FH) || ((temp & 0x0f) > 9)) temp += 0x06;
			if ((F & FC) || (temp > 0x9f)) temp += 0x60;
		}

		A = (byte)temp;
		F &= ~(FH | FZ);

		if (temp & 0x100)   F |= FC;
		if (!(temp & 0xff)) F |= FZ;
		break;

	case 0x2F: /* CPL */
		CPL(A); break;

	case 0x18: /* JR */
		JR; break;
	case 0x20: /* JR NZ */
		if (!(F&FZ)) JR; else NOJR; break;
	case 0x28: /* JR Z */
		if (F&FZ) JR; else NOJR; break;
	case 0x30: /* JR NC */
		if (!(F&FC)) JR; else NOJR; break;
	case 0x38: /* JR C */
		if (F&FC) JR; else NOJR; break;

	case 0xC3: /* JP */
		JP; break;
	case 0xC2: /* JP NZ */
		if (!(F&FZ)) JP; else NOJP; break;
	case 0xCA: /* JP Z */
		if (F&FZ) JP; else NOJP; break;
	case 0xD2: /* JP NC */
		if (!(F&FC)) JP; else NOJP; break;
	case 0xDA: /* JP C */
		if (F&FC) JP; else NOJP; break;
	case 0xE9: /* JP HL */
		PC = HL; break;

	case 0xC9: /* RET */
		RET; break;
	case 0xC0: /* RET NZ */
		if (!(F&FZ)) RET; else NORET; break;
	case 0xC8: /* RET Z */
		if (F&FZ) RET; else NORET; break;
	case 0xD0: /* RET NC */
		if (!(F&FC)) RET; else NORET; break;
	case 0xD8: /* RET C */
		if (F&FC) RET; else NORET; break;
	case 0xD9: /* RETI */
		IME = IMA = 1; RET; break;

	case 0xCD: /* CALL */
		CALL; break;
	case 0xC4: /* CALL NZ */
		if (!(F&FZ)) CALL; else NOCALL; break;
	case 0xCC: /* CALL Z */
		if (F&FZ) CALL; else NOCALL; break;
	case 0xD4: /* CALL NC */
		if (!(F&FC)) CALL; else NOCALL; break;
	case 0xDC: /* CALL C */
		if (F&FC) CALL; else NOCALL; break;

	case 0xC7: /* RST 0 */
		RST(0x00); break;
	case 0xCF: /* RST 8 */
		RST(0x08); break;
	case 0xD7: /* RST 10 */
		RST(0x10); break;
	case 0xDF: /* RST 18 */
		RST(0x18); break;
	case 0xE7: /* RST 20 */
		RST(0x20); break;
	case 0xEF: /* RST 28 */
		RST(0x28); break;
	case 0xF7: /* RST 30 */
		RST(0x30); break;
	case 0xFF: /* RST 38 */
		RST(0x38); break;

	case 0xC1: /* POP BC */
		POP(BC); break;
	case 0xC5: /* PUSH BC */
		PUSH(BC); break;
	case 0xD1: /* POP DE */
		POP(DE); break;
	case 0xD5: /* PUSH DE */
		PUSH(DE); break;
	case 0xE1: /* POP HL */
		POP(HL); break;
	case 0xE5: /* PUSH HL */
		PUSH(HL); break;
	case 0xF1: /* POP AF */
		POP(AF); AF &= 0xfff0; break;
	case 0xF5: /* PUSH AF */
		PUSH(AF); break;

	case 0xE8: /* ADD SP,imm */
		// https://gammpei.github.io/blog/posts/2018-03-04/how-to-write-a-game-boy-emulator-part-8-blarggs-cpu-test-roms-1-3-4-5-7-8-9-10-11.html
		b = FETCH; // ADDSP(b); break;
		temp = SP + (signed char)b;

		F &= ~(FZ | FN | FH | FC);

		if ((SP ^ b ^ temp) & 0x10) F |= FH;
		if ((SP & 0xff) + b > 0xff) F |= FC;

		SP = temp;
		break;

	case 0xF3: /* DI */
		DI; break;
	case 0xFB: /* EI */
		EI; break;

	case 0x37: /* SCF */
		SCF; break;
	case 0x3F: /* CCF */
		CCF; break;

	case 0x10: /* STOP */
		PC++;
		if (R_KEY1 & 1)
		{
			cpu.double_speed ^= 1;
			R_KEY1 = (R_KEY1 & 0x7E) | ((cpu.double_speed & 1) << 7);
			break;
		}
		/* NOTE - we do not implement dmg STOP whatsoever */
		break;

	case 0x76: /* HALT */
		cpu.halted = 1;
		if (!IME)
		{
			MESSAGE_DEBUG("FIX ME: HALT requested with IME = 0\n");
		}
		break;

	case 0xCB: /* CB prefix */
		op = FETCH;
		clen = cb_cycles_table[op];
		switch (op)
		{
			CB_REG_CASES(B, 0);
			CB_REG_CASES(C, 1);
			CB_REG_CASES(D, 2);
			CB_REG_CASES(E, 3);
			CB_REG_CASES(H, 4);
			CB_REG_CASES(L, 5);
			CB_REG_CASES(A, 7);
		default:
			b = readb(HL);
			switch (op)
			{
				CB_REG_CASES(b, 6);
			}
			if ((op & 0xC0) != 0x40) /* exclude BIT */
				writeb(HL, b);
			break;
		}
		break;

	default:
		MESSAGE_ERROR("invalid opcode 0x%02X at address 0x%04X, rombank = %d\n",
			op, (PC-1) & 0xffff, 0);
		break; // abort();
	}

_skip:

	remaining -= clen;
	count += clen;

#if COUNTERS_TICK_PERIOD > 1
	if (count >= COUNTERS_TICK_PERIOD || remaining <= 0)
#endif
	{
		hw_emulate(count);
		count = 0;
	}

	if (remaining > 0)
		goto next;

	return cycles + -remaining;
}

static void cpu_disassemble(uint a, int c)
{
	while (c > 0)
	{
		char operands[64];
		char mnemonic[64];
		byte ops[3];
		int j = 0, k = 0;
		int opaddr = a;
		int opcode = readb(a++);
		const char *pattern = mnemonic_table[opcode];

		ops[k++] = opcode;

		if (opcode == 0xCB)
		{
			opcode = ops[k++] = readb(a++);
			pattern = cb_mnemonic_table[opcode];
		}

		while (*pattern)
		{
			if (*pattern == '%')
			{
				pattern++;
				switch (*pattern)
				{
				case 'B':
				case 'b':
					ops[k] = readb(a++);
					j += sprintf(mnemonic + j, "%02Xh", ops[k++]);
					break;
				case 'W':
				case 'w':
					ops[k] = readb(a++);
					ops[k+1] = readb(a++);
					j += sprintf(mnemonic + j, "%04Xh", ((ops[k+1] << 8) | ops[k]));
					k += 2;
					break;
				case 'O':
				case 'o':
					ops[k] = readb(a++);
					j += sprintf(mnemonic + j, "%+d", (n8)(ops[k++]));
					break;
				}
			}
			else
			{
				mnemonic[j++] = *pattern++;
			}
		}
		mnemonic[j] = 0;

		switch (operand_count[ops[0]])
		{
		case 1:
			sprintf(operands, "%02X       ", ops[0]);
			break;
		case 2:
			sprintf(operands, "%02X %02X    ", ops[0], ops[1]);
			break;
		case 3:
			sprintf(operands, "%02X %02X %02X ", ops[0], ops[1], ops[2]);
			break;
		}
		printf(
			"%04X: %s %-16.16s"
			" SP=%04X.%04X BC=%04X.%02X.%02X DE=%04X.%02X"
			" HL=%04X.%02X A=%02X F=%02X %c%c%c%c%c"
			" IE=%02X IF=%02X LCDC=%02X STAT=%02X LY=%02X LYC=%02X"
			" \n",
			opaddr,
			operands,
			mnemonic,
			SP, readw(SP),
			BC, readb(BC), readb(0xFF00 | C),
			DE, readb(DE),
			HL, readb(HL), A,
			F, (IME ? 'I' : '-'),
			((F & 0x80) ? 'Z' : '-'),
			((F & 0x40) ? 'N' : '-'),
			((F & 0x20) ? 'H' : '-'),
			((F & 0x10) ? 'C' : '-'),
			R_IE, R_IF, R_LCDC, R_STAT, R_LY, R_LYC
		);
		c--;
	}
}

/******************* END CPU *******************/





int gnuboy_init(int samplerate, bool stereo, int pixformat, void *vblank_func)
{
	host.snd.samplerate = samplerate;
	host.snd.stereo = stereo;
	host.snd.buffer = malloc(samplerate / 4);
	host.snd.len = samplerate / 8;
	host.snd.pos = 0;
	host.lcd.format = pixformat;
	host.lcd.vblank = vblank_func;
	// gnuboy_reset(true);
	return 0;
}


/*
 * gnuboy_reset is called to initialize the state of the emulated
 * system. It should set cpu registers, hardware registers, etc. to
 * their appropriate values at power up time.
 */
void gnuboy_reset(bool hard)
{
	memset(hw.ioregs, 0, sizeof(hw.ioregs));
	memset(hw.rmap, 0, sizeof(hw.rmap));
	memset(hw.wmap, 0, sizeof(hw.wmap));
	hw.interrupts = 0;
	hw.joypad = 0;
	hw.serial = 0;
	hw.hdma = 0;
	hw.timer = 0;
	hw.timer_div = 0;
	cart.sram_dirty = 0;
	cart.bankmode = 0;
	cart.rombank = 1;
	cart.rambank = 0;
	cart.enableram = 0;

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

	hw_updatemap();

	lcd_reset(hard);
	sound_reset(hard);
	cpu_reset(hard);
}


/*
	Time intervals throughout the code, unless otherwise noted, are
	specified in double-speed machine cycles (2MHz), each unit
	roughly corresponds to 0.477us.

	For CPU each cycle takes 2dsc (0.954us) in single-speed mode
	and 1dsc (0.477us) in double speed mode.

	Although hardware gbc LCDC would operate at completely different
	and fixed frequency, for emulation purposes timings for it are
	also specified in double-speed cycles.

	line = 228 dsc (109us)
	frame (154 lines) = 35112 dsc (16.7ms)
	of which
		visible lines x144 = 32832 dsc (15.66ms)
		vblank lines x10 = 2280 dsc (1.08ms)
*/
void gnuboy_run(bool draw)
{
	host.lcd.enabled = draw;
	host.snd.pos = 0;

	/* FIXME: judging by the time specified this was intended
	to emulate through vblank phase which is handled at the
	end of the loop. */
	cpu_emulate(2280);

	/* Step through visible line scanning phase */
	while (R_LY > 0 && R_LY < 144)
		cpu_emulate(lcd.cycles);

	/* Vblank reached, do a callback */
	if (draw && host.lcd.vblank)
		(host.lcd.vblank)();

	/* Sync hardware */
	sound_emulate();

	/* rtc tick */
	if (!(cart.rtc.flags & 0x40))
	{
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

	/* LCDC operation stopped */
	if (!(R_LCDC & 0x80))
		cpu_emulate(32832);

	/* Step through vblank phase */
	while (R_LY > 0)
		cpu_emulate(lcd.cycles);
}


void gnuboy_set_pad(uint pad)
{
	if (hw.joypad != pad)
	{
		hw.joypad = pad & 0xFF;
		hw_interrupt(IF_PAD, 1);
		hw_interrupt(IF_PAD, 0);
	}
}


int gnuboy_load_bios(const char *file)
{
	MESSAGE_INFO("Loading BIOS file: '%s'\n", file);

	if (!(hw.bios = malloc(0x900)))
	{
		MESSAGE_ERROR("Mem alloc failed.\n");
		return -2;
	}

	FILE *fp = fopen(file, "rb");
	if (!fp || fread(hw.bios, 1, 0x900, fp) < 0x100)
	{
		MESSAGE_ERROR("File read failed.\n");
		free(hw.bios);
		hw.bios = NULL;
	}
	fclose(fp);

	return hw.bios ? 0 : -1;
}


void gnuboy_load_bank(int bank)
{
	const size_t BANK_SIZE = 0x4000;
	const size_t OFFSET = bank * BANK_SIZE;

	if (!cart.rombanks[bank])
		cart.rombanks[bank] = malloc(BANK_SIZE);

	if (!cart.romFile)
		return;

	MESSAGE_INFO("loading bank %d.\n", bank);
	while (!cart.rombanks[bank])
	{
		int i = rand() & 0xFF;
		if (cart.rombanks[i])
		{
			MESSAGE_INFO("reclaiming bank %d.\n", i);
			cart.rombanks[bank] = cart.rombanks[i];
			cart.rombanks[i] = NULL;
			break;
		}
	}

	// Load the 16K page
	if (fseek(cart.romFile, OFFSET, SEEK_SET)
		|| !fread(cart.rombanks[bank], BANK_SIZE, 1, cart.romFile))
	{
		MESSAGE_WARN("ROM bank loading failed\n");
		if (!feof(cart.romFile))
			abort(); // This indicates an SD Card failure
	}
}


int gnuboy_load_rom(const char *file)
{
	// Memory Bank Controller names
	const char *mbc_names[16] = {
		"MBC_NONE", "MBC_MBC1", "MBC_MBC2", "MBC_MBC3",
		"MBC_MBC5", "MBC_MBC6", "MBC_MBC7", "MBC_HUC1",
		"MBC_HUC3", "MBC_MMM01", "INVALID", "INVALID",
		"INVALID", "INVALID", "INVALID", "INVALID",
	};
	const char *hw_types[] = {
		"DMG", "CGB", "SGB", "AGB", "???"
	};

	MESSAGE_INFO("Loading file: '%s'\n", file);

	byte header[0x200];

	cart.romFile = fopen(file, "rb");
	if (cart.romFile == NULL)
	{
		MESSAGE_ERROR("ROM fopen failed");
		return -1;
	}

	if (fread(&header, 0x200, 1, cart.romFile) != 1)
	{
		MESSAGE_ERROR("ROM fread failed");
		fclose(cart.romFile);
		return -1;
	}

	int type = header[0x0147];
	int romsize = header[0x0148];
	int ramsize = header[0x0149];

	if (header[0x0143] == 0x80 || header[0x0143] == 0xC0)
		hw.hwtype = GB_HW_CGB; // Game supports CGB mode so we go for that
	else if (header[0x0146] == 0x03)
		hw.hwtype = GB_HW_SGB; // Game supports SGB features
	else
		hw.hwtype = GB_HW_DMG; // Games supports DMG only

	memcpy(&cart.checksum, header + 0x014E, 2);
	memcpy(&cart.name, header + 0x0134, 16);
	cart.name[16] = 0;

	cart.has_battery = (type == 3 || type == 6 || type == 9 || type == 13 || type == 15 ||
						type == 16 || type == 19 || type == 27 || type == 30 || type == 255);
	cart.has_rtc  = (type == 15 || type == 16);
	cart.has_rumble = (type == 28 || type == 29 || type == 30);
	cart.has_sensor = (type == 34);
	cart.colorize = 0;

	if (type >= 1 && type <= 3)
		cart.mbc = MBC_MBC1;
	else if (type >= 5 && type <= 6)
		cart.mbc = MBC_MBC2;
	else if (type >= 11 && type <= 13)
		cart.mbc = MBC_MMM01;
	else if (type >= 15 && type <= 19)
		cart.mbc = MBC_MBC3;
	else if (type >= 25 && type <= 30)
		cart.mbc = MBC_MBC5;
	else if (type == 32)
		cart.mbc = MBC_MBC6;
	else if (type == 34)
		cart.mbc = MBC_MBC7;
	else if (type == 254)
		cart.mbc = MBC_HUC3;
	else if (type == 255)
		cart.mbc = MBC_HUC1;
	else
		cart.mbc = MBC_NONE;

	if (romsize < 9)
	{
		cart.romsize = (2 << romsize);
	}
	else if (romsize > 0x51 && romsize < 0x55)
	{
		cart.romsize = 128; // (2 << romsize) + 64;
	}
	else
	{
		MESSAGE_ERROR("Invalid ROM size: %d\n", romsize);
		return -2;
	}

	if (ramsize < 6)
	{
		const byte ramsize_table[] = {1, 1, 1, 4, 16, 8};
		cart.ramsize = ramsize_table[ramsize];
	}
	else
	{
		MESSAGE_ERROR("Invalid RAM size: %d\n", ramsize);
		cart.ramsize = 1;
	}

	cart.rambanks = malloc(8192 * cart.ramsize);
	if (!cart.rambanks)
	{
		MESSAGE_ERROR("SRAM alloc failed");
		return -3;
	}

	// Detect colorization palette that the real GBC would be using
	if (hw.hwtype != GB_HW_CGB)
	{
		//
		// The following algorithm was adapted from visualboyadvance-m at
		// https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/gb/GB.cpp
		//

		// Title checksums that are treated specially by the CGB boot ROM
		const uint8_t col_checksum[79] = {
			0x00, 0x88, 0x16, 0x36, 0xD1, 0xDB, 0xF2, 0x3C, 0x8C, 0x92, 0x3D, 0x5C,
			0x58, 0xC9, 0x3E, 0x70, 0x1D, 0x59, 0x69, 0x19, 0x35, 0xA8, 0x14, 0xAA,
			0x75, 0x95, 0x99, 0x34, 0x6F, 0x15, 0xFF, 0x97, 0x4B, 0x90, 0x17, 0x10,
			0x39, 0xF7, 0xF6, 0xA2, 0x49, 0x4E, 0x43, 0x68, 0xE0, 0x8B, 0xF0, 0xCE,
			0x0C, 0x29, 0xE8, 0xB7, 0x86, 0x9A, 0x52, 0x01, 0x9D, 0x71, 0x9C, 0xBD,
			0x5D, 0x6D, 0x67, 0x3F, 0x6B, 0xB3, 0x46, 0x28, 0xA5, 0xC6, 0xD3, 0x27,
			0x61, 0x18, 0x66, 0x6A, 0xBF, 0x0D, 0xF4
		};

		// The fourth character of the game title for disambiguation on collision.
		const uint8_t col_disambig_chars[29] = {
			'B', 'E', 'F', 'A', 'A', 'R', 'B', 'E',
			'K', 'E', 'K', ' ', 'R', '-', 'U', 'R',
			'A', 'R', ' ', 'I', 'N', 'A', 'I', 'L',
			'I', 'C', 'E', ' ', 'R'
		};

		// Palette ID | (Flags << 5)
		const uint8_t col_palette_info[94] = {
			0x7C, 0x08, 0x12, 0xA3, 0xA2, 0x07, 0x87, 0x4B, 0x20, 0x12, 0x65, 0xA8,
			0x16, 0xA9, 0x86, 0xB1, 0x68, 0xA0, 0x87, 0x66, 0x12, 0xA1, 0x30, 0x3C,
			0x12, 0x85, 0x12, 0x64, 0x1B, 0x07, 0x06, 0x6F, 0x6E, 0x6E, 0xAE, 0xAF,
			0x6F, 0xB2, 0xAF, 0xB2, 0xA8, 0xAB, 0x6F, 0xAF, 0x86, 0xAE, 0xA2, 0xA2,
			0x12, 0xAF, 0x13, 0x12, 0xA1, 0x6E, 0xAF, 0xAF, 0xAD, 0x06, 0x4C, 0x6E,
			0xAF, 0xAF, 0x12, 0x7C, 0xAC, 0xA8, 0x6A, 0x6E, 0x13, 0xA0, 0x2D, 0xA8,
			0x2B, 0xAC, 0x64, 0xAC, 0x6D, 0x87, 0xBC, 0x60, 0xB4, 0x13, 0x72, 0x7C,
			0xB5, 0xAE, 0xAE, 0x7C, 0x7C, 0x65, 0xA2, 0x6C, 0x64, 0x85
		};

		uint8_t infoIdx = 0;
		uint8_t checksum = 0;

		// Calculate the checksum over 16 title bytes.
		for (int i = 0; i < 16; i++)
		{
			checksum += header[0x0134 + i];
		}

		// Check if the checksum is in the list.
		for (size_t idx = 0; idx < 79; idx++)
		{
			if (col_checksum[idx] == checksum)
			{
				infoIdx = idx;

				// Indexes above 0x40 have to be disambiguated.
				if (idx <= 0x40)
					break;

				// No idea how that works. But it works.
				for (size_t i = idx - 0x41, j = 0; i < 29; i += 14, j += 14) {
					if (header[0x0137] == col_disambig_chars[i]) {
						infoIdx += j;
						break;
					}
				}
				break;
			}
		}

		cart.colorize = col_palette_info[infoIdx];
	}

	MESSAGE_INFO("Cart loaded: name='%s', hw=%s, mbc=%s, romsize=%dK, ramsize=%dK, colorize=%d\n",
		cart.name, hw_types[hw.hwtype], mbc_names[cart.mbc], cart.romsize * 16, cart.ramsize * 8, cart.colorize);

	// Gameboy color games can be very large so we only load 1024K for faster boot
	// Also 4/8MB games do not fully fit, our bank manager takes care of swapping.

	int preload = cart.romsize < 64 ? cart.romsize : 64;

	if (cart.romsize > 64 && (strncmp(cart.name, "RAYMAN", 6) == 0 || strncmp(cart.name, "NONAME", 6) == 0))
	{
		MESSAGE_INFO("Special preloading for Rayman 1/2\n");
		preload = cart.romsize - 40;
	}

	MESSAGE_INFO("Preloading the first %d banks\n", preload);
	for (int i = 0; i < preload; i++)
	{
		gnuboy_load_bank(i);
	}

	// Apply game-specific hacks
	if (strncmp(cart.name, "SIREN GB2 ", 11) == 0 || strncmp(cart.name, "DONKEY KONG", 16) == 0)
	{
		MESSAGE_INFO("HACK: Window offset hack enabled\n");
		lcd.enable_window_offset_hack = 1;
	}

	return 0;
}


void gnuboy_free_rom(void)
{
	for (int i = 0; i < 512; i++)
	{
		if (cart.rombanks[i]) {
			free(cart.rombanks[i]);
			cart.rombanks[i] = NULL;
		}
	}
	free(cart.rambanks);
	cart.rambanks = NULL;

	if (cart.romFile)
	{
		fclose(cart.romFile);
		cart.romFile = NULL;
	}

	if (cart.sramFile)
	{
		fclose(cart.sramFile);
		cart.sramFile = NULL;
	}

	memset(&cart, 0, sizeof(cart));
}


void gnuboy_get_time(int *day, int *hour, int *minute, int *second)
{
	if (day) *day = cart.rtc.d;
	if (hour) *hour = cart.rtc.h;
	if (minute) *minute = cart.rtc.m;
	if (second) *second = cart.rtc.s;
}


void gnuboy_set_time(int day, int hour, int minute, int second)
{
	cart.rtc.d = MIN(MAX(day, 0), 365);
	cart.rtc.h = MIN(MAX(hour, 0), 24);
	cart.rtc.m = MIN(MAX(minute, 0), 60);
	cart.rtc.s = MIN(MAX(second, 0), 60);
	cart.rtc.ticks = 0;
}


int gnuboy_get_hwtype(void)
{
	return hw.hwtype;
}


void gnuboy_set_hwtype(gb_hwtype_t type)
{
	// nothing for now
}


int gnuboy_get_palette(void)
{
	return host.lcd.colorize;
}


void gnuboy_set_palette(gb_palette_t pal)
{
	if (host.lcd.colorize != pal)
	{
		host.lcd.colorize = pal;
		hw_refresh();
	}
}


/**
 * Save state file format is:
 * GB:
 * 0x0000 - 0x0BFF: svars
 * 0x0CF0 - 0x0CFF: snd.wave
 * 0x0D00 - 0x0DFF: hw.ioregs
 * 0x0E00 - 0x0E80: lcd.pal
 * 0x0F00 - 0x0FFF: lcd.oam
 * 0x1000 - 0x2FFF: RAM
 * 0x3000 - 0x4FFF: VRAM
 * 0x5000 - 0x...:  SRAM
 *
 * GBC:
 * 0x0000 - 0x0BFF: svars
 * 0x0CF0 - 0x0CFF: snd.wave
 * 0x0D00 - 0x0DFF: hw.ioregs
 * 0x0E00 - 0x0EFF: lcd.pal
 * 0x0F00 - 0x0FFF: lcd.oam
 * 0x1000 - 0x8FFF: RAM
 * 0x9000 - 0xCFFF: VRAM
 * 0xD000 - 0x...:  SRAM
 *
 */

#ifndef IS_BIG_ENDIAN
#define LIL(x) (x)
#else
#define LIL(x) ((x<<24)|((x&0xff00)<<8)|((x>>8)&0xff00)|(x>>24))
#endif

#define SAVE_VERSION 0x107

#define I1(s, p) { 1, s, p }
#define I2(s, p) { 2, s, p }
#define I4(s, p) { 4, s, p }
#define END { 0, "\0\0\0\0", 0 }

typedef struct
{
	size_t len;
	char key[4];
	void *ptr;
} svar_t;

typedef struct
{
	void *ptr;
	size_t len;
} sblock_t;

// This is the format used by VBA, maybe others
typedef struct
{
	uint32_t s, m, h, d, flags;
	uint32_t regs[5];
	uint64_t rt;
} srtc_t;

static un32 ver;

static const svar_t svars[] =
{
	I4("GbSs", &ver),

	I2("PC  ", &PC),
	I2("SP  ", &SP),
	I2("BC  ", &BC),
	I2("DE  ", &DE),
	I2("HL  ", &HL),
	I2("AF  ", &AF),

	I4("IME ", &cpu.ime),
	I4("ima ", &cpu.ima),
	I4("spd ", &cpu.double_speed),
	I4("halt", &cpu.halted),
	I4("div ", &hw.timer_div),
	I4("tim ", &hw.timer),
	I4("lcdc", &lcd.cycles),
	I4("snd ", &snd.cycles),

	I4("ints", &hw.interrupts),
	I4("pad ", &hw.joypad),
	I4("hdma", &hw.hdma),
	I4("seri", &hw.serial),

	I4("mbcm", &cart.bankmode),
	I4("romb", &cart.rombank),
	I4("ramb", &cart.rambank),
	I4("enab", &cart.enableram),

	I4("rtcR", &cart.rtc.sel),
	I4("rtcL", &cart.rtc.latch),
	I4("rtcF", &cart.rtc.flags),
	I4("rtcd", &cart.rtc.d),
	I4("rtch", &cart.rtc.h),
	I4("rtcm", &cart.rtc.m),
	I4("rtcs", &cart.rtc.s),
	I4("rtct", &cart.rtc.ticks),
	I1("rtR8", &cart.rtc.regs[0]),
	I1("rtR9", &cart.rtc.regs[1]),
	I1("rtRA", &cart.rtc.regs[2]),
	I1("rtRB", &cart.rtc.regs[3]),
	I1("rtRC", &cart.rtc.regs[4]),

	I4("S1on", &snd.ch[0].on),
	I4("S1p ", &snd.ch[0].pos),
	I4("S1c ", &snd.ch[0].cnt),
	I4("S1ec", &snd.ch[0].encnt),
	I4("S1sc", &snd.ch[0].swcnt),
	I4("S1sf", &snd.ch[0].swfreq),

	I4("S2on", &snd.ch[1].on),
	I4("S2p ", &snd.ch[1].pos),
	I4("S2c ", &snd.ch[1].cnt),
	I4("S2ec", &snd.ch[1].encnt),

	I4("S3on", &snd.ch[2].on),
	I4("S3p ", &snd.ch[2].pos),
	I4("S3c ", &snd.ch[2].cnt),

	I4("S4on", &snd.ch[3].on),
	I4("S4p ", &snd.ch[3].pos),
	I4("S4c ", &snd.ch[3].cnt),
	I4("S4ec", &snd.ch[3].encnt),

	END
};

// Set in the far future for VBA-M support
#define RTC_BASE 1893456000


bool gnuboy_sram_dirty(void)
{
	return cart.sram_dirty != 0;
}

int gnuboy_load_sram(const char *file)
{
	if (!cart.has_battery || !cart.ramsize || !file || !*file)
		return -1;

	FILE *f = fopen(file, "rb");
	if (!f)
		return -2;

	MESSAGE_INFO("Loading SRAM from '%s'\n", file);

	cart.sram_dirty = 0;
	cart.sram_saved = 0;

	for (int i = 0; i < cart.ramsize; i++)
	{
		if (fseek(f, i * 8192, SEEK_SET) == 0 && fread(cart.rambanks[i], 8192, 1, f) == 1)
		{
			MESSAGE_INFO("Loaded SRAM bank %d.\n", i);
			cart.sram_saved = (1 << i);
		}
	}

	if (cart.has_rtc)
	{
		srtc_t rtc_buf;

		if (fseek(f, cart.ramsize * 8192, SEEK_SET) == 0 && fread(&rtc_buf, 48, 1, f) == 1)
		{
			cart.rtc = (gb_rtc_t){
				.s = rtc_buf.s,
				.m = rtc_buf.m,
				.h = rtc_buf.h,
				.d = rtc_buf.d,
				.flags = rtc_buf.flags,
				.regs = {
					rtc_buf.regs[0],
					rtc_buf.regs[1],
					rtc_buf.regs[2],
					rtc_buf.regs[3],
					rtc_buf.regs[4],
				},
			};
			MESSAGE_INFO("Loaded RTC section %03d %02d:%02d:%02d.\n", cart.rtc.d, cart.rtc.h, cart.rtc.m, cart.rtc.s);
		}
	}

	fclose(f);

	return cart.sram_saved ? 0 : -1;
}


/**
 * If quick_save is set to true, gnuboy_save_sram will only save the sectors that
 * changed + the RTC. If set to false then a full sram file is created.
 */
int gnuboy_save_sram(const char *file, bool quick_save)
{
	if (!cart.has_battery || !cart.ramsize || !file || !*file)
		return -1;

	FILE *f = fopen(file, "wb");
	if (!f)
		return -2;

	MESSAGE_INFO("Saving SRAM to '%s'...\n", file);

	// Mark everything as dirty and unsaved (do a full save)
	if (!quick_save)
	{
		cart.sram_dirty = (1 << cart.ramsize) - 1;
		cart.sram_saved = 0;
	}

	for (int i = 0; i < cart.ramsize; i++)
	{
		if (!(cart.sram_saved & (1 << i)) || (cart.sram_dirty & (1 << i)))
		{
			if (fseek(f, i * 8192, SEEK_SET) == 0 && fwrite(cart.rambanks[i], 8192, 1, f) == 1)
			{
				MESSAGE_INFO("Saved SRAM bank %d.\n", i);
				cart.sram_dirty &= ~(1 << i);
				cart.sram_saved |= (1 << i);
			}
		}
	}

	if (cart.has_rtc)
	{
		srtc_t rtc_buf = {
			.s = cart.rtc.s,
			.m = cart.rtc.m,
			.h = cart.rtc.h,
			.d = cart.rtc.d,
			.flags = cart.rtc.flags,
			.regs = {
				cart.rtc.regs[0],
				cart.rtc.regs[1],
				cart.rtc.regs[2],
				cart.rtc.regs[3],
				cart.rtc.regs[4],
			},
			.rt = RTC_BASE + cart.rtc.s + (cart.rtc.m * 60) + (cart.rtc.h * 3600) + (cart.rtc.d * 86400),
		};
		if (fseek(f, cart.ramsize * 8192, SEEK_SET) == 0 && fwrite(&rtc_buf, 48, 1, f) == 1)
		{
			MESSAGE_INFO("Saved RTC section.\n");
		}
	}

	fclose(f);

	return cart.sram_dirty ? -1 : 0;
}


int gnuboy_save_state(const char *file)
{
	void *buf = calloc(1, 4096);
	if (!buf) return -2;

	FILE *fp = fopen(file, "wb");
	if (!fp) goto _error;

	bool is_cgb = hw.hwtype == GB_HW_CGB;

	sblock_t blocks[] = {
		{buf, 1},
		{hw.rambanks, is_cgb ? 8 : 2},
		{lcd.vbank, is_cgb ? 4 : 2},
		{cart.rambanks, cart.ramsize * 2},
		{NULL, 0},
	};

	un32 (*header)[2] = (un32 (*)[2])buf;

	ver = SAVE_VERSION;

	for (int i = 0; svars[i].ptr; i++)
	{
		un32 d = 0;

		switch (svars[i].len)
		{
		case 1:
			d = *(un8 *)svars[i].ptr;
			break;
		case 2:
			d = *(un16 *)svars[i].ptr;
			break;
		case 4:
			d = *(un32 *)svars[i].ptr;
			break;
		}

		header[i][0] = *(un32 *)svars[i].key;
		header[i][1] = LIL(d);
	}

	memcpy(buf+0x0CF0, snd.wave, sizeof snd.wave);
	memcpy(buf+0x0D00, hw.ioregs, sizeof hw.ioregs);
	memcpy(buf+0x0E00, lcd.pal, sizeof lcd.pal);
	memcpy(buf+0x0F00, lcd.oam.mem, sizeof lcd.oam);

	for (int i = 0; blocks[i].ptr != NULL; i++)
	{
		if (fwrite(blocks[i].ptr, 4096, blocks[i].len, fp) < 1)
		{
			MESSAGE_ERROR("Write error in block %d\n", i);
			goto _error;
		}
	}

	fclose(fp);
	free(buf);

	return 0;

_error:
	if (fp) fclose(fp);
	if (buf) free(buf);

	return -1;
}


int gnuboy_load_state(const char *file)
{
	void* buf = calloc(1, 4096);
	if (!buf) return -2;

	FILE *fp = fopen(file, "rb");
	if (!fp) goto _error;

	bool is_cgb = hw.hwtype == GB_HW_CGB;

	sblock_t blocks[] = {
		{buf, 1},
		{hw.rambanks, is_cgb ? 8 : 2},
		{lcd.vbank, is_cgb ? 4 : 2},
		{cart.rambanks, cart.ramsize * 2},
		{NULL, 0},
	};

	for (int i = 0; blocks[i].ptr != NULL; i++)
	{
		if (fread(blocks[i].ptr, 4096, blocks[i].len, fp) < 1)
		{
			MESSAGE_ERROR("Read error in block %d\n", i);
			goto _error;
		}
	}

	un32 (*header)[2] = (un32 (*)[2])buf;

	for (int i = 0; svars[i].ptr; i++)
	{
		un32 d = 0;

		for (int j = 0; header[j][0]; j++)
		{
			if (header[j][0] == *(un32 *)svars[i].key)
			{
				d = LIL(header[j][1]);
				break;
			}
		}

		switch (svars[i].len)
		{
		case 1:
			*(un8 *)svars[i].ptr = d;
			break;
		case 2:
			*(un16 *)svars[i].ptr = d;
			break;
		case 4:
			*(un32 *)svars[i].ptr = d;
			break;
		}
	}

	if (ver != SAVE_VERSION)
		MESSAGE_ERROR("Save file version mismatch!\n");

	memcpy(snd.wave, buf+0x0CF0, sizeof snd.wave);
	memcpy(hw.ioregs, buf+0x0D00, sizeof hw.ioregs);
	memcpy(lcd.pal, buf+0x0E00, sizeof lcd.pal);
	memcpy(lcd.oam.mem, buf+0x0F00, sizeof lcd.oam);

	fclose(fp);
	free(buf);

	// Disable BIOS. This is a hack to support old saves
	R_BIOS = 0x1;

	// Older saves might overflow this
	cart.rambank &= (cart.ramsize - 1);

	hw_refresh();

	return 0;

_error:
	if (fp) fclose(fp);
	if (buf) free(buf);

	return -1;
}
