/***********************************************************************
**
**  REBOL [R3] Language Interpreter and Run-time Environment
**
**  Copyright 2012 REBOL Technologies
**  REBOL is a trademark of REBOL Technologies
**
**  Additional code modifications and improvements Copyright 2012 Saphirion AG
**
**  Licensed under the Apache License, Version 2.0 (the "License");
**  you may not use this file except in compliance with the License.
**  You may obtain a copy of the License at
**
**  http://www.apache.org/licenses/LICENSE-2.0
**
**  Unless required by applicable law or agreed to in writing, software
**  distributed under the License is distributed on an "AS IS" BASIS,
**  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**  See the License for the specific language governing permissions and
**  limitations under the License.
**
************************************************************************
**
**  Title: TEXT dialect API functions
**  Author: Richard Smolak
**
************************************************************************
**
**  NOTE to PROGRAMMERS:
**
**    1. Keep code clear and simple.
**    2. Document unusual code, reasoning, or gotchas.
**    3. Use same style for code, vars, indent(4), comments, etc.
**    4. Keep in mind Linux, OS X, BSD, big/little endian CPUs.
**    5. Test everything, then test it again.
**
***********************************************************************/

#include "../agg/agg_graphics.h"

extern "C" {
#include "host-view.h"
#include "host-renderer.h"
#include "host-draw-api.h"
#include "host-draw-api-agg.h"
#include "host-text-api.h"
#include "host-text-api-agg.h"
}

#include "../agg/agg_truetype_text.h"
using namespace agg;

extern "C" void* Rich_Text;

extern "C" REBOOL As_OS_Str(REBSER *series, REBCHR **string);
extern "C" REBOOL As_UTF32_Str(REBSER *series, REBCHR **string);

extern "C" void agg_rt_block_text(void *richtext, REBSER *block)
{
	REBCEC ctx;

	ctx.envr = richtext;
	ctx.block = block;
	ctx.index = 0;

	RL_DO_COMMANDS(block, 0, &ctx);
}

extern "C" REBINT agg_rt_gob_text(REBGOB *gob, REBDRW_CTX *draw_ctx, REBXYI abs_oft, REBXYI clip_oft, REBXYI clip_siz)
{
	if (GET_GOB_FLAG(gob, GOBF_WINDOW)) return 0; //don't render window title text

	REBYTE *buf;
	REBXYI buf_size;
	buf = (REBYTE*)draw_ctx->surface->pixels;
	buf_size.x = draw_ctx->surface->w;
	buf_size.y = draw_ctx->surface->h;

	agg_graphics::ren_buf rbuf_win(buf, buf_size.x, buf_size.y, buf_size.x << 2);		
	agg_graphics::pixfmt pixf_win(rbuf_win);
	agg_graphics::ren_base rb_win(pixf_win);
	rich_text* rt = (rich_text*)Rich_Text;
	REBINT w = GOB_LOG_W_INT(gob);	
	REBINT h = GOB_LOG_H_INT(gob);
	
	rt->rt_reset();
	rt->rt_attach_buffer(&rbuf_win, buf_size.x, buf_size.y);
	//note: rt_set_clip() include bottom-right values
//		rt->rt_set_clip(abs_oft.x, abs_oft.y, abs_oft.x+w, abs_oft.y+h, w, h);
	rt->rt_set_clip(clip_oft.x, clip_oft.y, clip_siz.x, clip_siz.y, w, h);

	if (GOB_TYPE(gob) == GOBT_TEXT)
		agg_rt_block_text(rt, (REBSER *)GOB_CONTENT(gob));
	else {
		REBCHR* str;
#ifdef TO_WIN32
		//Windows uses UTF16 wide chars
		REBOOL dealloc = As_OS_Str(GOB_CONTENT(gob), (REBCHR**)&str);
#else
		//linux, android use UTF32 wide chars
		REBOOL dealloc = As_UTF32_Str(GOB_CONTENT(gob), (REBCHR**)&str);
#endif
		if (str){
			rt->rt_set_text(str, dealloc);
			rt->rt_push(1);
		}
	}
	
	REBXYF oft = { abs_oft.x, abs_oft.y };
	return rt->rt_draw_text(DRAW_TEXT, &oft);
}

extern "C" void* agg_create_rich_text()
{
#ifdef AGG_WIN32_FONTS
	return (void*)new rich_text(GetDC( NULL ));
#endif
#ifdef AGG_FREETYPE
return (void*)new rich_text();
#endif
}

extern "C" void agg_destroy_rich_text(void* rt)
{
   delete (rich_text*)rt;
}
extern "C" int agg_rt_init(REBRDR_TXT *txt)
{
	txt->rich_text = Rich_Text = agg_create_rich_text();
	if (txt->rich_text) return 0;

	return -1;
}

extern "C" void agg_rt_fini(REBRDR_TXT *txt)
{
	if (!txt) return;
	agg_destroy_rich_text(txt->rich_text);
}

extern "C" void agg_rt_anti_alias(void* rt, REBINT mode)
{
	((rich_text*)rt)->rt_text_mode(mode);
}

extern "C" void agg_rt_bold(void* rt, REBINT state)
{
	font* font = ((rich_text*)rt)->rt_get_font();
	font->bold = state;
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_caret(void* rt, REBXYF* caret, REBXYF* highlightStart, REBXYF highlightEnd)
{
	if (highlightStart) ((rich_text*)rt)->rt_set_hinfo(*highlightStart,highlightEnd);
	if (caret) ((rich_text*)rt)->rt_set_caret(*caret);
}

extern "C" void agg_rt_center(void* rt)
{
	para* par = ((rich_text*)rt)->rt_get_para();
	par->align = W_TEXT_CENTER;
	((rich_text*)rt)->rt_set_para(par);
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_color(void* rt, REBCNT color)
{
	font* font = ((rich_text*)rt)->rt_get_font();
	font->color[0] = ((REBYTE*)&color)[0];
	font->color[1] = ((REBYTE*)&color)[1];
	font->color[2] = ((REBYTE*)&color)[2];
	font->color[3] = ((REBYTE*)&color)[3];
	((rich_text*)rt)->rt_push();
	((rich_text*)rt)->rt_color_change();
}

extern "C" void agg_rt_drop(void* rt, REBINT number)
{
	((rich_text*)rt)->rt_drop(number);
}

extern "C" void agg_rt_font(void* rt, void* fnt)
{
	((rich_text*)rt)->rt_set_font((font*)fnt);
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_font_size(void* rt, REBINT size)
{
	font* font = ((rich_text*)rt)->rt_get_font();
	font->size = size;
	((rich_text*)rt)->rt_push();
}

extern "C" void* agg_rt_get_font(void* rt)
{
	return (void*)((rich_text*)rt)->rt_get_font();
}


extern "C" void* agg_rt_get_para(void* rt)
{
	return (void*)((rich_text*)rt)->rt_get_para();
}

extern "C" void agg_rt_italic(void* rt, REBINT state)
{
	font* font = ((rich_text*)rt)->rt_get_font();
	font->italic = state;
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_left(void* rt)
{
	para* par = ((rich_text*)rt)->rt_get_para();
	par->align = W_TEXT_LEFT;
	((rich_text*)rt)->rt_set_para(par);
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_newline(void* rt, REBINT index)
{
	((rich_text*)rt)->rt_set_text((REBCHR*)"\n", TRUE);
	((rich_text*)rt)->rt_push(index);
}

extern "C" void agg_rt_para(void* rt, void* pra)
{
	((rich_text*)rt)->rt_set_para((para*)pra);
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_right(void* rt)
{
	para* par = ((rich_text*)rt)->rt_get_para();
	par->align = W_TEXT_RIGHT;
	((rich_text*)rt)->rt_set_para(par);
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_scroll(void* rt, REBXYF offset)
{
	para* par = ((rich_text*)rt)->rt_get_para();
	par->scroll_x = offset.x;
	par->scroll_y = offset.y;
	((rich_text*)rt)->rt_set_para(par);
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_shadow(void* rt, REBXYF d, REBCNT color, REBINT blur)
{
	font* font = ((rich_text*)rt)->rt_get_font();

	font->shadow_x = ROUND_TO_INT(d.x);
	font->shadow_y = ROUND_TO_INT(d.y);
	font->shadow_blur = blur;

	memcpy(font->shadow_color, (REBYTE*)&color, 4);

	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_set_font_styles(void* fnt_, u32 word){
	font *fnt = (font*)fnt_;
switch (word){
    case W_TEXT_BOLD:
	fnt->bold = TRUE;
	break;
    case W_TEXT_ITALIC:
	fnt->italic = TRUE;
	break;
    case W_TEXT_UNDERLINE:
	fnt->underline = TRUE;
	break;
			
		default:
			fnt->bold = FALSE;
			fnt->italic = FALSE;
			fnt->underline = FALSE;
			break;
}
}

extern "C" void agg_rt_size_text(void* rt, REBGOB* gob, REBXYF* size)
{
REBCHR* str;
REBOOL dealloc;
	((rich_text*)rt)->rt_reset();
	((rich_text*)rt)->rt_set_clip(0,0, GOB_LOG_W_INT(gob),GOB_LOG_H_INT(gob));
	if (GOB_TYPE(gob) == GOBT_TEXT){
		agg_rt_block_text(rt, (REBSER *)GOB_CONTENT(gob));
	} else if (GOB_TYPE(gob) == GOBT_STRING) {
#ifdef TO_WIN32
		//Windows uses UTF16 wide chars
		dealloc = As_OS_Str(GOB_CONTENT(gob), (REBCHR**)&str);
#else
		//linux, android use UTF32 wide chars
		dealloc = As_UTF32_Str(GOB_CONTENT(gob), (REBCHR**)&str);
#endif
		
    ((rich_text*)rt)->rt_set_text(str, dealloc);
    ((rich_text*)rt)->rt_push(1);
	} else {
		size->x = 0;
		size->y = 0;
		return;
	}

	((rich_text*)rt)->rt_size_text(size);
}

extern "C" void agg_rt_text(void* rt, REBSER* text, REBINT index)
{
	REBCHR* str;
#ifdef TO_WINDOWS
	//Windows uses UTF16 wide chars
	REBOOL dealloc = As_OS_Str(text, &str);
#else
	//linux, android use UTF32 wide chars
	REBOOL dealloc = As_UTF32_Str(text, &str);
#endif		
((rich_text*)rt)->rt_set_text(str, dealloc);
((rich_text*)rt)->rt_push(index);
}

extern "C" void agg_rt_underline(void* rt, REBINT state)
{
	font* font = ((rich_text*)rt)->rt_get_font();
	font->underline = state;
	((rich_text*)rt)->rt_push();
}

extern "C" void agg_rt_offset_to_caret(void* rt, REBGOB *gob, REBXYF xy, REBINT *element, REBINT *position)
{
REBCHR* str;
REBOOL dealloc;

	((rich_text*)rt)->rt_reset();
	((rich_text*)rt)->rt_set_clip(0,0, GOB_LOG_W_INT(gob),GOB_LOG_H_INT(gob));
	if (GOB_TYPE(gob) == GOBT_TEXT){
		agg_rt_block_text(rt, (REBSER *)GOB_CONTENT(gob));
	} else if (GOB_TYPE(gob) == GOBT_STRING) {
#ifdef TO_WIN32
		//Windows uses UTF16 wide chars
		dealloc = As_OS_Str(GOB_CONTENT(gob), (REBCHR**)&str);
#else
		//linux, android use UTF32 wide chars
		dealloc = As_UTF32_Str(GOB_CONTENT(gob), (REBCHR**)&str);
#endif
    ((rich_text*)rt)->rt_set_text(str, dealloc);
    ((rich_text*)rt)->rt_push(1);
	} else {
		*element = 0;
		*position = 0;
		return;
	}

	((rich_text*)rt)->rt_offset_to_caret(xy, element, position);
}

extern "C" void agg_rt_caret_to_offset(void* rt, REBGOB *gob, REBXYF* xy, REBINT element, REBINT position)
{
REBCHR* str;
REBOOL dealloc;
	((rich_text*)rt)->rt_reset();
	((rich_text*)rt)->rt_set_clip(0,0, GOB_LOG_W_INT(gob),GOB_LOG_H_INT(gob));
	if (GOB_TYPE(gob) == GOBT_TEXT){
		agg_rt_block_text(rt, (REBSER *)GOB_CONTENT(gob));
	} else if (GOB_TYPE(gob) == GOBT_STRING) {
#ifdef TO_WIN32
		//Windows uses UTF16 wide chars
		dealloc = As_OS_Str(GOB_CONTENT(gob), (REBCHR**)&str);
#else
		//linux, android use UTF32 wide chars
		dealloc = As_UTF32_Str(GOB_CONTENT(gob), (REBCHR**)&str);
#endif
		
    ((rich_text*)rt)->rt_set_text(str, dealloc);
    ((rich_text*)rt)->rt_push(1);
	} else {
		xy->x = 0;
		xy->y = 0;
		return;
	}

	((rich_text*)rt)->rt_caret_to_offset(xy, element, position);
}
