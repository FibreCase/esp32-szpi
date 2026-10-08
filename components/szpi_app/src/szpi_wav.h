#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*szpi_wav_read_at_fn)(void *context, uint64_t offset, void *buffer, size_t length);

typedef struct {
    uint64_t data_offset;
    uint32_t data_bytes;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint16_t block_align;
} szpi_wav_info_t;

bool szpi_wav_build_pcm_stereo_header(uint32_t data_bytes, uint8_t header[44]);
bool szpi_wav_parse(szpi_wav_read_at_fn read_at, void *context, uint64_t file_bytes, szpi_wav_info_t *info);
