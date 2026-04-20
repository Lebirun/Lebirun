#include <lebirun/psf.h>
#include <lebirun/mem_map.h>
#include <string.h>
#include <stdio.h>

static int psf1_load(const void *data, size_t size, psf_font_t *font) {
    const psf1_header_t *hdr = (const psf1_header_t *)data;
    
    if (size < sizeof(psf1_header_t)) {
        return -1;
    }
    
    if (hdr->magic[0] != PSF1_MAGIC0 || hdr->magic[1] != PSF1_MAGIC1) {
        return -1;
    }
    
    font->version = 1;
    font->width = 8;
    font->height = hdr->charsize;
    font->bytesperglyph = hdr->charsize;
    font->numglyph = (hdr->mode & PSF1_MODE512) ? 512 : 256;
    
    size_t glyph_data_size = font->numglyph * font->bytesperglyph;
    if (size < sizeof(psf1_header_t) + glyph_data_size) {
        return -1;
    }
    
    font->glyphs = (uint8_t *)data + sizeof(psf1_header_t);
    font->owns_data = 0;
    
    if (hdr->mode & PSF1_MODEHASTAB) {
        font->unicode_table = (uint16_t *)((uint8_t *)data + sizeof(psf1_header_t) + glyph_data_size);
        font->unicode_table_size = size - sizeof(psf1_header_t) - glyph_data_size;
    } else {
        font->unicode_table = 0;
        font->unicode_table_size = 0;
    }
    
    return 0;
}

static int psf2_load(const void *data, size_t size, psf_font_t *font) {
    const psf2_header_t *hdr = (const psf2_header_t *)data;
    
    if (size < sizeof(psf2_header_t)) {
        return -1;
    }
    
    uint8_t *magic = (uint8_t *)&hdr->magic;
    if (magic[0] != PSF2_MAGIC0 || magic[1] != PSF2_MAGIC1 ||
        magic[2] != PSF2_MAGIC2 || magic[3] != PSF2_MAGIC3) {
        return -1;
    }
    
    font->version = 2;
    font->width = hdr->width;
    font->height = hdr->height;
    font->bytesperglyph = hdr->bytesperglyph;
    font->numglyph = hdr->numglyph;
    
    size_t glyph_data_size = font->numglyph * font->bytesperglyph;
    if (size < hdr->headersize + glyph_data_size) {
        return -1;
    }
    
    font->glyphs = (uint8_t *)data + hdr->headersize;
    font->owns_data = 0;
    
    if (hdr->flags & PSF2_HAS_UNICODE_TABLE) {
        font->unicode_table = (uint16_t *)((uint8_t *)data + hdr->headersize + glyph_data_size);
        font->unicode_table_size = size - hdr->headersize - glyph_data_size;
    } else {
        font->unicode_table = 0;
        font->unicode_table_size = 0;
    }
    
    return 0; 
}

int psf_load(const void *data, size_t size, psf_font_t *font) {
    if (!data || size < 4 || !font) {
        return -1;
    }
    
    memset(font, 0, sizeof(psf_font_t));
    
    const uint8_t *bytes = (const uint8_t *)data;
    printf("psf_load: magic=%02X%02X%02X%02X size=%u\n", bytes[0], bytes[1], bytes[2], bytes[3], (unsigned)size);
    
    if (bytes[0] == PSF2_MAGIC0 && bytes[1] == PSF2_MAGIC1 &&
        bytes[2] == PSF2_MAGIC2 && bytes[3] == PSF2_MAGIC3) {
        int r = psf2_load(data, size, font);
        if (r == 0) printf("psf_load: PSF2 header ok width=%u height=%u glyphs=%u bytes/glyph=%u\n", font->width, font->height, font->numglyph, font->bytesperglyph);
        return r;
    }
    
    if (bytes[0] == PSF1_MAGIC0 && bytes[1] == PSF1_MAGIC1) {
        int r = psf1_load(data, size, font);
        if (r == 0) printf("psf_load: PSF1 header ok height=%u glyphs=%u\n", font->height, font->numglyph);
        return r;
    }
    
    return -1;
}

const uint8_t *psf_get_glyph(psf_font_t *font, uint64_t codepoint) {
    if (!font || !font->glyphs) {
        return 0;
    }
    
    if (codepoint >= font->numglyph) {
        codepoint = 0;
    }
    
    return font->glyphs + (codepoint * font->bytesperglyph);
}

void psf_free(psf_font_t *font) {
    if (font) {
        if (font->owns_data) {
            if (font->glyphs) {
                kfree(font->glyphs);
                font->glyphs = 0;
            }
            if (font->unicode_table) {
                kfree(font->unicode_table);
                font->unicode_table = 0;
            }
        }
    }
}
