#include "font.h"
#include "os.h"
#include "memory.h"
#include "draw.h"

#include <assert.h>
#include <stdlib.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include <stb/stb_rect_pack.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb/stb_truetype.h>

struct Bitmap {
	s32 width, height;
	u8* data;
};

Font* current_font = nullptr;

u64 round_up_pow2(u64 x) {
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    x++;
    return x;
}

Font font_load_from_os(const char* file_name) {
    
    String font_data = {};
    {
        const String current_path = os_get_path();
	    os_set_path_to_fonts(); 

        font_data = os_load_file_into_memory(file_name);
	    assert(font_data.data != NULL);

	    os_set_path(current_path);
    }

    Font font = {};
	stbtt_InitFont(&font.info, font_data.data, stbtt_GetFontOffsetForIndex(font_data.data, 0));
    
    font.num_glyphs = font.info.numGlyphs;// @Temporary: info is opaque, but we are peeking :)
    
    font.codepoints = (int*) c_alloc(font.num_glyphs * sizeof(int));
    memset(font.codepoints, 0, font.num_glyphs * sizeof(int));
    {
        u32 glyphs_found = 0;
        // Ask STBTT for the glyph indices.
        // @Temporary: linearly search the codepoint space because STBTT doesn't expose CP->glyph idx;
        //             later we will parse the ttf file in a similar way to STBTT.
        //             Linear search is exactly 17 times slower than parsing for 65536 glyphs.
        for (int codepoint = 0; codepoint < 0x110000; codepoint++) {
            int idx = stbtt_FindGlyphIndex(&font.info, codepoint);
            if (idx <= 0) continue;
            glyphs_found++;
            font.codepoints[idx] = codepoint;
        }
        //if (glyphs_found < result.num_glyphs) {
        //
        //}
    }

    double atlas_area = 0;
    {
        int x0 = 0;
        int x1 = 0;
        int y0 = 0;
        int y1 = 0;


        for (int i = 0; (u32)i < font.num_glyphs; i++) {
            stbtt_GetGlyphBox(&font.info, i, &x0, &y0, &x1, &y1);

            double w = (x1 - x0);
            double h = (y1 - y0);

            atlas_area += w * h;
        }
    }
    font.atlas_area = atlas_area;

    glGenTextures(ARRAYSIZE(font.atlas_ids), font.atlas_ids);
    
    return font;
}

void font_free(Font& font) {
    
    c_free(font.codepoints);

}

void font_pack_atlas(Font& font) {

    if (font.size < 2) font.size = 2;
    if (font.size > 128) font.size = 128;

    const float font_scale = stbtt_ScaleForPixelHeight(&font.info, font.size);

    if (!font.atlases[font.size].w) {

	    u32 h_oversample = 1; // @NOTE(Phillip): On a low DPI display this looks almost as good to me.
	    u32 v_oversample = 1; //                 The jump from 1x to 4x is way bigger than 4x to 64x.

        if (font.size <= 36) {
            h_oversample = 2;
            v_oversample = 2;
        }
        if (font.size <= 12) {
            h_oversample = 4;
            v_oversample = 4;
        }
        if (font.size <= 8) {
            h_oversample = 8;
            v_oversample = 8;
        }

	    Bitmap atlas;

        if (font.size <= 12) {
            atlas.width = 512 * h_oversample;
            atlas.height = 512 * v_oversample;
        } else {
            double area = font.atlas_area * h_oversample * v_oversample;
            area *= 1.0 + 1 / (sqrt(font.size)); // fudge factor for small sizes

            area *= font_scale * font_scale;

            float root = (float)sqrt(area);
        
            u32 atlas_dimension = (u32)root;
            atlas_dimension = (atlas_dimension + 127) & ~127;
            //atlas_dimension = round_up_pow2(atlas_dimension);

            atlas.width = atlas_dimension;
            atlas.height = atlas_dimension;
        }
        font.atlases[font.size].w = atlas.width;
        font.atlases[font.size].h = atlas.height;

	    atlas.data = (u8*) c_alloc(atlas.width * atlas.height);
        defer(c_free(atlas.data));

        stbtt_pack_context pc;
	    stbtt_packedchar* pdata = (stbtt_packedchar*) c_alloc(font.num_glyphs * sizeof(stbtt_packedchar));
        stbtt_pack_range pr;

	    stbtt_PackBegin(&pc, atlas.data, atlas.width, atlas.height, 0, 1, NULL);
	    pr.chardata_for_range = pdata;
	    pr.array_of_unicode_codepoints = font.codepoints;
        pr.first_unicode_codepoint_in_range = 0;
	    pr.num_chars = font.num_glyphs;
	    pr.font_size = font.size;

        stbtt_PackSetSkipMissingCodepoints(&pc, 1);
	    stbtt_PackSetOversampling(&pc, h_oversample, v_oversample);
	    stbtt_PackFontRanges(&pc, font.info.data, 0, &pr, 1);
	    stbtt_PackEnd(&pc);

	    glBindTexture(GL_TEXTURE_2D, font.atlas_ids[font.size]);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, atlas.width, atlas.height, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data);

        font.atlases[font.size].glyphs = (Font_Glyph*) c_alloc(font.num_glyphs * sizeof(Font_Glyph));
	    for (u32 i = 0; i < font.num_glyphs; i++) {
	    	Font_Glyph* glyph = &font.atlases[font.size].glyphs[i];

	    	glyph->x0 = pdata[i].x0;
	    	glyph->x1 = pdata[i].x1;
	    	glyph->y0 = pdata[i].y0;
	    	glyph->y1 = pdata[i].y1;

	    	glyph->width = ((f32)pdata[i].x1 - pdata[i].x0) / (float)h_oversample;
	    	glyph->height = ((f32)pdata[i].y1 - pdata[i].y0) / (float)v_oversample;
	    	glyph->bearing_x = pdata[i].xoff;
	    	glyph->bearing_y = pdata[i].yoff;
	    	glyph->advance = pdata[i].xadvance;
	    }

        c_free(pdata);

	    glBindTexture(GL_TEXTURE_2D, 0);
    }

	int ascent, descent, line_gap;
	stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &line_gap);

	font.ascent = (float)ascent * font_scale;
	font.descent = (float)descent * font_scale;
	font.line_gap = (float)line_gap * font_scale;

    // Don't free the data, since result.info needs it.

	bind_font(&font);
}

const Font_Glyph* font_find_glyph(const Font* font, u32 c) {
    int idx = stbtt_FindGlyphIndex(&font->info, c);
    if (idx > 0) {
        assert((u32)idx < font->num_glyphs);
        return &font->atlases[font->size].glyphs[idx];
    } else {
        return nullptr;
    }
}
