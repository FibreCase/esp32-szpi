#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "../components/szpi_app/src/szpi_wav.h"

typedef struct {
    uint8_t bytes[128];
    size_t length;
} memory_file_t;

static bool memory_read_at(void *context, uint64_t offset, void *buffer, size_t length)
{
    memory_file_t *file = context;
    if (offset > file->length || length > file->length - (size_t)offset) return false;
    memcpy(buffer, file->bytes + offset, length);
    return true;
}

static void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static size_t make_chunked_wav(memory_file_t *file, uint16_t channels, uint16_t bits, uint16_t format)
{
    uint8_t *b = file->bytes;
    memcpy(b, "RIFF", 4);
    memcpy(b + 8, "WAVE", 4);
    size_t pos = 12;
    memcpy(b + pos, "JUNK", 4);
    put_le32(b + pos + 4, 3);
    memcpy(b + pos + 8, "abc", 3);
    b[pos + 11] = 0;
    pos += 12;
    memcpy(b + pos, "fmt ", 4);
    put_le32(b + pos + 4, 16);
    put_le16(b + pos + 8, format);
    put_le16(b + pos + 10, channels);
    put_le32(b + pos + 12, 48000);
    uint16_t align = (uint16_t)(channels * (bits / 8U));
    put_le32(b + pos + 16, 48000U * align);
    put_le16(b + pos + 20, align);
    put_le16(b + pos + 22, bits);
    pos += 24;
    memcpy(b + pos, "data", 4);
    put_le32(b + pos + 4, 8);
    memset(b + pos + 8, 0x5a, 8);
    pos += 16;
    put_le32(b + 4, (uint32_t)(pos - 8));
    file->length = pos;
    return pos;
}

static void test_header(void)
{
    uint8_t header[44];
    assert(szpi_wav_build_pcm_stereo_header(3840, header));
    assert(memcmp(header, "RIFF", 4) == 0);
    assert(header[4] == 0x24 && header[5] == 0x0f);
    assert(memcmp(header + 8, "WAVEfmt ", 8) == 0);
    assert(header[22] == 2 && header[34] == 16);
    assert(memcmp(header + 36, "data", 4) == 0);
    assert(!szpi_wav_build_pcm_stereo_header(2, header));
    assert(!szpi_wav_build_pcm_stereo_header(UINT32_MAX, header));
}

static void test_parse_odd_chunk_and_mono(void)
{
    memory_file_t file = {0};
    szpi_wav_info_t info = {0};
    size_t length = make_chunked_wav(&file, 1, 16, 1);
    assert(szpi_wav_parse(memory_read_at, &file, length, &info));
    assert(info.channels == 1 && info.bits_per_sample == 16);
    assert(info.sample_rate == 48000 && info.block_align == 2);
    assert(info.data_offset == 56 && info.data_bytes == 8);
}

static void test_reject_bad_and_truncated_files(void)
{
    memory_file_t file = {0};
    szpi_wav_info_t info = {0};
    size_t length = make_chunked_wav(&file, 2, 16, 1);
    assert(szpi_wav_parse(memory_read_at, &file, length, &info));
    assert(info.channels == 2 && info.block_align == 4);
    assert(!szpi_wav_parse(memory_read_at, &file, length - 1U, &info));
    length = make_chunked_wav(&file, 2, 24, 1);
    assert(!szpi_wav_parse(memory_read_at, &file, length, &info));
    length = make_chunked_wav(&file, 2, 16, 3);
    assert(!szpi_wav_parse(memory_read_at, &file, length, &info));
    file.bytes[4] = 0xff;
    file.bytes[5] = 0xff;
    file.bytes[6] = 0xff;
    file.bytes[7] = 0x7f;
    assert(!szpi_wav_parse(memory_read_at, &file, length, &info));
}

int main(void)
{
    test_header();
    test_parse_odd_chunk_and_mono();
    test_reject_bad_and_truncated_files();
    return 0;
}
