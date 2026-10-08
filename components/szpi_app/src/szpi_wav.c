#include <string.h>
#include "szpi_wav.h"

#define WAV_SAMPLE_RATE 48000U
#define WAV_MAX_CHUNKS 32U

static uint16_t read_le16(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t read_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void write_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

bool szpi_wav_build_pcm_stereo_header(uint32_t data_bytes, uint8_t header[44])
{
    if (header == NULL || data_bytes > UINT32_MAX - 36U || (data_bytes % 4U) != 0) return false;
    memset(header, 0, 44);
    memcpy(header, "RIFF", 4);
    write_le32(header + 4, data_bytes + 36U);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16);
    write_le16(header + 20, 1);
    write_le16(header + 22, 2);
    write_le32(header + 24, WAV_SAMPLE_RATE);
    write_le32(header + 28, WAV_SAMPLE_RATE * 4U);
    write_le16(header + 32, 4);
    write_le16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, data_bytes);
    return true;
}

bool szpi_wav_parse(szpi_wav_read_at_fn read_at, void *context, uint64_t file_bytes, szpi_wav_info_t *info)
{
    if (read_at == NULL || info == NULL || file_bytes < 44U) return false;
    uint8_t riff[12];
    if (!read_at(context, 0, riff, sizeof(riff)) || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) return false;
    uint64_t riff_end = (uint64_t)read_le32(riff + 4) + 8U;
    if (riff_end < 12U || riff_end > file_bytes) return false;

    szpi_wav_info_t parsed = {0};
    uint16_t format = 0;
    uint32_t byte_rate = 0;
    bool have_fmt = false;
    bool have_data = false;
    uint64_t position = 12;
    for (uint8_t chunk = 0; chunk < WAV_MAX_CHUNKS && position + 8U <= riff_end; ++chunk) {
        uint8_t header[8];
        if (!read_at(context, position, header, sizeof(header))) return false;
        uint32_t chunk_size = read_le32(header + 4);
        uint64_t content = position + 8U;
        uint64_t next = content + (uint64_t)chunk_size + (chunk_size & 1U);
        if (next < content || next > riff_end) return false;
        if (memcmp(header, "fmt ", 4) == 0) {
            if (chunk_size < 16U) return false;
            uint8_t fmt[16];
            if (!read_at(context, content, fmt, sizeof(fmt))) return false;
            format = read_le16(fmt);
            parsed.channels = read_le16(fmt + 2);
            parsed.sample_rate = read_le32(fmt + 4);
            byte_rate = read_le32(fmt + 8);
            parsed.block_align = read_le16(fmt + 12);
            parsed.bits_per_sample = read_le16(fmt + 14);
            have_fmt = true;
        } else if (memcmp(header, "data", 4) == 0) {
            parsed.data_offset = content;
            parsed.data_bytes = chunk_size;
            have_data = true;
        }
        position = next;
        if (have_fmt && have_data) break;
    }
    if (!have_fmt || !have_data || format != 1 || (parsed.channels != 1U && parsed.channels != 2U) ||
        parsed.sample_rate != WAV_SAMPLE_RATE || parsed.bits_per_sample != 16U) return false;
    uint16_t expected_align = (uint16_t)(parsed.channels * 2U);
    if (parsed.block_align != expected_align || byte_rate != parsed.sample_rate * expected_align ||
        parsed.data_bytes == 0 || parsed.data_bytes > UINT32_MAX - 36U ||
        (parsed.data_bytes % expected_align) != 0 || parsed.data_offset + parsed.data_bytes > riff_end) return false;
    *info = parsed;
    return true;
}
