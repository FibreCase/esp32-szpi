#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "provision_protocol.h"

int main(void)
{
    char qr[256];
    assert(szpi_wifi_qr_encode(qr, sizeof(qr), "Office;A:B,C\\D\"", "secret;:x"));
    assert(!strcmp(qr, "WIFI:T:WPA;S:Office\\;A\\:B\\,C\\\\D\\\";P:secret\\;\\:x;;"));
    assert(!szpi_wifi_qr_encode(qr, 12, "Office", "password"));
    assert(szpi_wifi_credentials_valid("Office", "password", false));
    assert(!szpi_wifi_credentials_valid("", "password", false));
    assert(!szpi_wifi_credentials_valid("Office", "short", false));
    assert(!szpi_wifi_credentials_valid("Office", "line\nbreak", false));
    char psk[65];
    memset(psk, 'a', 64); psk[64] = 0;
    assert(szpi_wifi_credentials_valid("Office", psk, false));
    assert(!szpi_wifi_credentials_valid("Office", psk, true));
    psk[3] = 'z';
    assert(!szpi_wifi_credentials_valid("Office", psk, false));
    uint8_t query[] = {0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
    uint8_t out[528];
    size_t n = szpi_dns_reply(query, sizeof(query), out, sizeof(out));
    assert(n == sizeof(query) + 16 && out[0] == 0x12 && out[1] == 0x34 && out[7] == 1);
    assert(!memcmp(out + n - 4, (uint8_t[]){192, 168, 4, 1}, 4));
    for (size_t i = 0; i < sizeof(query); i++) assert(!szpi_dns_reply(query, i, out, sizeof(out)));
    assert(!szpi_dns_reply(query, sizeof(query), out, 20));
    query[sizeof(query) - 3] = 28;
    assert(szpi_dns_reply(query, sizeof(query), out, sizeof(out)) == sizeof(query));
    assert(out[7] == 0);
    query[12] = 0xC0;
    assert(!szpi_dns_reply(query, sizeof(query), out, sizeof(out)));
    /* Exercise parser bounds over random and malformed datagrams. */
    uint32_t random = 1;
    uint8_t fuzz[512];
    for (unsigned k = 0; k < 20000; k++) {
        for (unsigned i = 0; i < sizeof(fuzz); i++) {
            random = random * 1664525U + 1013904223U;
            fuzz[i] = random >> 24;
        }
        (void)szpi_dns_reply(fuzz, k % sizeof(fuzz), out, sizeof(out));
    }
    puts("provision protocol: PASS");
}
