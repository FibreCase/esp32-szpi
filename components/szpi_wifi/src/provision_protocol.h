#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool szpi_wifi_credentials_valid(const char *ssid, const char *password, bool wpa3_only);
bool szpi_wifi_qr_encode(char *out, size_t capacity, const char *ssid, const char *password);
/* Answers one standard IN A/AAAA question. AAAA gets NODATA, never a false A.
 * Rejects compressed/truncated questions and multicast/response packets. */
size_t szpi_dns_reply(const uint8_t *query, size_t size, uint8_t *out, size_t capacity);
