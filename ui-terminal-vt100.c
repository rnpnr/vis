/* This file is included from ui-terminal.c
 *
 * The goal is *not* to reimplement curses. Instead we aim to provide the
 * simplest possible drawing backend for VT-100 compatible terminals.
 * This is useful for debugging and fuzzing purposes as well as for environments
 * with no curses support.
 *
 * The following terminal escape sequences are used:
 *
 *  - CSI ? 1049 h             Save cursor and use Alternate Screen Buffer (DECSET)
 *  - CSI ? 1049 l             Use Normal Screen Buffer and restore cursor (DECRST)
 *  - CSI ? 25 l               Hide Cursor (DECTCEM)
 *  - CSI ? 25 h               Show Cursor (DECTCEM)
 *  - CSI 2 J                  Erase in Display (ED)
 *  - CSI row ; column H       Cursor Position (CUP)
 *  - CSI ... m                Character Attributes (SGR)
 *    - CSI 0 m                     Normal
 *    - CSI 1 m                     Bold
 *    - CSI 3 m                     Italicized
 *    - CSI 4 m                     Underlined
 *    - CSI 5 m                     Blink
 *    - CSI 7 m                     Inverse
 *    - CSI 22 m                    Normal (not bold)
 *    - CSI 23 m                    Not italicized
 *    - CSI 24 m                    Not underlined
 *    - CSI 25 m                    Not blinking
 *    - CSI 27 m                    Not inverse
 *    - CSI 38 ; 2 ; R ; G ; B m    Set RGB foreground color
 *    - CSI 38 ; 5 ; I m            Set indexed foreground color
 *    - CSI 48 ; 2 ; R ; G ; B m    Set RGB background color
 *    - CSI 48 ; 5 ; I m            Set indexed background color
 *
 * See http://invisible-island.net/xterm/ctlseqs/ctlseqs.txt
 * for further information.
 */

#ifdef __AVX512F__
#include <immintrin.h>
#endif

#define UI_TERMKEY_FLAGS 0

VIS_INTERNAL VisCellStyle
vis_ui_backend_style_default(Ui *ui)
{
	VisCellStyle result = {0};
	result.properties |= (VisCellProperty_IndexedFG|VisCellProperty_IndexedBG);
	result.properties |= (VisCellProperty_FGSet|VisCellProperty_BGSet);
	VisCellStyleBGIndexSet(&result, 0);
	VisCellStyleFGIndexSet(&result, 7);
	return result;
}

VIS_INTERNAL VisTerminalStyle
vis_terminal_style_fg(VisCellStyle a)
{
	VisTerminalStyle result = {0};
	result.indexed = (a.properties & VisCellProperty_IndexedFG) != 0;
	if (result.indexed) {
		result.color.index = VisCellStyleFGIndexGet(&a);
	} else {
		result.color.rgb.r = a.fg_r;
		result.color.rgb.g = a.fg_g;
		result.color.rgb.b = a.fg_b;
	}
	return result;
}

VIS_INTERNAL VisTerminalStyle
vis_terminal_style_bg(VisCellStyle a)
{
	VisTerminalStyle result = {0};
	result.indexed = (a.properties & VisCellProperty_IndexedBG) != 0;
	if (result.indexed) {
		result.color.index = VisCellStyleBGIndexGet(&a);
	} else {
		result.color.rgb.r = a.bg_r;
		result.color.rgb.g = a.bg_g;
		result.color.rgb.b = a.bg_b;
	}
	return result;
}

VIS_INTERNAL bool
vis_terminal_style_equal(VisTerminalStyle a, VisTerminalStyle b)
{
	bool result = a.indexed == b.indexed &&
	              a.color.rgb.r == b.color.rgb.r &&
	              a.color.rgb.g == b.color.rgb.g &&
	              a.color.rgb.b == b.color.rgb.b;
	return result;
}

VIS_INTERNAL VisTerminalStyle
vis_terminal_style_rgb(Vis *vis, u8 r, u8 g, u8 b)
{
	VisTerminalStyle result = {0};
	result.color.rgb.r = r;
	result.color.rgb.g = g;
	result.color.rgb.b = b;
	return result;
}

VIS_INTERNAL VisTerminalStyle
vis_terminal_style_indexed(u16 index)
{
	VisTerminalStyle result = {0};
	result.indexed     = true;
	result.color.index = index;
	return result;
}

VIS_INTERNAL void
vis_ui_vt100_output(str8 s)
{
	// TODO(rnp): eintr
	(void)write(STDERR_FILENO, s.data, s.length);
}

VIS_INTERNAL void
vis_ui_vt100_altscreen(bool enable)
{
	vis_ui_vt100_output(enable ? str8("\x1b[?1049h") : str8("\x1b[?1049l" "\x1b[0m"));
}

VIS_INTERNAL void
vis_ui_vt100_immediate_clear(void)
{
	vis_ui_vt100_output(str8("\x1b[J"));
}

VIS_INTERNAL void
vis_ui_vt100_cursor_visible(bool visible)
{
	vis_ui_vt100_output(visible ? str8("\x1b[?25h") : str8("\x1b[?25l"));
}

#ifndef __AVX512F__
VIS_INTERNAL bool
vis_cell_equal(VisCell *_a, VisCell *_b)
{
	// static_assert(sizeof(VisCell) % 8 == 0, "");
	u64 a[sizeof(VisCell) / sizeof(u64)], b[sizeof(VisCell) / sizeof(u64)];
	memory_copy(a, _a, sizeof(VisCell));
	memory_copy(b, _b, sizeof(VisCell));
	bool result = true;
	for (u64 i = 0; i < countof(a); i++)
		result &= a[i] == b[i];
	return result;
}
#endif

VIS_INTERNAL void
ui_term_backend_blit(Ui *ui)
{
	VisVT100UI *vt  = &ui->vt100;
	Buffer     *buf = &vt->output_buffer;
	buf->length = 0;

	s32 cell_count = ui->width * ui->height;

	if unlikely(vt->flush_terminal) {
		u64 cells_size = round_up_to(cell_count * sizeof(VisCell), 64);
		memset(vt->cell_buffer.cells, 0, cells_size);
		vt->flush_terminal = false;
		vis_ui_vt100_immediate_clear();
	}

	// NOTE(rnp): clear dirty cell bits
	memset(vt->cell_buffer.dirty_cell_bits, 0, cell_count / 8 + 1);

	// NOTE(rnp): compute dirty cells

	#ifdef __AVX512F__
	// NOTE(rnp): don't need to do any masking or cleanup, cell array size
	// was rounded up to 64 bytes.
	for (s32 cell_index = 0; cell_index < cell_count; cell_index += 8) {
		__m512i fb, bb, hi, lo, test;
		fb = _mm512_loadu_epi64(vt->cell_buffer.cells + cell_index + 0);
		bb = _mm512_loadu_epi64(ui->cell_buffer.cells + cell_index + 0);
		lo = _mm512_xor_epi64(fb, bb);

		fb = _mm512_loadu_epi64(vt->cell_buffer.cells + cell_index + 4);
		bb = _mm512_loadu_epi64(ui->cell_buffer.cells + cell_index + 4);
		hi = _mm512_xor_epi64(fb, bb);

		// NOTE(rnp): xor leaves bits set when data is not equal which is what we want;
		// however, we need to compare 16 byte values not 8 byte values. Shuffle upper
		// portion of 128 bit lanes down and or with lower portion leaving bits set
		// when either upper or lower 8 bytes are not equal.
		lo = _mm512_or_epi64(lo, _mm512_shuffle_epi32(lo, 0x0e));
		hi = _mm512_or_epi64(hi, _mm512_shuffle_epi32(hi, 0x0e));

		// NOTE(rnp): pack lower half of 128 bit results into lower 32 bytes of register
		lo = _mm512_mask_compress_epi64(lo, 0x55, lo);
		hi = _mm512_mask_compress_epi64(hi, 0x55, hi);

		// NOTE(rnp): mix lower 4 lanes of lo with lower 4 lanes of hi.
		// lo goes to lower 4 lanes of test and hi goes to upper 4 lanes
		test = _mm512_inserti64x4(lo, _mm512_extracti64x4_epi64(hi, 0), 1);

		// NOTE(rnp): test which lanes are non zero (cells not equal) and store the result
		vt->cell_buffer.dirty_cell_bits[cell_index / 8] = _mm512_test_epi64_mask(test, test);
	}

	#else

	for (s32 cell_index = 0; cell_index < cell_count; cell_index++) {
		if (!vis_cell_equal(vt->cell_buffer.cells + cell_index, ui->cell_buffer.cells + cell_index)) {
			s32 bin = cell_index / 8;
			s32 bit = cell_index % 8;
			vt->cell_buffer.dirty_cell_bits[bin] |= 1 << bit;
		}
	}

	#endif

	// NOTE(rnp): prepare output buffer
	VisTerminalStyle fg = {0};
	VisTerminalStyle bg = {0};
	u8 attributes = 0;

	// NOTE(rnp): reposition cursor, reset attributes
	str8 command = str8("\x1b[H" "\x1b[0m");
	buffer_append(buf, command.data, command.length);

	for (s32 cell_index = 0, cursor_cell = 0; cell_index < cell_count; cell_index++) {
		s32 bin = cell_index / 8;
		s32 bit = cell_index % 8;
		if (vt->cell_buffer.dirty_cell_bits[bin] & (1 << bit)) {
			VisCell *fb = vt->cell_buffer.cells + cell_index;
			VisCell *bb = ui->cell_buffer.cells + cell_index;
			if (cursor_cell != cell_index) {
				s32 x = cell_index % ui->width;
				s32 y = cell_index / ui->width;
				vis_buffer_appendf(buf, "\x1b[%d;%dH", y + 1, x + 1);
				cursor_cell = cell_index;
			}

			VisCellStyle style = bb->style;
			if (style.attributes != attributes) {
				static const struct {
					u8 flag;
					char on[2], off[4];
				} cell_attrs[] = {
					{VisCellAttribute_Bold,      "1", "22"},
					{VisCellAttribute_Dim,       "2", "22"},
					{VisCellAttribute_Italic,    "3", "23"},
					{VisCellAttribute_Underline, "4", "24"},
					{VisCellAttribute_Blink,     "5", "25"},
					{VisCellAttribute_Reverse,   "7", "27"},
				};

				for (u64 i = 0; i < countof(cell_attrs); i++) {
					u8 flag = cell_attrs[i].flag;
					if ((attributes & flag) != (style.attributes & flag)) {
						vis_buffer_appendf(buf, "\x1b[%sm", (style.attributes & flag) ?
						                   cell_attrs[i].on :
						                   cell_attrs[i].off);
					}
				}
				attributes = style.attributes;
			}

			VisTerminalStyle style_fg = vis_terminal_style_fg(style);
			if (!vis_terminal_style_equal(fg, style_fg)) {
				if (style_fg.indexed) {
					vis_buffer_appendf(buf, "\x1b[38;5;%um", (u32)style_fg.color.index);
				} else {
					vis_buffer_appendf(buf, "\x1b[38;2;%u;%u;%um", (u32)style_fg.color.rgb.r,
					                   (u32)style_fg.color.rgb.g, (u32)style_fg.color.rgb.b);
				}
				fg = style_fg;
			}

			VisTerminalStyle style_bg = vis_terminal_style_bg(style);
			if (!vis_terminal_style_equal(bg, style_bg)) {
				if (style_bg.indexed) {
					vis_buffer_appendf(buf, "\x1b[48;5;%um", (u32)style_bg.color.index);
				} else {
					vis_buffer_appendf(buf, "\x1b[48;2;%u;%u;%um", (u32)style_bg.color.rgb.r,
					                   (u32)style_bg.color.rgb.g, (u32)style_bg.color.rgb.b);
				}
				bg = style_bg;
			}

			buffer_append(buf, bb->data, bb->data_length);
			memory_copy(fb, bb, sizeof(*fb));

			// NOTE(rnp): anytime we print a character the terminal's cursor advances by the cell width
			cursor_cell += fb->width;
		}
	}

	// NOTE(rnp): maintain cursor position in case it gets queried through escape codes
	vis_buffer_appendf(buf, "\x1b[%d;%dH", ui->cur_row + 1, ui->cur_col + 1);
	vis_ui_vt100_output((str8){.data = (u8 *)buf->data, .length = buf->length});
}

VIS_INTERNAL void ui_term_backend_clear(Ui *ui) {}
VIS_INTERNAL void ui_term_backend_save(Ui *ui, bool fscr) {}

VIS_INTERNAL void
ui_term_backend_restore(Ui *ui)
{
	ui->vt100.flush_terminal = true;
}

VIS_INTERNAL bool
ui_term_backend_resize(Ui *ui, int width, int height)
{
	bool result = vis_cell_buffer_resize(&ui->vt100.cell_buffer, width, height);
	if (result) ui->vt100.flush_terminal = true;
	return result;
}

int ui_terminal_colors(void) {
	char *term = getenv("TERM");
	return (term && strstr(term, "-256color")) ? 256 : 16;
}

VIS_INTERNAL void
ui_term_backend_suspend(Ui *tui)
{
	termkey_stop(&tui->termkey);
	vis_ui_vt100_cursor_visible(true);
	vis_ui_vt100_altscreen(false);
}

VIS_INTERNAL void
ui_terminal_resume(Ui *tui)
{
	vis_ui_vt100_altscreen(true);
	vis_ui_vt100_cursor_visible(false);
	termkey_start(&tui->termkey, UI_TERMKEY_FLAGS);
}

static bool
ui_backend_init(Ui *ui, char *term)
{
	ui_terminal_resume(ui);
	return true;
}

VIS_INTERNAL void
vis_ui_backend_free(Ui *ui)
{
	ui_term_backend_suspend(ui);
	if (ui->vt100.cell_buffer.size) munmap(ui->vt100.cell_buffer.cells, ui->vt100.cell_buffer.size);
	buffer_release(&ui->vt100.output_buffer);
}
