#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <lebirun/mem_map.h>
#include "xz.h"

typedef struct {
    const uint8_t *next;
    const uint8_t *end;
} xz_input_t;

typedef struct {
    xz_input_t input;
    uint32_t range;
    uint32_t code;
    int failed;
    uint16_t *probabilities;
    size_t probability_count;
    uint16_t *match;
    uint16_t *repeat;
    uint16_t *select[3];
    uint16_t *short_repeat;
    uint16_t *slot;
    uint16_t *distance;
    uint16_t *alignment;
    uint16_t *length[2];
    uint16_t *literal;
    unsigned lc;
    unsigned lp;
    unsigned positions;
    unsigned state;
    uint32_t history[4];
    uint64_t dictionary;
    uint8_t *output;
    size_t capacity;
    size_t written;
    size_t dictionary_start;
} xz_decoder_t;

static uint32_t xz_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static uint32_t xz_crc(const uint8_t *data, size_t size)
{
    static const uint32_t nibble[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
    };
    uint32_t value;

    value = UINT32_MAX;
    while (size--) {
        value ^= *data++;
        value = (value >> 4) ^ nibble[value & 15];
        value = (value >> 4) ^ nibble[value & 15];
    }
    return ~value;
}

static int xz_integer(xz_input_t *input, uint64_t *value)
{
    unsigned shift;
    unsigned byte;

    *value = 0;
    for (shift = 0; shift < 63; shift += 7) {
        if (input->next == input->end) return -1;
        byte = *input->next++;
        *value |= (uint64_t)(byte & 127) << shift;
        if (!(byte & 128)) return shift && !byte ? -1 : 0;
    }
    return -1;
}

static inline __attribute__((always_inline)) void
xz_normalize(xz_decoder_t *decoder)
{
    if (decoder->range >= (1u << 24)) return;
    if (decoder->input.next == decoder->input.end) {
        decoder->failed = 1;
        return;
    }
    decoder->range <<= 8;
    decoder->code = (decoder->code << 8) | *decoder->input.next++;
}

static unsigned xz_bit(xz_decoder_t *decoder, uint16_t *probability)
{
    uint32_t boundary;
    uint32_t mask;
    uint32_t value;

    xz_normalize(decoder);
    value = *probability;
    boundary = (decoder->range >> 11) * value;
    mask = 0u - (decoder->code >= boundary);
    decoder->code -= boundary & mask;
    decoder->range = (boundary & ~mask) |
                     ((decoder->range - boundary) & mask);
    *probability = value + (((2048 - value) >> 5) & ~mask) -
                   ((value >> 5) & mask);
    return mask & 1;
}

static uint32_t xz_tree(xz_decoder_t *decoder, uint16_t *probabilities,
                         unsigned bits, int reverse)
{
    uint32_t node;
    uint32_t value;
    unsigned index;
    unsigned bit;

    node = 1;
    value = 0;
    for (index = 0; index < bits; index++) {
        bit = xz_bit(decoder, probabilities + node);
        node = node * 2 + bit;
        if (reverse) value |= (uint32_t)bit << index;
        else value = value * 2 + bit;
    }
    return value;
}

static unsigned xz_length(xz_decoder_t *decoder, unsigned repeat,
                           unsigned position)
{
    uint16_t *probabilities;

    probabilities = decoder->length[repeat];
    if (!xz_bit(decoder, probabilities))
        return 2 + xz_tree(decoder, probabilities + 2 + position * 8, 3, 0);
    if (!xz_bit(decoder, probabilities + 1))
        return 10 + xz_tree(decoder, probabilities + 2 +
                            decoder->positions * 8 + position * 8, 3, 0);
    return 18 + xz_tree(decoder, probabilities + 2 +
                        decoder->positions * 16, 8, 0);
}

static int xz_properties(xz_decoder_t *decoder, unsigned properties)
{
    unsigned lc;
    unsigned lp;
    unsigned positions;
    unsigned index;
    size_t count;
    size_t length_count;
    uint16_t *probabilities;

    if (properties >= 225) return -1;
    lc = properties % 9;
    lp = properties / 9 % 5;
    positions = 1u << (properties / 45);
    if (lc + lp > 4) return -1;
    length_count = 258 + 16 * positions;
    count = 24 * positions + 48 + 256 + 115 + 16 +
            2 * length_count + (768u << (lc + lp));
    if (count != decoder->probability_count) {
        probabilities = kmalloc(count * sizeof(*probabilities));
        if (!probabilities) return -1;
        kfree(decoder->probabilities);
        decoder->probabilities = probabilities;
        decoder->probability_count = count;
    }
    decoder->lc = lc;
    decoder->lp = lp;
    decoder->positions = positions;
    probabilities = decoder->probabilities;
    decoder->match = probabilities;
    probabilities += 12 * positions;
    decoder->repeat = probabilities;
    probabilities += 12;
    for (index = 0; index < 3; index++) {
        decoder->select[index] = probabilities;
        probabilities += 12;
    }
    decoder->short_repeat = probabilities;
    probabilities += 12 * positions;
    decoder->slot = probabilities;
    probabilities += 256;
    decoder->distance = probabilities;
    probabilities += 115;
    decoder->alignment = probabilities;
    probabilities += 16;
    decoder->length[0] = probabilities;
    probabilities += length_count;
    decoder->length[1] = probabilities;
    decoder->literal = probabilities + length_count;
    return 0;
}

static void xz_reset_model(xz_decoder_t *decoder)
{
    size_t index;

    for (index = 0; index < decoder->probability_count; index++)
        decoder->probabilities[index] = 1024;
    decoder->state = 0;
    memset(decoder->history, 0, sizeof(decoder->history));
}

static int xz_distance_valid(xz_decoder_t *decoder, uint64_t distance)
{
    return distance <= decoder->dictionary &&
           distance <= decoder->written - decoder->dictionary_start;
}

static int xz_literal(xz_decoder_t *decoder)
{
    uint16_t *probabilities;
    size_t position;
    unsigned previous;
    unsigned context;
    unsigned symbol;
    unsigned prediction;
    unsigned predicted_bit;
    unsigned bit;

    position = decoder->written - decoder->dictionary_start;
    previous = position ? decoder->output[decoder->written - 1] : 0;
    context = ((position & ((1u << decoder->lp) - 1)) << decoder->lc) |
              (previous >> (8 - decoder->lc));
    probabilities = decoder->literal + context * 768;
    symbol = 1;
    if (decoder->state >= 7) {
        if (!xz_distance_valid(decoder, (uint64_t)decoder->history[0] + 1))
            return -1;
        prediction = decoder->output[decoder->written -
                                      (uint64_t)decoder->history[0] - 1];
        while (symbol < 256) {
            predicted_bit = (prediction >> 7) & 1;
            prediction <<= 1;
            bit = xz_bit(decoder, probabilities +
                          256 + predicted_bit * 256 + symbol);
            symbol = symbol * 2 + bit;
            if (bit != predicted_bit) break;
        }
    }
    while (symbol < 256)
        symbol = symbol * 2 + xz_bit(decoder, probabilities + symbol);
    decoder->output[decoder->written++] = (uint8_t)symbol;
    if (decoder->state < 4) decoder->state = 0;
    else decoder->state -= decoder->state < 10 ? 3 : 6;
    return decoder->failed ? -1 : 0;
}

static uint32_t xz_match_distance(xz_decoder_t *decoder, unsigned length)
{
    uint32_t slot;
    uint32_t distance;
    uint32_t direct;
    unsigned bits;
    unsigned index;

    index = length - 2;
    if (index > 3) index = 3;
    slot = xz_tree(decoder, decoder->slot + index * 64, 6, 0);
    if (slot < 4) return slot;
    bits = slot / 2 - 1;
    distance = (2u + (slot & 1)) << bits;
    if (slot < 14)
        return distance + xz_tree(decoder,
                                   decoder->distance + distance - slot,
                                   bits, 1);
    direct = 0;
    for (index = 0; index < bits - 4; index++) {
        xz_normalize(decoder);
        decoder->range >>= 1;
        direct <<= 1;
        if (decoder->code >= decoder->range) {
            decoder->code -= decoder->range;
            direct |= 1;
        }
    }
    return distance + (direct << 4) +
           xz_tree(decoder, decoder->alignment, 4, 1);
}

static int xz_lzma_chunk(xz_decoder_t *decoder, const uint8_t *input,
                          size_t packed, size_t unpacked)
{
    size_t end;
    unsigned position;
    unsigned length;
    unsigned index;
    uint32_t distance;
    uint64_t actual_distance;

    if (packed < 5 || input[0] || unpacked >
        decoder->capacity - decoder->written) return -1;
    decoder->input.next = input + 5;
    decoder->input.end = input + packed;
    decoder->range = UINT32_MAX;
    decoder->code = (uint32_t)input[1] << 24 | (uint32_t)input[2] << 16 |
                    (uint32_t)input[3] << 8 | input[4];
    decoder->failed = decoder->code == UINT32_MAX;
    end = decoder->written + unpacked;
    while (decoder->written < end && !decoder->failed) {
        position = (decoder->written - decoder->dictionary_start) &
                   (decoder->positions - 1);
        if (!xz_bit(decoder, decoder->match +
                     decoder->state * decoder->positions + position)) {
            if (xz_literal(decoder)) return -1;
            continue;
        }
        length = 1;
        if (!xz_bit(decoder, decoder->repeat + decoder->state)) {
            length = xz_length(decoder, 0, position);
            distance = xz_match_distance(decoder, length);
            for (index = 3; index; index--)
                decoder->history[index] = decoder->history[index - 1];
            decoder->history[0] = distance;
            decoder->state = decoder->state < 7 ? 7 : 10;
        } else {
            if (!xz_bit(decoder, decoder->select[0] + decoder->state)) {
                if (!xz_bit(decoder, decoder->short_repeat +
                             decoder->state * decoder->positions + position)) {
                    decoder->state = decoder->state < 7 ? 9 : 11;
                } else {
                    length = xz_length(decoder, 1, position);
                    decoder->state = decoder->state < 7 ? 8 : 11;
                }
            } else {
                if (!xz_bit(decoder, decoder->select[1] + decoder->state))
                    index = 1;
                else index = 2 + xz_bit(decoder,
                                         decoder->select[2] + decoder->state);
                distance = decoder->history[index];
                while (index) {
                    decoder->history[index] = decoder->history[index - 1];
                    index--;
                }
                decoder->history[0] = distance;
                length = xz_length(decoder, 1, position);
                decoder->state = decoder->state < 7 ? 8 : 11;
            }
        }
        actual_distance = (uint64_t)decoder->history[0] + 1;
        if (decoder->failed || length > end - decoder->written ||
            !xz_distance_valid(decoder, actual_distance)) return -1;
        while (length--) {
            decoder->output[decoder->written] =
                decoder->output[decoder->written - actual_distance];
            decoder->written++;
        }
    }
    xz_normalize(decoder);
    return decoder->failed || decoder->code ||
           decoder->input.next != decoder->input.end ? -1 : 0;
}

static int xz_lzma2(xz_decoder_t *decoder, const uint8_t *input,
                     size_t size)
{
    size_t offset;
    size_t unpacked;
    size_t packed;
    unsigned control;
    int need_dictionary;
    int need_properties;
    int need_reset;

    offset = 0;
    need_dictionary = 1;
    need_properties = 1;
    need_reset = 1;
    decoder->written = 0;
    decoder->dictionary_start = 0;
    while (offset < size) {
        control = input[offset++];
        if (!control) return offset == size ? 0 : -1;
        if (size - offset < 2) return -1;
        unpacked = ((size_t)input[offset] << 8 | input[offset + 1]) + 1;
        offset += 2;
        if (control == 1 || control == 2) {
            if (control == 1) {
                decoder->dictionary_start = decoder->written;
                need_dictionary = 0;
                need_properties = 1;
            }
            if (need_dictionary || unpacked > size - offset ||
                unpacked > decoder->capacity - decoder->written) return -1;
            memcpy(decoder->output + decoder->written, input + offset,
                   unpacked);
            decoder->written += unpacked;
            offset += unpacked;
            need_reset = 1;
            continue;
        }
        if (control < 128 || size - offset < 2) return -1;
        unpacked += (size_t)(control & 31) << 16;
        packed = ((size_t)input[offset] << 8 | input[offset + 1]) + 1;
        offset += 2;
        if (control >= 224) {
            decoder->dictionary_start = decoder->written;
            need_dictionary = 0;
            need_properties = 1;
        }
        if (need_dictionary || (need_reset && control < 160)) return -1;
        if (control >= 192) {
            if (offset == size || xz_properties(decoder, input[offset++]))
                return -1;
            need_properties = 0;
        }
        if (need_properties || packed > size - offset) return -1;
        if (control >= 160) xz_reset_model(decoder);
        if (xz_lzma_chunk(decoder, input + offset, packed, unpacked))
            return -1;
        offset += packed;
        need_reset = 0;
    }
    return -1;
}

static int xz_sign_byte(unsigned value)
{
    return value == 0 || value == 255;
}

static void xz_x86(uint8_t *data, size_t size, uint32_t start)
{
    size_t position;
    size_t previous;
    size_t gap;
    unsigned mask;
    unsigned width;
    unsigned bits;
    uint32_t source;
    uint32_t target;
    uint32_t address;

    position = 0;
    previous = (size_t)-1;
    mask = 0;
    while (size - position >= 5) {
        if ((data[position] & 254) != 232) {
            position++;
            continue;
        }
        gap = position - previous;
        if (gap > 3) mask = 0;
        else {
            mask = (mask << (gap - 1)) & 7;
            if (mask) {
                width = 0;
                for (bits = mask; bits; bits >>= 1) width++;
                if ((mask & (mask - 1)) ||
                    xz_sign_byte(data[position + 4 - width])) {
                    previous = position++;
                    mask = ((mask << 1) & 7) | 1;
                    continue;
                }
            }
        }
        previous = position;
        if (xz_sign_byte(data[position + 4])) {
            source = xz_u32(data + position + 1);
            address = start + (uint32_t)position + 5;
            for (;;) {
                target = source - address;
                if (!mask) break;
                width = 0;
                for (bits = mask; bits; bits >>= 1) width++;
                bits = width * 8;
                if (!xz_sign_byte((target >> (24 - bits)) & 255)) break;
                source = target ^ (UINT32_MAX >> bits);
            }
            data[position + 1] = (uint8_t)target;
            data[position + 2] = (uint8_t)(target >> 8);
            data[position + 3] = (uint8_t)(target >> 16);
            data[position + 4] = (target & (1u << 24)) ? 255 : 0;
            position += 5;
        } else {
            position++;
            mask = ((mask << 1) & 7) | 1;
        }
    }
}

static int xz_stream(xz_decoder_t *decoder, const uint8_t *source,
                      size_t size, uint8_t *output, size_t capacity,
                      size_t *written)
{
    static const uint8_t magic[6] = { 253, 55, 122, 88, 90, 0 };
    xz_input_t index;
    xz_input_t header;
    const uint8_t *footer;
    const uint8_t *index_start;
    const uint8_t *block;
    const uint8_t *payload;
    const uint8_t *check;
    uint64_t index_size;
    uint64_t records;
    uint64_t record;
    uint64_t unpadded;
    uint64_t unpacked;
    uint64_t packed;
    uint64_t specified_packed;
    uint64_t specified_unpacked;
    uint64_t identifier;
    uint64_t properties;
    size_t header_size;
    size_t check_size;
    size_t total;
    size_t padding;
    unsigned flags;
    unsigned filters;
    unsigned filter;
    unsigned dictionary;
    uint32_t x86_start;

    if (size < 32 || (size & 3) || memcmp(source, magic, 6)) return -1;
    padding = 0;
    while (size && !source[size - 1]) {
        size--;
        padding++;
    }
    if (size < 32 || (padding & 3)) return -1;
    footer = source + size - 12;
    if (source[6] || source[7] > 1 || footer[10] != 89 ||
        footer[11] != 90 || footer[8] != source[6] ||
        footer[9] != source[7] || xz_crc(source + 6, 2) !=
        xz_u32(source + 8) || xz_crc(footer + 4, 6) != xz_u32(footer))
        return -1;
    check_size = source[7] ? 4 : 0;
    index_size = ((uint64_t)xz_u32(footer + 4) + 1) * 4;
    if (index_size > size - 24 || index_size < 8) return -1;
    index_start = footer - index_size;
    if (*index_start || xz_crc(index_start, index_size - 4) !=
        xz_u32(footer - 4)) return -1;
    index.next = index_start + 1;
    index.end = footer - 4;
    if (xz_integer(&index, &records)) return -1;
    block = source + 12;
    total = 0;
    for (record = 0; record < records; record++) {
        if (xz_integer(&index, &unpadded) || xz_integer(&index, &unpacked) ||
            block >= index_start || !*block) return -1;
        header_size = ((size_t)*block + 1) * 4;
        if (header_size > (size_t)(index_start - block) ||
            unpadded > (uint64_t)(index_start - block) ||
            unpadded < header_size + check_size + 1 ||
            unpacked > capacity - total) return -1;
        if (xz_crc(block, header_size - 4) !=
            xz_u32(block + header_size - 4)) return -1;
        flags = block[1];
        filters = (flags & 3) + 1;
        if ((flags & 60) || filters > 2) return -1;
        header.next = block + 2;
        header.end = block + header_size - 4;
        specified_packed = UINT64_MAX;
        specified_unpacked = UINT64_MAX;
        if ((flags & 64) && xz_integer(&header, &specified_packed)) return -1;
        if ((flags & 128) && xz_integer(&header, &specified_unpacked)) return -1;
        x86_start = 0;
        dictionary = 0;
        for (filter = 0; filter < filters; filter++) {
            if (xz_integer(&header, &identifier) ||
                xz_integer(&header, &properties) ||
                properties > (uint64_t)(header.end - header.next)) return -1;
            if (filter + 1 == filters) {
                if (identifier != 33 || properties != 1 ||
                    *header.next > 40) return -1;
                dictionary = *header.next;
            } else {
                if (identifier != 4 || (properties && properties != 4))
                    return -1;
                if (properties) x86_start = xz_u32(header.next);
            }
            header.next += properties;
        }
        while (header.next < header.end)
            if (*header.next++) return -1;
        packed = unpadded - header_size - check_size;
        if ((specified_packed != UINT64_MAX && specified_packed != packed) ||
            (specified_unpacked != UINT64_MAX && specified_unpacked != unpacked))
            return -1;
        payload = block + header_size;
        padding = (4 - (unpadded & 3)) & 3;
        if (padding > (uint64_t)(index_start - block) - unpadded) return -1;
        decoder->dictionary = dictionary == 40 ? UINT32_MAX :
                              (uint64_t)(2 + (dictionary & 1)) <<
                              (dictionary / 2 + 11);
        decoder->output = output + total;
        decoder->capacity = capacity - total;
        if (xz_lzma2(decoder, payload, packed) ||
            decoder->written != unpacked) return -1;
        if (filters == 2) xz_x86(output + total, unpacked, x86_start);
        check = payload + packed;
        while (padding--)
            if (*check++) return -1;
        if (check_size && xz_crc(output + total, unpacked) != xz_u32(check))
            return -1;
        block = check + check_size;
        total += unpacked;
    }
    if (block != index_start || (size_t)(index.end - index.next) > 3)
        return -1;
    while (index.next < index.end)
        if (*index.next++) return -1;
    *written = total;
    return 0;
}

int xz_decompress_xz(const uint8_t *source, uint64_t source_size,
                      uint8_t *output, uint64_t output_size,
                      uint64_t *output_length)
{
    xz_decoder_t decoder;
    size_t written;
    int result;

    if (output_length) *output_length = 0;
    if (!source || !output || source_size > SIZE_MAX || output_size > SIZE_MAX)
        return -1;
    memset(&decoder, 0, sizeof(decoder));
    written = 0;
    result = xz_stream(&decoder, source, source_size, output, output_size,
                       &written);
    kfree(decoder.probabilities);
    if (!result && output_length) *output_length = written;
    return result;
}
