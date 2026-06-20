/** @file p8_text.c
 *
 *  Converts a textual .p8 cartridge into the same RAM/code representation used
 *  by the .p8.png loader.
 *
 *  SPDX-License-Identifier: MIT
 **/
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core.h"
#include "p8_text.h"

typedef enum p8_section
{
    P8_SECTION_NONE,
    P8_SECTION_LUA,
    P8_SECTION_GFX,
    P8_SECTION_GFF,
    P8_SECTION_MAP,
    P8_SECTION_SFX,
    P8_SECTION_MUSIC
} p8_section;

static const char* g_p8_text_error;

const char* p8_text_error(void)
{
    return g_p8_text_error ? g_p8_text_error : "unknown .p8 parse error";
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_byte(const char* text)
{
    int hi = hex_nibble(text[0]);
    int lo = hex_nibble(text[1]);
    return (hi < 0 || lo < 0) ? -1 : (hi << 4) | lo;
}

static bool line_equals(const char* line, size_t len, const char* expected)
{
    size_t expected_len = strlen(expected);
    return len == expected_len && memcmp(line, expected, len) == 0;
}

static bool is_include_line(const char* line, size_t len)
{
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
    return len - i >= 8 && memcmp(line + i, "#include", 8) == 0;
}

static p8_section section_from_line(const char* line, size_t len)
{
    if (line_equals(line, len, "__lua__")) return P8_SECTION_LUA;
    if (line_equals(line, len, "__gfx__")) return P8_SECTION_GFX;
    if (line_equals(line, len, "__gff__")) return P8_SECTION_GFF;
    if (line_equals(line, len, "__map__")) return P8_SECTION_MAP;
    if (line_equals(line, len, "__sfx__")) return P8_SECTION_SFX;
    if (line_equals(line, len, "__music__")) return P8_SECTION_MUSIC;
    if (len >= 4 && line[0] == '_' && line[1] == '_' &&
        line[len - 2] == '_' && line[len - 1] == '_')
        return P8_SECTION_NONE;
    return (p8_section)-1;
}

static bool parse_gfx_line(const char* line, size_t len, int row,
                           uint8_t* cart_data)
{
    if (row >= 128) return true;
    if (len < 128)
    {
        g_p8_text_error = "short __gfx__ row";
        return false;
    }

    uint8_t* dst = cart_data + row * 64;
    for (int x = 0; x < 128; x += 2)
    {
        int lo = hex_nibble(line[x]);
        int hi = hex_nibble(line[x + 1]);
        if (lo < 0 || hi < 0)
        {
            g_p8_text_error = "invalid hex in __gfx__";
            return false;
        }
        dst[x >> 1] = (uint8_t)(lo | (hi << 4));
    }
    return true;
}

static bool parse_hex_stream(const char* line, size_t len, uint8_t* dst,
                             int dst_size, int* offset, const char* error)
{
    size_t i = 0;
    while (i < len)
    {
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i == len) break;
        if (i + 1 >= len)
        {
            g_p8_text_error = error;
            return false;
        }
        int value = hex_byte(line + i);
        if (value < 0)
        {
            g_p8_text_error = error;
            return false;
        }
        if (*offset < dst_size) dst[(*offset)++] = (uint8_t)value;
        i += 2;
    }
    return true;
}

static bool parse_map_line(const char* line, size_t len, int row,
                           uint8_t* cart_data)
{
    if (row >= 64) return true;
    if (len < 256)
    {
        g_p8_text_error = "short __map__ row";
        return false;
    }

    uint8_t* dst = row < 32
        ? cart_data + 0x2000 + row * 128
        : cart_data + 0x1000 + (row - 32) * 128;
    for (int x = 0; x < 128; x++)
    {
        int value = hex_byte(line + x * 2);
        if (value < 0)
        {
            g_p8_text_error = "invalid hex in __map__";
            return false;
        }
        dst[x] = (uint8_t)value;
    }
    return true;
}

static bool parse_sfx_line(const char* line, size_t len, int index,
                           uint8_t* cart_data)
{
    if (index >= 64) return true;
    if (len < 168)
    {
        g_p8_text_error = "short __sfx__ row";
        return false;
    }

    uint8_t* dst = cart_data + 0x3200 + index * 68;
    int mode = hex_byte(line);
    int speed = hex_byte(line + 2);
    int loop_start = hex_byte(line + 4);
    int loop_end = hex_byte(line + 6);
    if (mode < 0 || speed < 0 || loop_start < 0 || loop_end < 0)
    {
        g_p8_text_error = "invalid header in __sfx__";
        return false;
    }

    dst[64] = (uint8_t)mode;
    dst[65] = (uint8_t)speed;
    dst[66] = (uint8_t)loop_start;
    dst[67] = (uint8_t)loop_end;

    for (int note = 0; note < 32; note++)
    {
        const char* src = line + 8 + note * 5;
        int pitch = hex_byte(src);
        int wave = hex_nibble(src[2]);
        int volume = hex_nibble(src[3]);
        int effect = hex_nibble(src[4]);
        if (pitch < 0 || wave < 0 || volume < 0 || effect < 0)
        {
            g_p8_text_error = "invalid note in __sfx__";
            return false;
        }
        uint16_t packed = (uint16_t)((pitch & 0x3f) |
            ((wave & 0x7) << 6) |
            ((volume & 0x7) << 9) |
            ((effect & 0x7) << 12) |
            ((wave & 0x8) << 12));
        dst[note * 2] = (uint8_t)packed;
        dst[note * 2 + 1] = (uint8_t)(packed >> 8);
    }
    return true;
}

static bool parse_music_line(const char* line, size_t len, int index,
                             uint8_t* cart_data)
{
    if (index >= 64) return true;
    if (len < 10)
    {
        g_p8_text_error = "short __music__ row";
        return false;
    }

    int flags = hex_byte(line);
    if (flags < 0)
    {
        g_p8_text_error = "invalid flags in __music__";
        return false;
    }

    size_t pos = 2;
    while (pos < len && (line[pos] == ' ' || line[pos] == '\t')) pos++;
    if (pos + 8 > len)
    {
        g_p8_text_error = "short channels in __music__";
        return false;
    }

    uint8_t* dst = cart_data + 0x3100 + index * 4;
    for (int channel = 0; channel < 4; channel++)
    {
        int value = hex_byte(line + pos + channel * 2);
        if (value < 0)
        {
            g_p8_text_error = "invalid channel in __music__";
            return false;
        }
        dst[channel] = (uint8_t)value;
    }
    if (flags & 0x01) dst[0] |= 0x80;
    if (flags & 0x02) dst[1] |= 0x80;
    if (flags & 0x04) dst[2] |= 0x80;
    return true;
}

bool p8_text_parse(const uint8_t* data, size_t size, uint8_t* cart_data,
                   uint8_t** code, uint32_t* code_size)
{
    if (!data || !cart_data || !code || !code_size)
    {
        g_p8_text_error = "invalid .p8 parser arguments";
        return false;
    }

    g_p8_text_error = NULL;
    *code = NULL;
    *code_size = 0;
    memset(cart_data, 0, CART_DATA_SIZE);

    uint8_t* parsed_code = (uint8_t*)SDL_malloc(size + 1);
    if (!parsed_code)
    {
        g_p8_text_error = "could not allocate .p8 Lua code";
        return false;
    }

    const uint8_t* cursor = data;
    const uint8_t* end = data + size;
    if (size >= 3 && cursor[0] == 0xef && cursor[1] == 0xbb &&
        cursor[2] == 0xbf)
        cursor += 3;

    p8_section section = P8_SECTION_NONE;
    bool valid_header = false;
    bool found_lua = false;
    size_t parsed_code_size = 0;
    int gfx_row = 0;
    int gff_offset = 0;
    int map_row = 0;
    int sfx_index = 0;
    int music_index = 0;

    while (cursor < end)
    {
        const char* line = (const char*)cursor;
        const uint8_t* newline = cursor;
        while (newline < end && *newline != '\n') newline++;
        size_t len = (size_t)(newline - cursor);
        if (len > 0 && line[len - 1] == '\r') len--;
        cursor = newline < end ? newline + 1 : end;

        if (!valid_header && len >= 16 &&
            memcmp(line, "pico-8 cartridge", 16) == 0)
            valid_header = true;

        p8_section next = section_from_line(line, len);
        if ((int)next >= 0)
        {
            section = next;
            if (section == P8_SECTION_LUA) found_lua = true;
            continue;
        }

        switch (section)
        {
            case P8_SECTION_LUA:
                if (is_include_line(line, len))
                {
                    g_p8_text_error =
                        "#include is unsupported; use a self-contained .p8";
                    SDL_free(parsed_code);
                    return false;
                }
                memcpy(parsed_code + parsed_code_size, line, len);
                parsed_code_size += len;
                parsed_code[parsed_code_size++] = '\n';
                break;
            case P8_SECTION_GFX:
                if (len && !parse_gfx_line(line, len, gfx_row++, cart_data))
                    goto parse_failed;
                break;
            case P8_SECTION_GFF:
                if (len && !parse_hex_stream(line, len, cart_data + 0x3000,
                                             0x100, &gff_offset,
                                             "invalid hex in __gff__"))
                    goto parse_failed;
                break;
            case P8_SECTION_MAP:
                if (len && !parse_map_line(line, len, map_row++, cart_data))
                    goto parse_failed;
                break;
            case P8_SECTION_SFX:
                if (len && !parse_sfx_line(line, len, sfx_index++, cart_data))
                    goto parse_failed;
                break;
            case P8_SECTION_MUSIC:
                if (len &&
                    !parse_music_line(line, len, music_index++, cart_data))
                    goto parse_failed;
                break;
            default:
                break;
        }
    }

    if (!valid_header)
    {
        g_p8_text_error = "missing PICO-8 cartridge header";
        goto parse_failed;
    }
    if (!found_lua)
    {
        g_p8_text_error = "missing __lua__ section";
        goto parse_failed;
    }

    parsed_code[parsed_code_size] = '\0';
    uint8_t* compact = (uint8_t*)SDL_realloc(
        parsed_code, parsed_code_size + 1);
    if (compact) parsed_code = compact;
    *code = parsed_code;
    *code_size = (uint32_t)parsed_code_size;
    return true;

parse_failed:
    SDL_free(parsed_code);
    return false;
}
