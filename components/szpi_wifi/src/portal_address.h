#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* IPv4 and IPv4-mapped IPv6 only; native IPv6 has no AP subnet here. */
static inline bool szpi_portal_address_ipv4(int family, const void *bytes,
    int ipv4_family, int ipv6_family, uint32_t *address)
{
    const uint8_t *value = bytes;
    if (family == ipv6_family) {
        static const uint8_t prefix[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
        if (memcmp(value, prefix, sizeof(prefix))) return false;
        value += 12;
    } else if (family != ipv4_family) return false;
    *address = ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
        ((uint32_t)value[2] << 8) | value[3];
    return true;
}
