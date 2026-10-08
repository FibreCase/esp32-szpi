#include "provision_protocol.h"
#include <string.h>

bool szpi_wifi_credentials_valid(const char *ssid, const char *password, bool wpa3_only)
{
    if (!ssid || !password) return false;
    size_t n = strnlen(ssid, 33), p = strnlen(password, 65);
    if (!n || n > 32 || p < 8 || p > 64 || (wpa3_only && p > 63)) return false;
    /* UTF-8 is accepted; embedded NUL/control bytes cannot be represented by UI. */
    for (size_t i = 0; i < n; i++) if ((unsigned char)ssid[i] < 32 || ssid[i] == 127) return false;
    for (size_t i = 0; i < p; i++) if ((unsigned char)password[i] < 32 || password[i] == 127) return false;
    if (p == 64) {
        for (size_t i = 0; i < p; i++) {
            char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        }
    }
    return true;
}

static bool append(char *out, size_t cap, size_t *n, const char *text, bool escape)
{
    for (; *text; text++) {
        if (escape && strchr("\\;,:\"", *text)) {
            if (*n + 1 >= cap) return false;
            out[(*n)++] = '\\';
        }
        if (*n + 1 >= cap) return false;
        out[(*n)++] = *text;
    }
    out[*n] = 0;
    return true;
}

bool szpi_wifi_qr_encode(char *out, size_t cap, const char *ssid, const char *password)
{
    if (!out || !cap || !ssid || !password) return false;
    size_t n = 0;
    return append(out, cap, &n, "WIFI:T:WPA;S:", false) &&
        append(out, cap, &n, ssid, true) && append(out, cap, &n, ";P:", false) &&
        append(out, cap, &n, password, true) && append(out, cap, &n, ";;", false);
}

size_t szpi_dns_reply(const uint8_t *q, size_t size, uint8_t *out, size_t cap)
{
    if (!q || !out || size < 17 || (q[2] & 0xF8) || q[4] || q[5] != 1 ||
        q[6] || q[7] || q[8] || q[9]) return 0;
    size_t pos = 12, name_size = 0;
    while (pos < size && q[pos]) {
        size_t label = q[pos++];
        if (label > 63 || pos + label >= size) return 0;
        pos += label;
        name_size += label + 1;
        if (name_size > 253) return 0;
    }
    if (pos + 5 > size) return 0;
    pos++;
    uint16_t type = (q[pos] << 8) | q[pos + 1];
    if (q[pos + 2] || q[pos + 3] != 1 || (type != 1 && type != 28)) return 0;
    size_t end = pos + 4, total = end + (type == 1 ? 16 : 0);
    if (total > cap) return 0;
    memcpy(out, q, end);
    out[2] = 0x84 | (q[2] & 1); /* authoritative; copy recursion desired */
    out[3] = 0;
    memset(out + 6, 0, 6);
    if (type == 1) {
        out[7] = 1;
        const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 192, 168, 4, 1};
        memcpy(out + end, answer, sizeof(answer));
    }
    return total;
}
