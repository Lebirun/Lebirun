#ifndef _LEBIRUN_PSF_H
#define _LEBIRUN_PSF_H

#include <stdint.h>
#include <stddef.h>

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04
#define PSF1_MODE512 0x01
#define PSF1_MODEHASTAB 0x02
#define PSF1_MODEHASSEQ 0x04

#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xb5
#define PSF2_MAGIC2 0x4a
#define PSF2_MAGIC3 0x86

#define PSF2_HAS_UNICODE_TABLE 0x01

typedef struct {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
} __attribute__((packed)) psf1_header_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t numglyph;
    uint32_t bytesperglyph;
    uint32_t height;
    uint32_t width;
} __attribute__((packed)) psf2_header_t;

typedef struct {
    uint64_t version;
    uint64_t width;
    uint64_t height;
    uint64_t bytesperglyph;
    uint64_t numglyph;
    uint8_t *glyphs;
    uint16_t *unicode_table;
    uint64_t unicode_table_size;
    uint8_t owns_data;
} psf_font_t; 

int psf_load(const void *data, size_t size, psf_font_t *font);
const uint8_t *psf_get_glyph(psf_font_t *font, uint64_t codepoint);
void psf_free(psf_font_t *font);

#endif
