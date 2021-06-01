// (c) Copyright 2007 notaz, All rights reserved.
// Free for non-commercial use.

// For commercial use, separate licencing terms must be obtained.

// don't like to use loads of #ifdefs, so duplicating GP2X code
// horribly instead

#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <unistd.h>
#include <sys/syslimits.h> // PATH_MAX

#include <pspdisplay.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <psputils.h>

#include "psp.h"
#include "emu.h"
#include "menu.h"
#include "mp3.h"
#include "../common/menu.h"
#include "../common/emu.h"
#include "../common/readpng.h"
#include "../common/lprintf.h"
#include "version.h"

#include <Pico/PicoInt.h>
#include <Pico/Patch.h>
#include <zlib/zlib.h>


#define pspKeyUnkn "???"
static const char * const pspKeyNames[] = {
	"SELECT",   pspKeyUnkn, pspKeyUnkn, "START",    "UP",       "RIGHT",     "DOWN",     "LEFT",
	"L",        "R",        pspKeyUnkn, pspKeyUnkn, "TRIANGLE", "CIRCLE",    "X",        "SQUARE",
	"HOME",     "HOLD",     "WLAN_UP",  "REMOTE",   "VOLUP",    "VOLDOWN",   "SCREEN",   "NOTE",
	pspKeyUnkn, pspKeyUnkn, pspKeyUnkn, pspKeyUnkn, "NUB UP",   "NUB RIGHT", "NUB DOWN", "NUB LEFT" // fake
};

static unsigned short bg_buffer[480*272] __attribute__((aligned(16)));
#define menu_screen psp_screen

static void menu_darken_bg(void *dst, const void *src, int pixels, int darker);
static void menu_prepare_bg(int use_game_bg, int use_fg);


static unsigned int inp_prev = 0;

static unsigned long wait_for_input(unsigned int interesting, int is_key_config)
{
	unsigned int ret;
	static int repeats = 0, wait = 50;
	int release = 0, count, i;

	if (!is_key_config)
		interesting |= (interesting & 0xf0) << 24; // also use analog

	if (repeats == 2 || repeats == 4) wait /= 2;
	if (repeats == 6) wait = 15;

	for (i = 0; i < 6 && inp_prev == psp_pad_read(1); i++) {
		if (i == 0) repeats++;
		psp_msleep(wait);
	}

	for (count = 0; !((ret = psp_pad_read(1)) & interesting) && count < 100; count++) {
		psp_msleep(50);
		release = 1;
	}

	if (release || ret != inp_prev) {
		repeats = 0;
		wait = 50;
	}
	inp_prev = ret;

	if (!is_key_config)
		ret |= (ret & 0xf0000000) >> 24; // use analog as d-pad

	// we don't need diagonals in menus
	if ((ret&BTN_UP)   && (ret&BTN_LEFT))  ret &= ~BTN_LEFT;
	if ((ret&BTN_UP)   && (ret&BTN_RIGHT)) ret &= ~BTN_RIGHT;
	if ((ret&BTN_DOWN) && (ret&BTN_LEFT))  ret &= ~BTN_LEFT;
	if ((ret&BTN_DOWN) && (ret&BTN_RIGHT)) ret &= ~BTN_RIGHT;

	return ret;
}

static void menu_draw_begin(void)
{
	// short *src = (short *)bg_buffer, *dst = (short *)menu_screen;
	// int i;

	// for (i = 272; i >= 0; i--, dst += 512, src += 480)
	//	memcpy32((int *)dst, (int *)src, 480*2/4);

	sceGuSync(0,0); // sync with prev
	sceGuStart(GU_DIRECT, guCmdList);
	sceGuCopyImage(GU_PSM_5650, 0, 0, 480, 272, 480, bg_buffer, 0, 0, 512, menu_screen);
	sceGuFinish();
	sceGuSync(0,0);
}


static void menu_draw_end(void)
{
	psp_video_flip(1);
}


// --------- loading ROM screen ----------

static int lcdr_line = 0;

static void load_progress_cb(int percent)
{
	int ln, len = percent * 480 / 100;
	unsigned short *dst;

	//sceDisplayWaitVblankStart();

	dst = (unsigned short *)menu_screen + 512*10*lcdr_line;

	if (len > 480) len = 480;
	for (ln = 8; ln > 0; ln--, dst += 512)
		memset(dst, 0xff, len*2);
}

static void cdload_progress_cb(int percent)
{
	int ln, len = percent * 480 / 100;
	unsigned short *dst;

	if (lcdr_line <= 2) {
		lcdr_line++;
		smalltext_out16(1, lcdr_line++ * 10, "Processing CD image / MP3s", 0xffff);
		smalltext_out16_lim(1, lcdr_line++ * 10, romFileName, 0xffff, 80);
	}

	dst = (unsigned short *)menu_screen + 512*10*lcdr_line;

	if (len > 480) len = 480;
	for (ln = 8; ln > 0; ln--, dst += 512)
		memset(dst, 0xff, len*2);
}

void menu_romload_prepare(const char *rom_name)
{
	const char *p = rom_name + strlen(rom_name);
	while (p > rom_name && *p != '/') p--;

	psp_video_switch_to_single();
	if (rom_data) menu_draw_begin();
	else memset32_uncached(psp_screen, 0, 512*272*2/4);

	smalltext_out16(1, 1, "Loading", 0xffff);
	smalltext_out16_lim(1, 10, p, 0xffff, 80);
	PicoCartLoadProgressCB = load_progress_cb;
	PicoCDLoadProgressCB = cdload_progress_cb;
	lcdr_line = 2;
}

void menu_romload_end(void)
{
	PicoCartLoadProgressCB = PicoCDLoadProgressCB = NULL;
	smalltext_out16(1, ++lcdr_line*10, "Starting emulation...", 0xffff);
}

// -------------- ROM selector --------------

// SceIoDirent
#define DT_DIR FIO_SO_IFDIR
#define DT_REG FIO_SO_IFREG

struct my_dirent
{
	unsigned char d_type;
	char d_name[255];
};

// bbbb bggg gggr rrrr
static unsigned short file2color(const char *fname)
{
	const char *ext = fname + strlen(fname) - 3;
	static const char *rom_exts[]   = { "zip", "bin", "smd", "gen", "iso", "cso" };
	static const char *other_exts[] = { "gmv", "pat" };
	int i;

	if (ext < fname) ext = fname;
	for (i = 0; i < sizeof(rom_exts)/sizeof(rom_exts[0]); i++)
		if (strcasecmp(ext, rom_exts[i]) == 0) return 0xfdf7;
	for (i = 0; i < sizeof(other_exts)/sizeof(other_exts[0]); i++)
		if (strcasecmp(ext, other_exts[i]) == 0) return 0xaff5;
	return 0xffff;
}

static void draw_dirlist(char *curdir, struct my_dirent **namelist, int n, int sel)
{
	int start, i, pos;

	start = 13 - sel;
	n--; // exclude current dir (".")

	menu_draw_begin();

	if (rom_data == NULL) {
//		menu_darken_bg(menu_screen, menu_screen, 321*240, 0);
	}

	menu_darken_bg((char *)menu_screen + 512*129*2, (char *)menu_screen + 512*129*2, 512*10, 0);

	if (start - 2 >= 0)
		smalltext_out16_lim(14, (start - 2)*10, curdir, 0xffff, 53-2);
	for (i = 0; i < n; i++) {
		pos = start + i;
		if (pos < 0)  continue;
		if (pos > 26) break;
		if (namelist[i+1]->d_type & DT_DIR) {
			smalltext_out16_lim(14,   pos*10, "/", 0xd7ff, 1);
			smalltext_out16_lim(14+6, pos*10, namelist[i+1]->d_name, 0xd7ff, 80-3);
		} else {
			unsigned short color = file2color(namelist[i+1]->d_name);
			smalltext_out16_lim(14,   pos*10, namelist[i+1]->d_name, color, 80-2);
		}
	}
	text_out16(5, 130, ">");
	menu_draw_end();
}

static int scandir_cmp(const void *p1, const void *p2)
{
	struct my_dirent **d1 = (struct my_dirent **)p1, **d2 = (struct my_dirent **)p2;
	if ((*d1)->d_type == (*d2)->d_type) return strcasecmp((*d1)->d_name, (*d2)->d_name);
	if ((*d1)->d_type & DT_DIR) return -1; // put before
	if ((*d2)->d_type & DT_DIR) return  1;
	return strcasecmp((*d1)->d_name, (*d2)->d_name);
}

static char *filter_exts[] = {
	".mp3", ".srm", ".brm", "s.gz", ".mds", "bcfg", ".txt", ".htm", "html",
	".jpg", ".cue", ".pbp"
};

static int scandir_filter(const struct my_dirent *ent)
{
	const char *p;
	int i;

	if (ent == NULL || ent->d_name == NULL) return 0;
	if (strlen(ent->d_name) < 5) return 1;

	p = ent->d_name + strlen(ent->d_name) - 4;

	for (i = 0; i < sizeof(filter_exts)/sizeof(filter_exts[0]); i++)
	{
		if (strcasecmp(p, filter_exts[i]) == 0) return 0;
	}

	return 1;
}

static int my_scandir(const char *dir, struct my_dirent ***namelist_out,
		int(*filter)(const struct my_dirent *),
		int(*compar)(const void *, const void *))
{
	int ret = -1, dir_uid = -1, name_alloc = 4, name_count = 0;
	struct my_dirent **namelist = NULL, *ent;
	SceIoDirent sce_ent;

	namelist = malloc(sizeof(*namelist) * name_alloc);
	if (namelist == NULL) { lprintf("%s:%i: OOM\n", __FILE__, __LINE__); goto fail; }

	// try to read first..
	dir_uid = sceIoDopen(dir);
	if (dir_uid >= 0)
	{
		/* it is very important to clear SceIoDirent to be passed to sceIoDread(), */
		/* or else it may crash, probably misinterpreting something in it. */
		memset(&sce_ent, 0, sizeof(sce_ent));
		ret = sceIoDread(dir_uid, &sce_ent);
		if (ret < 0)
		{
			lprintf("sceIoDread(\"%s\") failed with %i\n", dir, ret);
			goto fail;
		}
	}
	else
		lprintf("sceIoDopen(\"%s\") failed with %i\n", dir, dir_uid);

	while (ret > 0)
	{
		ent = malloc(sizeof(*ent));
		if (ent == NULL) { lprintf("%s:%i: OOM\n", __FILE__, __LINE__); goto fail; }
		ent->d_type = sce_ent.d_stat.st_attr;
		strncpy(ent->d_name, sce_ent.d_name, sizeof(ent->d_name));
		ent->d_name[sizeof(ent->d_name)-1] = 0;
		if (filter == NULL || filter(ent))
		     namelist[name_count++] = ent;
		else free(ent);

		if (name_count >= name_alloc)
		{
			void *tmp;
			name_alloc *= 2;
			tmp = realloc(namelist, sizeof(*namelist) * name_alloc);
			if (tmp == NULL) { lprintf("%s:%i: OOM\n", __FILE__, __LINE__); goto fail; }
			namelist = tmp;
		}

		memset(&sce_ent, 0, sizeof(sce_ent));
		ret = sceIoDread(dir_uid, &sce_ent);
	}

	// sort
	if (compar != NULL && name_count > 3) qsort(&namelist[2], name_count - 2, sizeof(namelist[0]), compar);

	// all done.
	ret = name_count;
	*namelist_out = namelist;
	goto end;

fail:
	if (namelist != NULL)
	{
		while (name_count--)
			free(namelist[name_count]);
		free(namelist);
	}
end:
	if (dir_uid >= 0) sceIoDclose(dir_uid);
	return ret;
}


static SceIoStat cpstat;

static char *romsel_loop(char *curr_path)
{
	struct my_dirent **namelist;
	int n, iret, sel = 0;
	unsigned long inp = 0;
	char *ret = NULL, *fname = NULL;

	// is this a dir or a full path?
	memset(&cpstat, 0, sizeof(cpstat));
	iret = sceIoGetstat(curr_path, &cpstat);
	if (iret >= 0 && (cpstat.st_attr & FIO_SO_IFREG)) { // file
		char *p;
		for (p = curr_path + strlen(curr_path) - 1; p > curr_path && *p != '/'; p--);
		if (p > curr_path) {
			*p = 0;
			fname = p+1;
		}
		else strcpy(curr_path, "ms0:/");
	}
	else if (iret >= 0 && (cpstat.st_attr & FIO_SO_IFDIR)); // dir
	else strcpy(curr_path, "ms0:/"); // something else

	n = my_scandir(curr_path, &namelist, scandir_filter, scandir_cmp);
	if (n < 0) {
		// try root..
		n = my_scandir("ms0:/", &namelist, scandir_filter, scandir_cmp);
		if (n < 0) {
			// oops, we failed
			lprintf("scandir failed, dir: "); lprintf(curr_path); lprintf("\n");
			return NULL;
		}
	}

	// try to find sel
	if (fname != NULL) {
		int i;
		for (i = 1; i < n; i++) {
			if (strcmp(namelist[i]->d_name, fname) == 0) {
				sel = i - 1;
				break;
			}
		}
	}

	for (;;)
	{
		draw_dirlist(curr_path, namelist, n, sel);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT|BTN_L|BTN_R|BTN_X|BTN_CIRCLE, 0);
		if(inp & BTN_UP  )  { sel--;   if (sel < 0)   sel = n-2; }
		if(inp & BTN_DOWN)  { sel++;   if (sel > n-2) sel = 0; }
		if(inp & BTN_LEFT)  { sel-=10; if (sel < 0)   sel = 0; }
		if(inp & BTN_L)     { sel-=24; if (sel < 0)   sel = 0; }
		if(inp & BTN_RIGHT) { sel+=10; if (sel > n-2) sel = n-2; }
		if(inp & BTN_R)     { sel+=24; if (sel > n-2) sel = n-2; }
		if(inp & BTN_CIRCLE) { // enter dir/select
			if (namelist[sel+1]->d_type & DT_REG) {
				strcpy(romFileName, curr_path);
				strcat(romFileName, "/");
				strcat(romFileName, namelist[sel+1]->d_name);
				ret = romFileName;
				break;
			} else if (namelist[sel+1]->d_type & DT_DIR) {
				int newlen = strlen(curr_path) + strlen(namelist[sel+1]->d_name) + 2;
				char *p, *newdir = malloc(newlen);
				if (strcmp(namelist[sel+1]->d_name, "..") == 0) {
					char *start = curr_path;
					p = start + strlen(start) - 1;
					while (*p == '/' && p > start) p--;
					while (*p != '/' && *p != ':' && p > start) p--;
					if (p <= start || *p == ':') strcpy(newdir, "ms0:/");
					else { strncpy(newdir, start, p-start); newdir[p-start] = 0; }
				} else {
					strcpy(newdir, curr_path);
					p = newdir + strlen(newdir) - 1;
					while (*p == '/' && p >= newdir) *p-- = 0;
					strcat(newdir, "/");
					strcat(newdir, namelist[sel+1]->d_name);
				}
				ret = romsel_loop(newdir);
				free(newdir);
				break;
			}
		}
		if(inp & BTN_X) break; // cancel
	}

	if (n > 0) {
		while(n--) free(namelist[n]);
		free(namelist);
	}

	return ret;
}

// ------------ debug menu ------------

char *debugString(void);

static void draw_debug(void)
{
	char *p, *str = debugString();
	int len, line;

	menu_draw_begin();

	p = str;
	for (line = 0; line < 24; line++)
	{
		while (*p && *p != '\n') p++;
		len = p - str;
		if (len > 55) len = 55;
		smalltext_out16_lim(1, line*10, str, 0xffff, len);
		if (*p == 0) break;
		p++; str = p;
	}
	menu_draw_end();
}

static void debug_menu_loop(void)
{
	int ret = 0;
	draw_debug();
	while (!(ret & (BTN_X|BTN_CIRCLE)))
		ret = wait_for_input(BTN_X|BTN_CIRCLE, 0);
}

// ------------ patch/gg menu ------------

static void draw_patchlist(int sel)
{
	int start, i, pos, active;

	start = 13 - sel;

	menu_draw_begin();

	for (i = 0; i < PicoPatchCount; i++) {
		pos = start + i;
		if (pos < 0)  continue;
		if (pos > 26) break;
		active = PicoPatches[i].active;
		smalltext_out16_lim(14,     pos*10, active ? "ON " : "OFF", active ? 0xfff6 : 0xffff, 3);
		smalltext_out16_lim(14+6*4, pos*10, PicoPatches[i].name, active ? 0xfff6 : 0xffff, 53-6);
	}
	pos = start + i;
	if (pos < 27) smalltext_out16_lim(14, pos*10, "done", 0xffff, 4);

	text_out16(5, 130, ">");
	menu_draw_end();
}


static void patches_menu_loop(void)
{
	int menu_sel = 0;
	unsigned long inp = 0;

	for(;;)
	{
		draw_patchlist(menu_sel);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT|BTN_L|BTN_R|BTN_X|BTN_CIRCLE, 0);
		if(inp & BTN_UP  ) { menu_sel--; if (menu_sel < 0) menu_sel = PicoPatchCount; }
		if(inp & BTN_DOWN) { menu_sel++; if (menu_sel > PicoPatchCount) menu_sel = 0; }
		if(inp &(BTN_LEFT|BTN_L))  { menu_sel-=10; if (menu_sel < 0) menu_sel = 0; }
		if(inp &(BTN_RIGHT|BTN_R)) { menu_sel+=10; if (menu_sel > PicoPatchCount) menu_sel = PicoPatchCount; }
		if(inp & BTN_CIRCLE) { // action
			if (menu_sel < PicoPatchCount)
				PicoPatches[menu_sel].active = !PicoPatches[menu_sel].active;
			else 	return;
		}
		if(inp & BTN_X) return;
	}

}

// ------------ savestate loader ------------

static int state_slot_flags = 0;

static void state_check_slots(void)
{
	int slot;

	state_slot_flags = 0;

	for (slot = 0; slot < 10; slot++)
	{
		if (emu_checkSaveFile(slot))
		{
			state_slot_flags |= 1 << slot;
		}
	}
}

static void *get_oldstate_for_preview(void)
{
	unsigned char *ptr = malloc(sizeof(Pico.vram) + sizeof(Pico.cram) + sizeof(Pico.vsram) + sizeof(Pico.video));
	if (ptr == NULL) return NULL;

	memcpy(ptr, Pico.vram, sizeof(Pico.vram));
	memcpy(ptr + sizeof(Pico.vram), Pico.cram, sizeof(Pico.cram));
	memcpy(ptr + sizeof(Pico.vram) + sizeof(Pico.cram), Pico.vsram, sizeof(Pico.vsram));
	memcpy(ptr + sizeof(Pico.vram) + sizeof(Pico.cram) + sizeof(Pico.vsram), &Pico.video, sizeof(Pico.video));
	return ptr;
}

static void restore_oldstate(void *ptrx)
{
	unsigned char *ptr = ptrx;
	memcpy(Pico.vram,  ptr,  sizeof(Pico.vram));
	memcpy(Pico.cram,  ptr + sizeof(Pico.vram), sizeof(Pico.cram));
	memcpy(Pico.vsram, ptr + sizeof(Pico.vram) + sizeof(Pico.cram), sizeof(Pico.vsram));
	memcpy(&Pico.video,ptr + sizeof(Pico.vram) + sizeof(Pico.cram) + sizeof(Pico.vsram), sizeof(Pico.video));
	free(ptrx);
}

static void draw_savestate_bg(int slot)
{
	void *file, *oldstate;
	char *fname;

	fname = emu_GetSaveFName(1, 0, slot);
	if (!fname) return;

	oldstate = get_oldstate_for_preview();
	if (oldstate == NULL) return;

	if (strcmp(fname + strlen(fname) - 3, ".gz") == 0) {
		file = gzopen(fname, "rb");
		emu_setSaveStateCbs(1);
	} else {
		file = fopen(fname, "rb");
		emu_setSaveStateCbs(0);
	}

	if (file) {
		if (PicoMCD & 1) {
			PicoCdLoadStateGfx(file);
		} else {
			areaSeek(file, 0x10020, SEEK_SET);  // skip header and RAM in state file
			areaRead(Pico.vram, 1, sizeof(Pico.vram), file);
			areaSeek(file, 0x2000, SEEK_CUR);
			areaRead(Pico.cram, 1, sizeof(Pico.cram), file);
			areaRead(Pico.vsram, 1, sizeof(Pico.vsram), file);
			areaSeek(file, 0x221a0, SEEK_SET);
			areaRead(&Pico.video, 1, sizeof(Pico.video), file);
		}
		areaClose(file);
	}

	emu_forcedFrame();
	menu_prepare_bg(1, 0);

	restore_oldstate(oldstate);
}

static void draw_savestate_menu(int menu_sel, int is_loading)
{
	int tl_x = 80+25, tl_y = 16+60, y, i;

	if (state_slot_flags & (1 << menu_sel))
		draw_savestate_bg(menu_sel);
	menu_draw_begin();

	text_out16(tl_x, 16+30, is_loading ? "Load state" : "Save state");

	menu_draw_selection(tl_x - 16, tl_y + menu_sel*10, 108);

	/* draw all 10 slots */
	y = tl_y;
	for (i = 0; i < 10; i++, y+=10)
	{
		text_out16(tl_x, y, "SLOT %i (%s)", i, (state_slot_flags & (1 << i)) ? "USED" : "free");
	}
	text_out16(tl_x, y, "back");

	menu_draw_end();
}

static int savestate_menu_loop(int is_loading)
{
	static int menu_sel = 10;
	int menu_sel_max = 10;
	unsigned long inp = 0;

	state_check_slots();

	for(;;)
	{
		draw_savestate_menu(menu_sel, is_loading);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_X|BTN_CIRCLE, 0);
		if(inp & BTN_UP  ) {
			do {
				menu_sel--; if (menu_sel < 0) menu_sel = menu_sel_max;
			} while (!(state_slot_flags & (1 << menu_sel)) && menu_sel != menu_sel_max && is_loading);
		}
		if(inp & BTN_DOWN) {
			do {
				menu_sel++; if (menu_sel > menu_sel_max) menu_sel = 0;
			} while (!(state_slot_flags & (1 << menu_sel)) && menu_sel != menu_sel_max && is_loading);
		}
		if(inp & BTN_CIRCLE) { // save/load
			if (menu_sel < 10) {
				state_slot = menu_sel;
				PicoStateProgressCB = emu_msg_cb; /* also suitable for menu */
				if (emu_SaveLoadGame(is_loading, 0)) {
					strcpy(menuErrorMsg, is_loading ? "Load failed" : "Save failed");
					return 1;
				}
				return 0;
			} else	return 1;
		}
		if(inp & BTN_X) return 1;
	}
}

// -------------- key config --------------

static char *action_binds(int player_idx, int action_mask)
{
	static char strkeys[32*5];
	int i;

	strkeys[0] = 0;
	for (i = 0; i < 32; i++) // i is key index
	{
		if (currentConfig.KeyBinds[i] & action_mask)
		{
			if (player_idx >= 0 && ((currentConfig.KeyBinds[i] >> 16) & 3) != player_idx) continue;
			if (strkeys[0]) {
				strcat(strkeys, i >= 28 ? ", " : " + "); // nub "buttons" don't create combos
				strcat(strkeys, pspKeyNames[i]);
				break;
			}
			else strcpy(strkeys, pspKeyNames[i]);
		}
	}

	return strkeys;
}

static void unbind_action(int action)
{
	int i;

	for (i = 0; i < 32; i++)
		currentConfig.KeyBinds[i] &= ~action;
}

static int count_bound_keys(int action, int pl_idx)
{
	int i, keys = 0;

	for (i = 0; i < 32; i++)
	{
		if (pl_idx >= 0 && (currentConfig.KeyBinds[i]&0x30000) != (pl_idx<<16)) continue;
		if (currentConfig.KeyBinds[i] & action) keys++;
	}

	return keys;
}

typedef struct { char *name; int mask; } bind_action_t;

static void draw_key_config(const bind_action_t *opts, int opt_cnt, int player_idx, int sel)
{
	int x, y, tl_y = 16+40, i;

	menu_draw_begin();
	if (player_idx >= 0) {
		text_out16(80+80, 16+20, "Player %i controls", player_idx + 1);
		x = 80+80;
	} else {
		text_out16(80+80, 16+20, "Emulator controls");
		x = 80+40;
	}

	menu_draw_selection(x - 16, tl_y + sel*10, (player_idx >= 0) ? 66 : 130);

	y = tl_y;
	for (i = 0; i < opt_cnt; i++, y+=10)
		text_out16(x, y, "%s : %s", opts[i].name, action_binds(player_idx, opts[i].mask));

	text_out16(x, y, "Done");

	if (sel < opt_cnt) {
		text_out16(80+30, 220, "Press a button to bind/unbind");
		text_out16(80+30, 230, "Use SELECT to clear");
		text_out16(80+30, 240, "To bind UP/DOWN, hold SELECT");
		text_out16(80+30, 250, "Select \"Done\" to exit");
	} else {
		text_out16(80+30, 230, "Use Options -> Save cfg");
		text_out16(80+30, 240, "to save controls");
		text_out16(80+30, 250, "Press X or O to exit");
	}
	menu_draw_end();
}

static void key_config_loop(const bind_action_t *opts, int opt_cnt, int player_idx)
{
	int sel = 0, menu_sel_max = opt_cnt, prev_select = 0, i;
	unsigned long inp = 0;

	for (;;)
	{
		draw_key_config(opts, opt_cnt, player_idx, sel);
		inp = wait_for_input(CONFIGURABLE_KEYS|BTN_SELECT, 1);
		if (!(inp & BTN_SELECT)) {
			prev_select = 0;
			if(inp & BTN_UP  ) { sel--; if (sel < 0) sel = menu_sel_max; continue; }
			if(inp & BTN_DOWN) { sel++; if (sel > menu_sel_max) sel = 0; continue; }
		}
		if (sel >= opt_cnt) {
			if (inp & (BTN_X|BTN_CIRCLE)) break;
			else continue;
		}
		// if we are here, we want to bind/unbind something
		if ((inp & BTN_SELECT) && !prev_select)
			unbind_action(opts[sel].mask);
		prev_select = inp & BTN_SELECT;
		inp &= CONFIGURABLE_KEYS;
		inp &= ~BTN_SELECT;
		for (i = 0; i < 32; i++)
			if (inp & (1 << i)) {
				if (count_bound_keys(opts[sel].mask, player_idx) >= 2)
				     currentConfig.KeyBinds[i] &= ~opts[sel].mask; // allow to unbind only
				else currentConfig.KeyBinds[i] ^=  opts[sel].mask;
				if (player_idx >= 0 && (currentConfig.KeyBinds[i] & opts[sel].mask)) {
					currentConfig.KeyBinds[i] &= ~(3 << 16);
					currentConfig.KeyBinds[i] |= player_idx << 16;
				}
			}
	}
}

static void draw_kc_sel(int menu_sel)
{
	int tl_x = 80+25+40, tl_y = 16+60, y;

	y = tl_y;
	menu_draw_begin();
	menu_draw_selection(tl_x - 16, tl_y + menu_sel*10, 138);

	text_out16(tl_x, y,       "Player 1");
	text_out16(tl_x, (y+=10), "Player 2");
	text_out16(tl_x, (y+=10), "Emulator controls");
	text_out16(tl_x, (y+=10), "Done");

	menu_draw_end();
}


// PicoPad[] format: MXYZ SACB RLDU
static bind_action_t ctrl_actions[] =
{
	{ "UP     ", 0x001 },
	{ "DOWN   ", 0x002 },
	{ "LEFT   ", 0x004 },
	{ "RIGHT  ", 0x008 },
	{ "A      ", 0x040 },
	{ "B      ", 0x010 },
	{ "C      ", 0x020 },
	{ "START  ", 0x080 },
	{ "MODE   ", 0x800 },
	{ "X      ", 0x400 },
	{ "Y      ", 0x200 },
	{ "Z      ", 0x100 },
};

// player2_flag, ?, ?, ?, ?, ?, ?, menu
// "NEXT SAVE SLOT", "PREV SAVE SLOT", "SWITCH RENDERER", "SAVE STATE",
// "LOAD STATE", "VOLUME UP", "VOLUME DOWN", "DONE"
static bind_action_t emuctrl_actions[] =
{
	{ "Load State     ", 1<<28 },
	{ "Save State     ", 1<<27 },
	{ "Prev Save Slot ", 1<<25 },
	{ "Next Save Slot ", 1<<24 },
	{ "Switch Renderer", 1<<26 },
};

static void kc_sel_loop(void)
{
	int menu_sel = 3, menu_sel_max = 3;
	unsigned long inp = 0;
	int is_6button = currentConfig.PicoOpt & 0x020;

	while (1)
	{
		draw_kc_sel(menu_sel);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_X|BTN_CIRCLE, 0);
		if (inp & BTN_UP  ) { menu_sel--; if (menu_sel < 0) menu_sel = menu_sel_max; }
		if (inp & BTN_DOWN) { menu_sel++; if (menu_sel > menu_sel_max) menu_sel = 0; }
		if (inp & BTN_CIRCLE) {
			switch (menu_sel) {
				case 0: key_config_loop(ctrl_actions, is_6button ? 12 : 8, 0); return;
				case 1: key_config_loop(ctrl_actions, is_6button ? 12 : 8, 1); return;
				case 2: key_config_loop(emuctrl_actions,
						sizeof(emuctrl_actions)/sizeof(emuctrl_actions[0]), -1); return;
				case 3: if (rom_data == NULL) emu_WriteConfig(0); return;
				default: return;
			}
		}
		if (inp & BTN_X) return;
	}
}


// --------- sega/mega cd options ----------

menu_entry cdopt_entries[] =
{
	{ NULL,                        MB_NONE,  MA_CDOPT_TESTBIOS_USA, NULL, 0, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_CDOPT_TESTBIOS_EUR, NULL, 0, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_CDOPT_TESTBIOS_JAP, NULL, 0, 0, 0, 1 },
	{ "CD LEDs",                   MB_ONOFF, MA_CDOPT_LEDS,         &currentConfig.EmuOpt,  0x0400, 0, 0, 1 },
	{ "CDDA audio (using mp3s)",   MB_ONOFF, MA_CDOPT_CDDA,         &currentConfig.PicoOpt, 0x0800, 0, 0, 1 },
	{ "PCM audio",                 MB_ONOFF, MA_CDOPT_PCM,          &currentConfig.PicoOpt, 0x0400, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_CDOPT_READAHEAD,    NULL, 0, 0, 0, 1 },
	{ "SaveRAM cart",              MB_ONOFF, MA_CDOPT_SAVERAM,      &currentConfig.PicoOpt, 0x8000, 0, 0, 1 },
	{ "Scale/Rot. fx (slow)",      MB_ONOFF, MA_CDOPT_SCALEROT_CHIP,&currentConfig.PicoOpt, 0x1000, 0, 0, 1 },
	{ "Better sync (slow)",        MB_ONOFF, MA_CDOPT_BETTER_SYNC,  &currentConfig.PicoOpt, 0x2000, 0, 0, 1 },
	{ "done",                      MB_NONE,  MA_CDOPT_DONE,         NULL, 0, 0, 0, 1 },
};

#define CDOPT_ENTRY_COUNT (sizeof(cdopt_entries) / sizeof(cdopt_entries[0]))


struct bios_names_t
{
	char us[32], eu[32], jp[32];
};

static void menu_cdopt_cust_draw(const menu_entry *entry, int x, int y, void *param)
{
	struct bios_names_t *bios_names = param;
	char ra_buff[16];

	switch (entry->id)
	{
		case MA_CDOPT_TESTBIOS_USA: text_out16(x, y, "USA BIOS:     %s", bios_names->us); break;
		case MA_CDOPT_TESTBIOS_EUR: text_out16(x, y, "EUR BIOS:     %s", bios_names->eu); break;
		case MA_CDOPT_TESTBIOS_JAP: text_out16(x, y, "JAP BIOS:     %s", bios_names->jp); break;
		case MA_CDOPT_READAHEAD:
			if (PicoCDBuffers > 1) sprintf(ra_buff, "%5iK", PicoCDBuffers * 2);
			else strcpy(ra_buff, "     OFF");
			text_out16(x, y, "ReadAhead buffer      %s", ra_buff);
			break;
		default:break;
	}
}

static void draw_cd_menu_options(int menu_sel, struct bios_names_t *bios_names)
{
	int tl_x = 80+25, tl_y = 16+60;
	menu_id selected_id;
	char ra_buff[16];

	if (PicoCDBuffers > 1) sprintf(ra_buff, "%5iK", PicoCDBuffers * 2);
	else strcpy(ra_buff, "     OFF");

	menu_draw_begin();

	menu_draw_selection(tl_x - 16, tl_y + menu_sel*10, 246);

	me_draw(cdopt_entries, CDOPT_ENTRY_COUNT, tl_x, tl_y, menu_cdopt_cust_draw, bios_names);

	selected_id = me_index2id(cdopt_entries, CDOPT_ENTRY_COUNT, menu_sel);
	if ((selected_id == MA_CDOPT_TESTBIOS_USA && strcmp(bios_names->us, "NOT FOUND")) ||
		(selected_id == MA_CDOPT_TESTBIOS_EUR && strcmp(bios_names->eu, "NOT FOUND")) ||
		(selected_id == MA_CDOPT_TESTBIOS_JAP && strcmp(bios_names->jp, "NOT FOUND")))
			text_out16(tl_x, 250, "Press start to test selected BIOS");

	menu_draw_end();
}

static void cd_menu_loop_options(void)
{
	static int menu_sel = 0;
	int menu_sel_max = 10;
	unsigned long inp = 0;
	struct bios_names_t bios_names;
	menu_id selected_id;
	char *bios, *p;

	if (emu_findBios(4, &bios)) { // US
		for (p = bios+strlen(bios)-1; p > bios && *p != '/'; p--);
		if (*p == '/') p++;
		strncpy(bios_names.us, p, sizeof(bios_names.us)); bios_names.us[sizeof(bios_names.us)-1] = 0;
	} else	strcpy(bios_names.us, "NOT FOUND");

	if (emu_findBios(8, &bios)) { // EU
		for (p = bios+strlen(bios)-1; p > bios && *p != '/'; p--);
		if (*p == '/') p++;
		strncpy(bios_names.eu, p, sizeof(bios_names.eu)); bios_names.eu[sizeof(bios_names.eu)-1] = 0;
	} else	strcpy(bios_names.eu, "NOT FOUND");

	if (emu_findBios(1, &bios)) { // JP
		for (p = bios+strlen(bios)-1; p > bios && *p != '/'; p--);
		if (*p == '/') p++;
		strncpy(bios_names.jp, p, sizeof(bios_names.jp)); bios_names.jp[sizeof(bios_names.jp)-1] = 0;
	} else	strcpy(bios_names.jp, "NOT FOUND");

	menuErrorMsg[0] = 0;

	for (;;)
	{
		draw_cd_menu_options(menu_sel, &bios_names);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT|BTN_X|BTN_CIRCLE|BTN_START, 0);
		if (inp & BTN_UP  ) { menu_sel--; if (menu_sel < 0) menu_sel = menu_sel_max; }
		if (inp & BTN_DOWN) { menu_sel++; if (menu_sel > menu_sel_max) menu_sel = 0; }
		selected_id = me_index2id(cdopt_entries, CDOPT_ENTRY_COUNT, menu_sel);
		if (inp & (BTN_LEFT|BTN_RIGHT)) { // multi choise
			if (!me_process(cdopt_entries, CDOPT_ENTRY_COUNT, selected_id, (inp&BTN_RIGHT) ? 1 : 0) &&
			    selected_id == MA_CDOPT_READAHEAD) {
				if (inp & BTN_LEFT) {
					PicoCDBuffers >>= 1;
					if (PicoCDBuffers < 64) PicoCDBuffers = 0;
				} else {
					if (PicoCDBuffers < 64) PicoCDBuffers = 64;
					else PicoCDBuffers <<= 1;
					if (PicoCDBuffers > 8*1024) PicoCDBuffers = 8*1024; // 16M
				}
			}
		}
		if (inp & BTN_CIRCLE) // toggleable options
			if (!me_process(cdopt_entries, CDOPT_ENTRY_COUNT, selected_id, 1) &&
			    selected_id == MA_CDOPT_DONE) {
				return;
			}
		if (inp & BTN_START) {
			switch (selected_id) { // BIOS testers
				case MA_CDOPT_TESTBIOS_USA:
					if (emu_findBios(4, &bios)) { // test US
						strcpy(romFileName, bios);
						engineState = PGS_ReloadRom;
						return;
					}
					break;
				case MA_CDOPT_TESTBIOS_EUR:
					if (emu_findBios(8, &bios)) { // test EU
						strcpy(romFileName, bios);
						engineState = PGS_ReloadRom;
						return;
					}
					break;
				case MA_CDOPT_TESTBIOS_JAP:
					if (emu_findBios(1, &bios)) { // test JP
						strcpy(romFileName, bios);
						engineState = PGS_ReloadRom;
						return;
					}
					break;
				default:
					break;
			}
		}
		if (inp & BTN_X) return;
	}
}

// --------- display options ----------

menu_entry opt3_entries[] =
{
	{ NULL,                        MB_NONE,  MA_OPT3_SCALE,         NULL, 0, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_OPT3_HSCALE32,      NULL, 0, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_OPT3_HSCALE40,      NULL, 0, 0, 0, 1 },
	{ NULL,                        MB_ONOFF, MA_OPT3_FILTERING,     &currentConfig.scaling, 1,  0,  0, 1 },
	{ NULL,                        MB_RANGE, MA_OPT3_GAMMAA,        &currentConfig.gamma,   0, -4, 16, 1 },
	{ NULL,                        MB_NONE,  MA_OPT3_VSYNC,         NULL, 0, 0, 0, 1 },
	{ "Set to unscaled centered",  MB_NONE,  MA_OPT3_PRES_NOSCALE,  NULL, 0, 0, 0, 1 },
	{ "Set to 4:3 scaled",         MB_NONE,  MA_OPT3_PRES_SCALE43,  NULL, 0, 0, 0, 1 },
	{ "Set to fullscreen",         MB_NONE,  MA_OPT3_PRES_FULLSCR,  NULL, 0, 0, 0, 1 },
	{ "done",                      MB_NONE,  MA_OPT3_DONE,          NULL, 0, 0, 0, 1 },
};

#define OPT3_ENTRY_COUNT (sizeof(opt3_entries) / sizeof(opt3_entries[0]))


static void menu_opt3_cust_draw(const menu_entry *entry, int x, int y, void *param)
{
	switch (entry->id)
	{
		case MA_OPT3_SCALE:
			text_out16(x, y, "Scale factor:                      %.2f", currentConfig.scale);
			break;
		case MA_OPT3_HSCALE32:
			text_out16(x, y, "Hor. scale (for low res. games):   %.2f", currentConfig.hscale32);
			break;
		case MA_OPT3_HSCALE40:
			text_out16(x, y, "Hor. scale (for hi res. games):    %.2f", currentConfig.hscale40);
			break;
		case MA_OPT3_FILTERING:
			text_out16(x, y, "Bilinear filtering                 %s", currentConfig.scaling?"ON":"OFF");
			break;
		case MA_OPT3_GAMMAA:
			text_out16(x, y, "Gamma adjustment                  %2i", currentConfig.gamma);
			break;
		case MA_OPT3_VSYNC: {
			char *val = "    never";
			if (currentConfig.EmuOpt & 0x2000)
				val = (currentConfig.EmuOpt & 0x10000) ? "sometimes" : "   always";
			text_out16(x, y, "Wait for vsync (slow)         %s", val);
			break;
		}
		default: break;
	}
}

static void menu_opt3_preview(int is_32col)
{
	void *oldstate = NULL;

	if (rom_data == NULL || ((Pico.video.reg[12]&1)^1) != is_32col)
	{
		extern char bgdatac32_start[], bgdatac40_start[];
		extern int bgdatac32_size, bgdatac40_size;
		void *bgdata = is_32col ? bgdatac32_start : bgdatac40_start;
		unsigned long insize = is_32col ? bgdatac32_size : bgdatac40_size, outsize = 65856;
		int ret;
		ret = uncompress((Bytef *)bg_buffer, &outsize, bgdata, insize);
		if (ret == 0)
		{
			if (rom_data != NULL) oldstate = get_oldstate_for_preview();
			memcpy(Pico.vram,  bg_buffer, sizeof(Pico.vram));
			memcpy(Pico.cram,  (char *)bg_buffer + 0x10000, 0x40*2);
			memcpy(Pico.vsram, (char *)bg_buffer + 0x10080, 0x40*2);
			memcpy(&Pico.video,(char *)bg_buffer + 0x10100, 0x40);
		}
		else
			lprintf("uncompress returned %i\n", ret);
	}

	memset32_uncached(psp_screen, 0, 512*272*2/4);
	emu_forcedFrame();
	menu_prepare_bg(1, 0);

	if (oldstate) restore_oldstate(oldstate);
}

static void draw_dispmenu_options(int menu_sel)
{
	int tl_x = 80, tl_y = 16+50;

	menu_draw_begin();

	menu_draw_selection(tl_x - 16, tl_y + menu_sel*10, 316);

	me_draw(opt3_entries, OPT3_ENTRY_COUNT, tl_x, tl_y, menu_opt3_cust_draw, NULL);

	menu_draw_end();
}

static void dispmenu_loop_options(void)
{
	static int menu_sel = 0;
	int menu_sel_max, is_32col = (Pico.video.reg[12]&1)^1;
	unsigned long inp = 0;
	menu_id selected_id;

	menu_sel_max = me_count_enabled(opt3_entries, OPT3_ENTRY_COUNT) - 1;

	for (;;)
	{
		draw_dispmenu_options(menu_sel);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT|BTN_X|BTN_CIRCLE, 0);
		if (inp & BTN_UP  ) { menu_sel--; if (menu_sel < 0) menu_sel = menu_sel_max; }
		if (inp & BTN_DOWN) { menu_sel++; if (menu_sel > menu_sel_max) menu_sel = 0; }
		selected_id = me_index2id(opt3_entries, OPT3_ENTRY_COUNT, menu_sel);
		if (selected_id == MA_OPT3_HSCALE40 &&  is_32col) { is_32col = 0; menu_opt3_preview(is_32col); }
		if (selected_id == MA_OPT3_HSCALE32 && !is_32col) { is_32col = 1; menu_opt3_preview(is_32col); }

		if (inp & (BTN_LEFT|BTN_RIGHT)) // multi choise
		{
			float *setting = NULL;
			int tmp;
			me_process(opt3_entries, OPT3_ENTRY_COUNT, selected_id, (inp&BTN_RIGHT) ? 1 : 0);
			switch (selected_id) {
				case MA_OPT3_SCALE:	setting = &currentConfig.scale; break;
				case MA_OPT3_HSCALE40:	setting = &currentConfig.hscale40; is_32col = 0; break;
				case MA_OPT3_HSCALE32:	setting = &currentConfig.hscale32; is_32col = 1; break;
				case MA_OPT3_FILTERING:
				case MA_OPT3_GAMMAA:	menu_opt3_preview(is_32col); break;
				case MA_OPT3_VSYNC:    tmp = ((currentConfig.EmuOpt>>13)&1) | ((currentConfig.EmuOpt>>15)&2);
					tmp = (inp & BTN_LEFT) ? (tmp>>1) : ((tmp<<1)|1);
					if (tmp > 3) tmp = 3;
					currentConfig.EmuOpt &= ~0x12000;
					currentConfig.EmuOpt |= ((tmp&2)<<15) | ((tmp&1)<<13);
					break;
				default: break;
			}
			if (setting != NULL) {
				while ((inp = psp_pad_read(0)) & (BTN_LEFT|BTN_RIGHT)) {
					*setting += (inp & BTN_LEFT) ? -0.01 : 0.01;
					if (*setting <= 0) *setting = 0.01;
					menu_opt3_preview(is_32col);
					draw_dispmenu_options(menu_sel); // will wait vsync
				}
			}
		}
		if (inp & BTN_CIRCLE) { // toggleable options
			me_process(opt3_entries, OPT3_ENTRY_COUNT, selected_id, 1);
			switch (selected_id) {
				case MA_OPT3_DONE:
					return;
				case MA_OPT3_PRES_NOSCALE:
					currentConfig.scale = currentConfig.hscale40 = currentConfig.hscale32 = 1.0;
					menu_opt3_preview(is_32col);
					break;
				case MA_OPT3_PRES_SCALE43:
					currentConfig.scale = 1.20;
					currentConfig.hscale40 = 1.00;
					currentConfig.hscale32 = 1.25;
					menu_opt3_preview(is_32col);
					break;
				case MA_OPT3_PRES_FULLSCR:
					currentConfig.scale = 1.20;
					currentConfig.hscale40 = 1.25;
					currentConfig.hscale32 = 1.56;
					menu_opt3_preview(is_32col);
					break;
				case MA_OPT3_FILTERING:
					menu_opt3_preview(is_32col);
					break;
				default: break;
			}
		}
		if (inp & BTN_X) return;
	}
}


// --------- advanced options ----------

menu_entry opt2_entries[] =
{
	{ "Emulate Z80",               MB_ONOFF, MA_OPT2_ENABLE_Z80,     &currentConfig.PicoOpt,0x00004, 0, 0, 1 },
	{ "Emulate YM2612 (FM)",       MB_ONOFF, MA_OPT2_ENABLE_YM2612,  &currentConfig.PicoOpt,0x00001, 0, 0, 1 },
	{ "Emulate SN76496 (PSG)",     MB_ONOFF, MA_OPT2_ENABLE_SN76496, &currentConfig.PicoOpt,0x00002, 0, 0, 1 },
	{ "gzip savestates",           MB_ONOFF, MA_OPT2_GZIP_STATES,    &currentConfig.EmuOpt, 0x00008, 0, 0, 1 },
	{ "Don't save last used ROM",  MB_ONOFF, MA_OPT2_NO_LAST_ROM,    &currentConfig.EmuOpt, 0x00020, 0, 0, 1 },
	{ "Status line in main menu",  MB_ONOFF, MA_OPT2_STATUS_LINE,    &currentConfig.EmuOpt, 0x20000, 0, 0, 1 },
	{ "Disable frame limitter",    MB_ONOFF, MA_OPT2_NO_FRAME_LIMIT, &currentConfig.EmuOpt, 0x40000, 0, 0, 1 },
	{ "done",                      MB_NONE,  MA_OPT2_DONE,           NULL, 0, 0, 0, 1 },
};

#define OPT2_ENTRY_COUNT (sizeof(opt2_entries) / sizeof(opt2_entries[0]))


static void draw_amenu_options(int menu_sel)
{
	int tl_x = 80+25, tl_y = 16+50;

	menu_draw_begin();

	menu_draw_selection(tl_x - 16, tl_y + menu_sel*10, 252);

	me_draw(opt2_entries, OPT2_ENTRY_COUNT, tl_x, tl_y, NULL, NULL);

	menu_draw_end();
}

static void amenu_loop_options(void)
{
	static int menu_sel = 0;
	int menu_sel_max;
	unsigned long inp = 0;
	menu_id selected_id;

	menu_sel_max = me_count_enabled(opt2_entries, OPT2_ENTRY_COUNT) - 1;

	for(;;)
	{
		draw_amenu_options(menu_sel);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT|BTN_X|BTN_CIRCLE, 0);
		if (inp & BTN_UP  ) { menu_sel--; if (menu_sel < 0) menu_sel = menu_sel_max; }
		if (inp & BTN_DOWN) { menu_sel++; if (menu_sel > menu_sel_max) menu_sel = 0; }
		selected_id = me_index2id(opt2_entries, OPT2_ENTRY_COUNT, menu_sel);
		if (inp & (BTN_LEFT|BTN_RIGHT)) { // multi choise
			if (!me_process(opt2_entries, OPT2_ENTRY_COUNT, selected_id, (inp&BTN_RIGHT) ? 1 : 0) &&
			    selected_id == MA_OPT2_GAMMA) {
				// TODO?
			}
		}
		if (inp & BTN_CIRCLE) { // toggleable options
			if (!me_process(opt2_entries, OPT2_ENTRY_COUNT, selected_id, 1) &&
			    selected_id == MA_OPT2_DONE) {
				return;
			}
		}
		if (inp & BTN_X) return;
	}
}

// -------------- options --------------


menu_entry opt_entries[] =
{
	{ NULL,                        MB_NONE,  MA_OPT_RENDERER,      NULL, 0, 0, 0, 1 },
	{ "Accurate timing (slower)",  MB_ONOFF, MA_OPT_ACC_TIMING,    &currentConfig.PicoOpt, 0x0040, 0, 0, 1 },
	{ "Accurate sprites (slower)", MB_ONOFF, MA_OPT_ACC_SPRITES,   &currentConfig.PicoOpt, 0x0080, 0, 0, 1 },
	{ "Show FPS",                  MB_ONOFF, MA_OPT_SHOW_FPS,      &currentConfig.EmuOpt,  0x0002, 0, 0, 1 },
	{ NULL,                        MB_RANGE, MA_OPT_FRAMESKIP,     &currentConfig.Frameskip, 0, -1, 16, 1 },
	{ "Enable sound",              MB_ONOFF, MA_OPT_ENABLE_SOUND,  &currentConfig.EmuOpt,  0x0004, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_OPT_SOUND_QUALITY, NULL, 0, 0, 0, 1 },
	{ "6 button pad",              MB_ONOFF, MA_OPT_6BUTTON_PAD,   &currentConfig.PicoOpt, 0x0020, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_OPT_REGION,        NULL, 0, 0, 0, 1 },
	{ "Use SRAM/BRAM savestates",  MB_ONOFF, MA_OPT_SRAM_STATES,   &currentConfig.EmuOpt,  0x0001, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_OPT_CONFIRM_STATES,NULL, 0, 0, 0, 1 },
	{ "Save slot",                 MB_RANGE, MA_OPT_SAVE_SLOT,     &state_slot, 0, 0, 9, 1 },
	{ NULL,                        MB_NONE,  MA_OPT_CPU_CLOCKS,    NULL, 0, 0, 0, 1 },
	{ "[Display options]",         MB_NONE,  MA_OPT_DISP_OPTS,     NULL, 0, 0, 0, 1 },
	{ "[Sega/Mega CD options]",    MB_NONE,  MA_OPT_SCD_OPTS,      NULL, 0, 0, 0, 1 },
	{ "[Advanced options]",        MB_NONE,  MA_OPT_ADV_OPTS,      NULL, 0, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_OPT_SAVECFG,       NULL, 0, 0, 0, 1 },
	{ "Save cfg for current game only",MB_NONE,MA_OPT_SAVECFG_GAME,NULL, 0, 0, 0, 1 },
	{ NULL,                        MB_NONE,  MA_OPT_LOADCFG,       NULL, 0, 0, 0, 1 },
};

#define OPT_ENTRY_COUNT (sizeof(opt_entries) / sizeof(opt_entries[0]))


static const char *region_name(unsigned int code)
{
	static const char *names[] = { "Auto", "      Japan NTSC", "      Japan PAL", "      USA", "      Europe" };
	static const char *names_short[] = { "", " JP", " JP", " US", " EU" };
	int u, i = 0;
	if (code) {
		code <<= 1;
		while((code >>= 1)) i++;
		if (i > 4) return "unknown";
		return names[i];
	} else {
		static char name[24];
		strcpy(name, "Auto:");
		for (u = 0; u < 3; u++) {
			i = 0; code = ((PicoAutoRgnOrder >> u*4) & 0xf) << 1;
			while((code >>= 1)) i++;
			strcat(name, names_short[i]);
		}
		return name;
	}
}


static void menu_opt_cust_draw(const menu_entry *entry, int x, int y, void *param)
{
	char *str, str24[24];

	switch (entry->id)
	{
		case MA_OPT_RENDERER:
			if (currentConfig.PicoOpt&0x10)
				str = "fast";
			else if (currentConfig.EmuOpt&0x80)
				str = "accurate";
			else
				str = " 8bit accurate"; // n/a
			text_out16(x, y, "Renderer:                  %s", str);
			break;
		case MA_OPT_FRAMESKIP:
			if (currentConfig.Frameskip < 0)
			     strcpy(str24, "Auto");
			else sprintf(str24, "%i", currentConfig.Frameskip);
			text_out16(x, y, "Frameskip                  %s", str24);
			break;
		case MA_OPT_SOUND_QUALITY:
			str = (currentConfig.PicoOpt&0x08)?"stereo":"mono";
			text_out16(x, y, "Sound Quality:     %5iHz %s", currentConfig.PsndRate, str);
			break;
		case MA_OPT_REGION:
			text_out16(x, y, "Region:              %s", region_name(currentConfig.PicoRegion));
			break;
		case MA_OPT_CONFIRM_STATES:
			switch ((currentConfig.EmuOpt >> 9) & 5) {
				default: str = "OFF";    break;
				case 1:  str = "writes"; break;
				case 4:  str = "loads";  break;
				case 5:  str = "both";   break;
			}
			text_out16(x, y, "Confirm savestate          %s", str);
			break;
		case MA_OPT_CPU_CLOCKS:
			text_out16(x, y, "CPU/bus clock       %3i/%3iMHz", currentConfig.CPUclock, currentConfig.CPUclock/2);
			break;
		case MA_OPT_SAVECFG:
			str24[0] = 0;
			if (config_slot != 0) sprintf(str24, " (profile: %i)", config_slot);
			text_out16(x, y, "Save cfg as default%s", str24);
			break;
		case MA_OPT_LOADCFG:
			text_out16(x, y, "Load cfg from profile %i", config_slot);
			break;
		default:
			lprintf("%s: unimplemented (%i)\n", __FUNCTION__, entry->id);
			break;
	}
}


static void draw_menu_options(int menu_sel)
{
	int tl_x = 80+25, tl_y = 16+24;

	menu_draw_begin();

	menu_draw_selection(tl_x - 16, tl_y + menu_sel*10, 284);

	me_draw(opt_entries, OPT_ENTRY_COUNT, tl_x, tl_y, menu_opt_cust_draw, NULL);

	menu_draw_end();
}

static int sndrate_prevnext(int rate, int dir)
{
	int i, rates[] = { 11025, 22050, 44100 };

	for (i = 0; i < 5; i++)
		if (rates[i] == rate) break;

	i += dir ? 1 : -1;
	if (i > 2) return dir ? 44100 : 22050;
	if (i < 0) return dir ? 22050 : 11025;
	return rates[i];
}

static void region_prevnext(int right)
{
	// jp_ntsc=1, jp_pal=2, usa=4, eu=8
	static int rgn_orders[] = { 0x148, 0x184, 0x814, 0x418, 0x841, 0x481 };
	int i;
	if (right) {
		if (!currentConfig.PicoRegion) {
			for (i = 0; i < 6; i++)
				if (rgn_orders[i] == PicoAutoRgnOrder) break;
			if (i < 5) PicoAutoRgnOrder = rgn_orders[i+1];
			else currentConfig.PicoRegion=1;
		}
		else currentConfig.PicoRegion<<=1;
		if (currentConfig.PicoRegion > 8) currentConfig.PicoRegion = 8;
	} else {
		if (!currentConfig.PicoRegion) {
			for (i = 0; i < 6; i++)
				if (rgn_orders[i] == PicoAutoRgnOrder) break;
			if (i > 0) PicoAutoRgnOrder = rgn_orders[i-1];
		}
		else currentConfig.PicoRegion>>=1;
	}
}

static void menu_options_save(void)
{
	PicoOpt = currentConfig.PicoOpt;
	PsndRate = currentConfig.PsndRate;
	PicoRegionOverride = currentConfig.PicoRegion;
	if (PicoRegionOverride) {
		// force setting possibly changed..
		Pico.m.pal = (PicoRegionOverride == 2 || PicoRegionOverride == 8) ? 1 : 0;
	}
	if (!(PicoOpt & 0x20)) {
		// unbind XYZ MODE, just in case
		unbind_action(0xf00);
	}
}

static int menu_loop_options(void)
{
	static int menu_sel = 0;
	int menu_sel_max, ret;
	unsigned long inp = 0;
	menu_id selected_id;

	currentConfig.PicoOpt = PicoOpt;
	currentConfig.PsndRate = PsndRate;
	currentConfig.PicoRegion = PicoRegionOverride;

	me_enable(opt_entries, OPT_ENTRY_COUNT, MA_OPT_SAVECFG_GAME, rom_data != NULL);
	me_enable(opt_entries, OPT_ENTRY_COUNT, MA_OPT_LOADCFG, config_slot != config_slot_current);
	menu_sel_max = me_count_enabled(opt_entries, OPT_ENTRY_COUNT) - 1;
	if (menu_sel > menu_sel_max) menu_sel = menu_sel_max;

	while (1)
	{
		draw_menu_options(menu_sel);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_LEFT|BTN_RIGHT|BTN_X|BTN_CIRCLE, 0);
		if (inp & BTN_UP  ) { menu_sel--; if (menu_sel < 0) menu_sel = menu_sel_max; }
		if (inp & BTN_DOWN) { menu_sel++; if (menu_sel > menu_sel_max) menu_sel = 0; }
		selected_id = me_index2id(opt_entries, OPT_ENTRY_COUNT, menu_sel);
		if (inp & (BTN_LEFT|BTN_RIGHT)) { // multi choise
			if (!me_process(opt_entries, OPT_ENTRY_COUNT, selected_id, (inp&BTN_RIGHT) ? 1 : 0)) {
				switch (selected_id) {
					case MA_OPT_RENDERER:
						if ((currentConfig.PicoOpt&0x10) || !(currentConfig.EmuOpt &0x80)) {
							currentConfig.PicoOpt&= ~0x10;
							currentConfig.EmuOpt |=  0x80;
						} else {
							currentConfig.PicoOpt|=  0x10;
							currentConfig.EmuOpt &= ~0x80;
						}
						break;
					case MA_OPT_SOUND_QUALITY:
						currentConfig.PsndRate = sndrate_prevnext(currentConfig.PsndRate, inp & BTN_RIGHT);
						break;
					case MA_OPT_REGION:
						region_prevnext(inp & BTN_RIGHT);
						break;
					case MA_OPT_CONFIRM_STATES: {
							 int n = ((currentConfig.EmuOpt>>9)&1) | ((currentConfig.EmuOpt>>10)&2);
							 n += (inp & BTN_LEFT) ? -1 : 1;
							 if (n < 0) n = 0; else if (n > 3) n = 3;
							 n |= n << 1; n &= ~2;
							 currentConfig.EmuOpt &= ~0xa00;
							 currentConfig.EmuOpt |= n << 9;
							 break;
						 }
					case MA_OPT_SAVE_SLOT:
						 if (inp & BTN_RIGHT) {
							 state_slot++; if (state_slot > 9) state_slot = 0;
						 } else {state_slot--; if (state_slot < 0) state_slot = 9;
						 }
						 break;
					case MA_OPT_CPU_CLOCKS:
						 while ((inp = psp_pad_read(0)) & (BTN_LEFT|BTN_RIGHT)) {
							 currentConfig.CPUclock += (inp & BTN_LEFT) ? -1 : 1;
							 if (currentConfig.CPUclock <  19) currentConfig.CPUclock = 19;
							 if (currentConfig.CPUclock > 333) currentConfig.CPUclock = 333;
							 draw_menu_options(menu_sel); // will wait vsync
						 }
						 break;
					case MA_OPT_SAVECFG:
					case MA_OPT_SAVECFG_GAME:
					case MA_OPT_LOADCFG:
						 config_slot += (inp&BTN_RIGHT) ? 1 : -1;
						 if (config_slot > 9) config_slot = 0;
						 if (config_slot < 0) config_slot = 9;
						 me_enable(opt_entries, OPT_ENTRY_COUNT, MA_OPT_LOADCFG, config_slot != config_slot_current);
						 menu_sel_max = me_count_enabled(opt_entries, OPT_ENTRY_COUNT) - 1;
						 if (menu_sel > menu_sel_max) menu_sel = menu_sel_max;
						 break;
					default:
						//lprintf("%s: something unknown selected (%i)\n", __FUNCTION__, selected_id);
						break;
				}
			}
		}
		if (inp & BTN_CIRCLE) {
			if (!me_process(opt_entries, OPT_ENTRY_COUNT, selected_id, 1))
			{
				switch (selected_id)
				{
					case MA_OPT_DISP_OPTS:
						dispmenu_loop_options();
						break;
					case MA_OPT_SCD_OPTS:
						cd_menu_loop_options();
						if (engineState == PGS_ReloadRom)
							return 0; // test BIOS
						break;
					case MA_OPT_ADV_OPTS:
						amenu_loop_options();
						break;
					case MA_OPT_SAVECFG: // done (update and write)
						menu_options_save();
						if (emu_WriteConfig(0)) strcpy(menuErrorMsg, "config saved");
						else strcpy(menuErrorMsg, "failed to write config");
						return 1;
					case MA_OPT_SAVECFG_GAME: // done (update and write for current game)
						menu_options_save();
						if (emu_WriteConfig(1)) strcpy(menuErrorMsg, "config saved");
						else strcpy(menuErrorMsg, "failed to write config");
						return 1;
					case MA_OPT_LOADCFG:
						ret = emu_ReadConfig(1, 1);
						if (!ret) ret = emu_ReadConfig(0, 1);
						if (ret)  strcpy(menuErrorMsg, "config loaded");
						else      strcpy(menuErrorMsg, "failed to load config");
						return 1;
					default:
						//lprintf("%s: something unknown selected (%i)\n", __FUNCTION__, selected_id);
						break;
				}
			}
		}
		if(inp & BTN_X) {
			menu_options_save();
			return 0;  // done (update, no write)
		}
	}
}

// -------------- credits --------------

static void draw_menu_credits(void)
{
	int tl_x = 80+15, tl_y = 16+64, y;
	menu_draw_begin();

	text_out16(tl_x, 16+20, "PicoDrive v" VERSION " (c) notaz, 2006,2007");

	y = tl_y;
	text_out16(tl_x, y, "Credits:");
	text_out16(tl_x, (y+=10), "fDave: base code of PicoDrive");
	text_out16(tl_x, (y+=10), "Chui: Fame/C");
	text_out16(tl_x, (y+=10), "NJ: CZ80");
	text_out16(tl_x, (y+=10), "MAME devs: YM2612 and SN76496 cores");
	text_out16(tl_x, (y+=10), "Stephane Dallongeville:");
	text_out16(tl_x, (y+=10), "    Gens code, base of Fame/C, CZ80");
	text_out16(tl_x, (y+=10), "Charles MacDonald: Genesis hw docs");
	text_out16(tl_x, (y+=10), "Haze: Genesis hw info");
	text_out16(tl_x, (y+=10), "ps2dev.org people: PSP SDK/code");
	text_out16(tl_x, (y+=10), "ketchupgun: skin design");

	menu_draw_end();
}


// -------------- root menu --------------

menu_entry main_entries[] =
{
	{ "Resume game",        MB_NONE, MA_MAIN_RESUME_GAME, NULL, 0, 0, 0, 0 },
	{ "Save State",         MB_NONE, MA_MAIN_SAVE_STATE,  NULL, 0, 0, 0, 0 },
	{ "Load State",         MB_NONE, MA_MAIN_LOAD_STATE,  NULL, 0, 0, 0, 0 },
	{ "Reset game",         MB_NONE, MA_MAIN_RESET_GAME,  NULL, 0, 0, 0, 0 },
	{ "Load new ROM/ISO",   MB_NONE, MA_MAIN_LOAD_ROM,    NULL, 0, 0, 0, 1 },
	{ "Change options",     MB_NONE, MA_MAIN_OPTIONS,     NULL, 0, 0, 0, 1 },
	{ "Configure controls", MB_NONE, MA_MAIN_CONTROLS,    NULL, 0, 0, 0, 1 },
	{ "Credits",            MB_NONE, MA_MAIN_CREDITS,     NULL, 0, 0, 0, 1 },
	{ "Patches / GameGenie",MB_NONE, MA_MAIN_PATCHES,     NULL, 0, 0, 0, 0 },
	{ "Exit",               MB_NONE, MA_MAIN_EXIT,        NULL, 0, 0, 0, 1 }
};

#define MAIN_ENTRY_COUNT (sizeof(main_entries) / sizeof(main_entries[0]))

static void draw_menu_root(int menu_sel)
{
	const int tl_x = 86+70, tl_y = 16+70;
	char *stat = NULL;

	menu_draw_begin();

	if ((currentConfig.EmuOpt&0x20000) && (stat = psp_get_status_line()))
		text_out16(287, 12, "%s", stat);

	text_out16(tl_x, 48, "PicoDrive v" VERSION);

	menu_draw_selection(tl_x - 16, tl_y + menu_sel*10, 146);

	me_draw(main_entries, MAIN_ENTRY_COUNT, tl_x, tl_y, NULL, NULL);

	// error
	if (menuErrorMsg[0])
		text_out16(10, 252, menuErrorMsg);
	menu_draw_end();
}


static void menu_loop_root(void)
{
	static int menu_sel = 0;
	int ret, menu_sel_max;
	unsigned long inp = 0;

	me_enable(main_entries, MAIN_ENTRY_COUNT, MA_MAIN_RESUME_GAME, rom_data != NULL);
	me_enable(main_entries, MAIN_ENTRY_COUNT, MA_MAIN_SAVE_STATE,  rom_data != NULL);
	me_enable(main_entries, MAIN_ENTRY_COUNT, MA_MAIN_LOAD_STATE,  rom_data != NULL);
	me_enable(main_entries, MAIN_ENTRY_COUNT, MA_MAIN_RESET_GAME,  rom_data != NULL);
	me_enable(main_entries, MAIN_ENTRY_COUNT, MA_MAIN_PATCHES,     PicoPatches != NULL);

	menu_sel_max = me_count_enabled(main_entries, MAIN_ENTRY_COUNT) - 1;
	if (menu_sel > menu_sel_max) menu_sel = menu_sel_max;

	// mp3 errors?
	if (mp3_last_error != 0) {
		if (mp3_last_error == -1)
		     sprintf(menuErrorMsg, "Unsupported mp3 format, use 44kHz stereo");
		else sprintf(menuErrorMsg, "mp3 init failed, code %08x", mp3_last_error);
		mp3_last_error = 0;
	}

	/* make sure action buttons are not pressed on entering menu */
	draw_menu_root(menu_sel);

	while (psp_pad_read(1) & (BTN_X|BTN_CIRCLE|BTN_SELECT)) psp_msleep(50);

	for (;;)
	{
		draw_menu_root(menu_sel);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_X|BTN_CIRCLE|BTN_SELECT|BTN_L|BTN_R, 0);
		if(inp & BTN_UP  )  { menu_sel--; if (menu_sel < 0) menu_sel = menu_sel_max; }
		if(inp & BTN_DOWN)  { menu_sel++; if (menu_sel > menu_sel_max) menu_sel = 0; }
		if((inp & (BTN_L|BTN_R)) == (BTN_L|BTN_R)) debug_menu_loop();
		if( inp & (BTN_SELECT|BTN_X)) {
			if (rom_data) {
				while (psp_pad_read(1) & (BTN_SELECT|BTN_X)) psp_msleep(50); // wait until released
				engineState = PGS_Running;
				break;
			}
		}
		if(inp & BTN_CIRCLE)  {
			menuErrorMsg[0] = 0; // clear error msg
			switch (me_index2id(main_entries, MAIN_ENTRY_COUNT, menu_sel))
			{
				case MA_MAIN_RESUME_GAME:
					if (rom_data) {
						while (psp_pad_read(1) & BTN_CIRCLE) psp_msleep(50);
						engineState = PGS_Running;
						return;
					}
					break;
				case MA_MAIN_SAVE_STATE:
					if (rom_data) {
						if(savestate_menu_loop(0))
							continue;
						engineState = PGS_Running;
						return;
					}
					break;
				case MA_MAIN_LOAD_STATE:
					if (rom_data) {
						if(savestate_menu_loop(1))
							continue;
						engineState = PGS_Running;
						return;
					}
					break;
				case MA_MAIN_RESET_GAME:
					if (rom_data) {
						emu_ResetGame();
						engineState = PGS_Running;
						return;
					}
					break;
				case MA_MAIN_LOAD_ROM:
				{
					char curr_path[PATH_MAX], *selfname;
					FILE *tstf;
					if ( (tstf = fopen(currentConfig.lastRomFile, "rb")) )
					{
						fclose(tstf);
						strcpy(curr_path, currentConfig.lastRomFile);
					}
					else
						getcwd(curr_path, PATH_MAX);
					selfname = romsel_loop(curr_path);
					if (selfname) {
						lprintf("selected file: %s\n", selfname);
						engineState = PGS_ReloadRom;
						return;
					}
					break;
				}
				case MA_MAIN_OPTIONS:
					ret = menu_loop_options();
					if (ret == 1) continue; // status update
					if (engineState == PGS_ReloadRom)
						return; // BIOS test
					break;
				case MA_MAIN_CONTROLS:
					kc_sel_loop();
					break;
				case MA_MAIN_CREDITS:
					draw_menu_credits();
					psp_msleep(500);
					inp = 0;
					while (!(inp & (BTN_X|BTN_CIRCLE)))
						inp = wait_for_input(BTN_X|BTN_CIRCLE, 0);
					break;
				case MA_MAIN_EXIT:
					engineState = PGS_Quit;
					return;
				case MA_MAIN_PATCHES:
					if (rom_data && PicoPatches) {
						patches_menu_loop();
						PicoPatchApply();
						strcpy(menuErrorMsg, "Patches applied");
						continue;
					}
					break;
				default:
					lprintf("%s: something unknown selected\n", __FUNCTION__);
					break;
			}
		}
	}
}

// warning: alignment
static void menu_darken_bg(void *dst, const void *src, int pixels, int darker)
{
	unsigned int *dest = dst;
	const unsigned int *srce = src;
	pixels /= 2;
	if (darker)
	{
		while (pixels--)
		{
			unsigned int p = *srce++;
			*dest++ = ((p&0xf79ef79e)>>1) - ((p&0xc618c618)>>3);
		}
	}
	else
	{
		while (pixels--)
		{
			unsigned int p = *srce++;
			*dest++ = (p&0xf79ef79e)>>1;
		}
	}
}

static void menu_prepare_bg(int use_game_bg, int use_fg)
{
	if (use_game_bg)
	{
		// darken the active framebuffer
		unsigned short *dst = bg_buffer;
		unsigned short *src = use_fg ? psp_video_get_active_fb() : psp_screen;
		int i;
		for (i = 272; i > 0; i--, dst += 480, src += 512)
			menu_darken_bg(dst, src, 480, 1);
		//memset32_uncached((int *)(bg_buffer + 480*264), 0, 480*8*2/4);
	}
	else
	{
		// should really only happen once, on startup..
		memset32_uncached((int *)(void *)bg_buffer, 0, sizeof(bg_buffer)/4);
		readpng(bg_buffer, "skin/background.png", READPNG_BG);
	}
	sceKernelDcacheWritebackAll();
}

static void menu_gfx_prepare(void)
{
	menu_prepare_bg(rom_data != NULL, 1);

	menu_draw_begin();
	menu_draw_end();
}


void menu_loop(void)
{
	menu_gfx_prepare();

	menu_loop_root();

	menuErrorMsg[0] = 0;
}


// --------- CD tray close menu ----------

static void draw_menu_tray(int menu_sel)
{
	int tl_x = 70, tl_y = 90, y;

	menu_draw_begin();

	text_out16(tl_x, 20, "The unit is about to");
	text_out16(tl_x, 30, "close the CD tray.");

	y = tl_y;
	text_out16(tl_x, y,       "Load new CD image");
	text_out16(tl_x, (y+=10), "Insert nothing");

	// draw cursor
	text_out16(tl_x - 16, tl_y + menu_sel*10, ">");
	// error
	if (menuErrorMsg[0]) text_out16(5, 226, menuErrorMsg);
	menu_draw_end();
}


int menu_loop_tray(void)
{
	int menu_sel = 0, menu_sel_max = 1;
	unsigned long inp = 0;
	char curr_path[PATH_MAX], *selfname;
	FILE *tstf;

	menu_gfx_prepare();

	if ( (tstf = fopen(currentConfig.lastRomFile, "rb")) )
	{
		fclose(tstf);
		strcpy(curr_path, currentConfig.lastRomFile);
	}
	else
	{
		getcwd(curr_path, PATH_MAX);
	}

	/* make sure action buttons are not pressed on entering menu */
	draw_menu_tray(menu_sel);
	while (psp_pad_read(1) & BTN_CIRCLE) psp_msleep(50);

	for (;;)
	{
		draw_menu_tray(menu_sel);
		inp = wait_for_input(BTN_UP|BTN_DOWN|BTN_CIRCLE, 0);
		if(inp & BTN_UP  )  { menu_sel--; if (menu_sel < 0) menu_sel = menu_sel_max; }
		if(inp & BTN_DOWN)  { menu_sel++; if (menu_sel > menu_sel_max) menu_sel = 0; }
		if(inp & BTN_CIRCLE)  {
			switch (menu_sel) {
				case 0: // select image
					selfname = romsel_loop(curr_path);
					if (selfname) {
						int ret = -1, cd_type;
						cd_type = emu_cdCheck(NULL);
						if (cd_type > 0)
							ret = Insert_CD(romFileName, cd_type == 2);
						if (ret != 0) {
							sprintf(menuErrorMsg, "Load failed, invalid CD image?");
							lprintf("%s\n", menuErrorMsg);
							continue;
						}
						engineState = PGS_RestartRun;
						return 1;
					}
					break;
				case 1: // insert nothing
					engineState = PGS_RestartRun;
					return 0;
			}
		}
		menuErrorMsg[0] = 0; // clear error msg
	}
}


