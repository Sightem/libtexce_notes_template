#include "ntx_pack.h"

#include <fontlibc.h>
#include <graphx.h>
#include <keypadc.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <tice.h>
#include <tex/tex.h>
#include <tex_renderer.h>

#define COL_BG 255
#define COL_FG 0
#define UI_COL_BG 248
#define UI_COL_PANEL 249
#define UI_COL_HEADER 250
#define UI_COL_SEL 251
#define UI_COL_ACCENT 252
#define UI_COL_BORDER 253
#define RENDERER_SLAB_SIZE ((size_t)20 * 1024)

typedef struct
{
	uint16_t note_index;
	uint16_t chunk_index;
} ChunkMenuItem;

static void setup_menu_palette(void)
{
	gfx_palette[UI_COL_BG] = gfx_RGBTo1555(240, 242, 246);
	gfx_palette[UI_COL_PANEL] = gfx_RGBTo1555(224, 229, 236);
	gfx_palette[UI_COL_HEADER] = gfx_RGBTo1555(40, 68, 96);
	gfx_palette[UI_COL_SEL] = gfx_RGBTo1555(173, 196, 235);
	gfx_palette[UI_COL_ACCENT] = gfx_RGBTo1555(245, 177, 66);
	gfx_palette[UI_COL_BORDER] = gfx_RGBTo1555(130, 150, 170);
}

static void print_limited(const char* text, int max_chars)
{
	if (!text || max_chars <= 0)
		return;
	char buf[80];
	int n = 0;
	while (text[n] && n < max_chars && n < (int)(sizeof(buf) - 1))
	{
		buf[n] = text[n];
		n++;
	}
	buf[n] = '\0';
	if (text[n] && n >= 3 && n < (int)(sizeof(buf) - 1))
	{
		buf[n - 3] = '.';
		buf[n - 2] = '.';
		buf[n - 1] = '.';
	}
	gfx_PrintString(buf);
}

static bool require_fontpacks(fontlib_font_t** out_main, fontlib_font_t** out_script)
{
	fontlib_font_t* font_main = fontlib_GetFontByIndex("TeXFonts", 0);
	fontlib_font_t* font_script = fontlib_GetFontByIndex("TeXScrpt", 0);
	if (font_main && font_script)
	{
		if (out_main)
			*out_main = font_main;
		if (out_script)
			*out_script = font_script;
		return true;
	}

	gfx_FillScreen(COL_BG);
	gfx_SetTextFGColor(COL_FG);
	gfx_SetTextXY(4, 10);
	gfx_PrintString("Missing required fonts");
	gfx_SetTextXY(4, 24);
	if (!font_main)
		gfx_PrintString("- TeXFonts.8xv");
	gfx_SetTextXY(4, 34);
	if (!font_script)
		gfx_PrintString("- TeXScrpt.8xv");
	gfx_SetTextXY(4, 54);
	gfx_PrintString("Copy from assets/");
	gfx_SetTextXY(4, 68);
	gfx_PrintString("Press CLEAR");
	gfx_SwapDraw();

	while (true)
	{
		kb_Scan();
		if (kb_Data[6] & kb_Clear)
			break;
	}
	return false;
}

static bool build_chunk_menu(const NtxIndex* idx, ChunkMenuItem** out_items, uint16_t* out_count)
{
	if (!idx || !out_items || !out_count)
		return false;
	*out_items = NULL;
	*out_count = 0;

	uint32_t total = 0;
	for (uint16_t i = 0; i < idx->count; ++i)
		total += idx->entries[i].total_chunks;

	if (total == 0 || total > 0xFFFFu)
		return true;

	ChunkMenuItem* items = (ChunkMenuItem*)calloc((size_t)total, sizeof(ChunkMenuItem));
	if (!items)
		return false;

	uint32_t pos = 0;
	for (uint16_t n = 0; n < idx->count; ++n)
	{
		const NtxNoteEntry* note = &idx->entries[n];
		for (uint16_t c = 0; c < note->total_chunks; ++c)
		{
			items[pos].note_index = n;
			items[pos].chunk_index = c;
			pos++;
		}
	}

	*out_items = items;
	*out_count = (uint16_t)total;
	return true;
}

static void draw_chunk_menu(const NtxIndex* idx, const ChunkMenuItem* items, uint16_t count, int sel)
{
	gfx_FillScreen(UI_COL_BG);

	gfx_SetColor(UI_COL_HEADER);
	gfx_FillRectangle_NoClip(0, 0, GFX_LCD_WIDTH, 20);
	gfx_SetTextFGColor(255);
	gfx_SetTextXY(6, 6);
	gfx_PrintString("notes_viewer");

	char hdr[48];
	snprintf(hdr, sizeof(hdr), "chunks:%u", (unsigned)count);
	int hdr_w = (int)gfx_GetStringWidth(hdr);
	gfx_SetTextXY(GFX_LCD_WIDTH - hdr_w - 6, 6);
	gfx_PrintString(hdr);

	gfx_SetColor(UI_COL_PANEL);
	gfx_FillRectangle_NoClip(0, GFX_LCD_HEIGHT - 12, GFX_LCD_WIDTH, 12);
	gfx_SetTextFGColor(COL_FG);
	gfx_SetTextXY(6, GFX_LCD_HEIGHT - 10);
	gfx_PrintString("UP/DOWN:Move ENTER:Open CLEAR:Exit");

	if (!items || count == 0)
	{
		gfx_SetTextFGColor(COL_FG);
		gfx_SetTextXY(6, 30);
		gfx_PrintString("No chunks available.");
		gfx_SwapDraw();
		return;
	}

	const int list_x = 4;
	const int list_y = 24;
	const int list_w = GFX_LCD_WIDTH - 12;
	const int list_h = GFX_LCD_HEIGHT - list_y - 16;
	const int row_h = 18;
	const int visible_rows = list_h / row_h;
	int top = sel - (visible_rows / 2);
	if (top < 0)
		top = 0;
	if (top > (int)count - visible_rows)
		top = (int)count - visible_rows;
	if (top < 0)
		top = 0;

	int y = list_y;
	for (int r = 0; r < visible_rows; ++r)
	{
		int i = top + r;
		if (i >= count)
			break;
		const ChunkMenuItem* mi = &items[i];
		const NtxNoteEntry* note = &idx->entries[mi->note_index];

		const bool is_sel = (i == sel);
		gfx_SetColor(is_sel ? UI_COL_SEL : UI_COL_PANEL);
		gfx_FillRectangle(list_x, y, list_w, row_h - 2);
		gfx_SetColor(is_sel ? UI_COL_ACCENT : UI_COL_BORDER);
		gfx_Rectangle(list_x, y, list_w, row_h - 2);

		char rhs[20];
		snprintf(rhs, sizeof(rhs), "%u/%u", (unsigned)(mi->chunk_index + 1), (unsigned)note->total_chunks);
		const int rhs_w = (int)gfx_GetStringWidth(rhs);

		gfx_SetTextFGColor(COL_FG);
		gfx_SetTextXY(list_x + 4, y + 5);
		print_limited(note->title ? note->title : "(untitled)", 28);
		gfx_SetTextXY(list_x + list_w - rhs_w - 6, y + 5);
		gfx_PrintString(rhs);
		y += row_h;
	}

	if ((int)count > visible_rows)
	{
		const int track_x = GFX_LCD_WIDTH - 6;
		const int track_y = list_y;
		const int track_h = list_h;
		int thumb_h = (track_h * visible_rows) / (int)count;
		if (thumb_h < 10)
			thumb_h = 10;
		const int travel = track_h - thumb_h;
		const int denom = (int)count - visible_rows;
		const int thumb_y = track_y + ((denom > 0) ? ((travel * top) / denom) : 0);

		gfx_SetColor(UI_COL_BORDER);
		gfx_FillRectangle(track_x, track_y, 2, track_h);
		gfx_SetColor(UI_COL_ACCENT);
		gfx_FillRectangle(track_x, thumb_y, 2, thumb_h);
	}

	gfx_SwapDraw();
}

static void wait_for_nav_key_release(void)
{
	while (true)
	{
		kb_Scan();
		const bool held_clear = (kb_Data[6] & kb_Clear) != 0;
		const bool held_enter = (kb_Data[6] & kb_Enter) != 0;
		const bool held_2nd = (kb_Data[1] & kb_2nd) != 0;
		if (!held_clear && !held_enter && !held_2nd)
			break;
	}
}

static void view_chunk_tex(const NtxNoteEntry* note, uint16_t chunk_index, TeX_Renderer* renderer)
{
	char err[64] = { 0 };
	char* text = NULL;
	uint16_t text_len = 0;
	uint8_t split_kind = 0;
	if (!ntx_load_chunk_text(note, chunk_index, &text, &text_len, &split_kind, err, sizeof(err)))
	{
		gfx_FillScreen(COL_BG);
		gfx_SetTextFGColor(COL_FG);
		gfx_SetTextXY(4, 10);
		gfx_PrintString("Chunk load failed");
		gfx_SetTextXY(4, 24);
		gfx_PrintString(err);
		gfx_SetTextXY(4, 40);
		gfx_PrintString("Press CLEAR");
		gfx_SwapDraw();
		while (true)
		{
			kb_Scan();
			if (kb_Data[6] & kb_Clear)
				break;
		}
		return;
	}
	if (!renderer)
	{
		free(text);
		gfx_FillScreen(COL_BG);
		gfx_SetTextFGColor(COL_FG);
		gfx_SetTextXY(4, 10);
		gfx_PrintString("Renderer unavailable");
		gfx_SetTextXY(4, 24);
		gfx_PrintString("Press CLEAR");
		gfx_SwapDraw();
		while (true)
		{
			kb_Scan();
			if (kb_Data[6] & kb_Clear)
				break;
		}
		return;
	}

	TeX_Config cfg = {
		.color_fg = COL_FG,
		.color_bg = COL_BG,
		.font_pack = "TeXFonts",
		.error_callback = NULL,
		.error_userdata = NULL,
	};
	const int margin = 4;
	const int header_h = 12;
	const int footer_h = 10;
	const int content_width = GFX_LCD_WIDTH - (margin * 2);
	const int viewport_h = GFX_LCD_HEIGHT - header_h - footer_h;

	TeX_Layout* layout = tex_format(text, content_width, &cfg);
	tex_renderer_invalidate(renderer);

	int scroll_y = 0;
	int total_h = layout ? tex_get_total_height(layout) : 0;
	int max_scroll = (total_h > viewport_h) ? (total_h - viewport_h) : 0;

	bool prev_up = false;
	bool prev_down = false;
	bool prev_clear = false;
	bool prev_2nd = false;

	while (true)
	{
		kb_Scan();
		bool now_up = (kb_Data[7] & kb_Up) != 0;
		bool now_down = (kb_Data[7] & kb_Down) != 0;
		bool now_clear = (kb_Data[6] & kb_Clear) != 0;
		bool now_2nd = (kb_Data[1] & kb_2nd) != 0;

		bool up_press = now_up && !prev_up;
		bool down_press = now_down && !prev_down;
		bool clear_press = now_clear && !prev_clear;
		bool second_press = now_2nd && !prev_2nd;

		prev_up = now_up;
		prev_down = now_down;
		prev_clear = now_clear;
		prev_2nd = now_2nd;

		if (up_press && scroll_y > 0)
		{
			scroll_y -= 10;
			if (scroll_y < 0)
				scroll_y = 0;
		}
		if (down_press && scroll_y < max_scroll)
		{
			scroll_y += 10;
			if (scroll_y > max_scroll)
				scroll_y = max_scroll;
		}
		if (clear_press || second_press)
			break;

		gfx_FillScreen(COL_BG);
		gfx_SetTextFGColor(COL_FG);
		gfx_SetTextXY(2, 1);
		gfx_PrintString(note->title ? note->title : "(untitled)");

		char hdr[64];
		snprintf(hdr, sizeof(hdr), "chunk %u/%u k=%u", (unsigned)(chunk_index + 1), (unsigned)note->total_chunks,
		         (unsigned)split_kind);
		gfx_SetTextXY(180, 1);
		gfx_PrintString(hdr);

		if (layout)
		{
			gfx_SetClipRegion(0, header_h, GFX_LCD_WIDTH, GFX_LCD_HEIGHT - footer_h);
			tex_draw(renderer, layout, margin, header_h, scroll_y);
			gfx_SetClipRegion(0, 0, GFX_LCD_WIDTH, GFX_LCD_HEIGHT);
		}
		else
		{
			gfx_SetTextXY(4, 20);
			gfx_PrintString("render init failed");
		}

		gfx_SetTextXY(2, GFX_LCD_HEIGHT - 9);
		gfx_PrintString("CLEAR/2ND:Back");
		gfx_SwapDraw();
	}

	if (layout)
		tex_free(layout);
	free(text);
}

int main(void)
{
	gfx_Begin();
	gfx_SetDrawBuffer();
	setup_menu_palette();
	gfx_SetTextFGColor(COL_FG);
	gfx_SetTextBGColor(COL_BG);
	fontlib_SetTransparency(true);

	fontlib_font_t* font_main = NULL;
	fontlib_font_t* font_script = NULL;
	if (!require_fontpacks(&font_main, &font_script))
	{
		gfx_End();
		return 1;
	}
	tex_draw_set_fonts(font_main, font_script);
	TeX_Renderer* renderer = tex_renderer_create_sized(RENDERER_SLAB_SIZE);
	if (!renderer)
	{
		gfx_FillScreen(COL_BG);
		gfx_SetTextFGColor(COL_FG);
		gfx_SetTextXY(4, 10);
		gfx_PrintString("TeX renderer OOM");
		gfx_SetTextXY(4, 24);
		gfx_PrintString("Need more free RAM");
		gfx_SetTextXY(4, 40);
		gfx_PrintString("Press CLEAR");
		gfx_SwapDraw();
		while (!(kb_Data[6] & kb_Clear))
			kb_Scan();
		gfx_End();
		return 1;
	}

	NtxIndex idx;
	char err[64] = { 0 };
	if (!ntx_load_index(&idx, err, sizeof(err)))
	{
		gfx_FillScreen(COL_BG);
		gfx_SetTextXY(4, 10);
		gfx_PrintString("NTXIDX load failed");
		gfx_SetTextXY(4, 24);
		gfx_PrintString(err);
		gfx_SetTextXY(4, 40);
		gfx_PrintString("Press CLEAR");
		gfx_SwapDraw();
		while (!(kb_Data[6] & kb_Clear))
			kb_Scan();
		gfx_End();
		return 1;
	}

	ChunkMenuItem* items = NULL;
	uint16_t item_count = 0;
	if (!build_chunk_menu(&idx, &items, &item_count))
	{
		ntx_free_index(&idx);
		gfx_End();
		return 1;
	}

	int sel = 0;
	bool prev_up = false;
	bool prev_down = false;
	bool prev_enter = false;
	bool prev_clear = false;

	while (true)
	{
		draw_chunk_menu(&idx, items, item_count, sel);
		kb_Scan();

		bool now_up = (kb_Data[7] & kb_Up) != 0;
		bool now_down = (kb_Data[7] & kb_Down) != 0;
		bool now_enter = (kb_Data[6] & kb_Enter) != 0;
		bool now_clear = (kb_Data[6] & kb_Clear) != 0;

		bool up_press = now_up && !prev_up;
		bool down_press = now_down && !prev_down;
		bool enter_press = now_enter && !prev_enter;
		bool clear_press = now_clear && !prev_clear;

		prev_up = now_up;
		prev_down = now_down;
		prev_enter = now_enter;
		prev_clear = now_clear;

		if (clear_press)
			break;
		if (up_press && sel > 0)
			sel--;
		if (down_press && sel < (int)item_count - 1)
			sel++;
		if (enter_press && item_count > 0)
		{
			const ChunkMenuItem* mi = &items[sel];
			const NtxNoteEntry* note = &idx.entries[mi->note_index];
			view_chunk_tex(note, mi->chunk_index, renderer);
			wait_for_nav_key_release();

			prev_up = false;
			prev_down = false;
			prev_enter = false;
			prev_clear = false;
		}
	}

	free(items);
	ntx_free_index(&idx);
	tex_renderer_destroy(renderer);
	gfx_End();
	return 0;
}
