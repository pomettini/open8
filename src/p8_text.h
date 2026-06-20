/** @file p8_text.h
 *
 *  Parser for the textual PICO-8 .p8 cartridge format.
 *
 *  SPDX-License-Identifier: MIT
 **/
#ifndef P8_TEXT_H
#define P8_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool p8_text_parse(const uint8_t* data, size_t size, uint8_t* cart_data,
                   uint8_t** code, uint32_t* code_size);
const char* p8_text_error(void);

#endif
