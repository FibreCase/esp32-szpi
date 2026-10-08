#include <assert.h>
#include <stdio.h>
#include "portal_address.h"
int main(void)
{
    uint32_t ip;
    uint8_t v4[] = {192,168,4,1};
    uint8_t mapped[] = {0,0,0,0,0,0,0,0,0,0,255,255,192,168,4,2};
    assert(szpi_portal_address_ipv4(2, v4, 2, 10, &ip) && ip == 0xc0a80401);
    assert(szpi_portal_address_ipv4(10, mapped, 2, 10, &ip) && ip == 0xc0a80402);
    mapped[0] = 0x20;
    assert(!szpi_portal_address_ipv4(10, mapped, 2, 10, &ip));
    assert(!szpi_portal_address_ipv4(99, v4, 2, 10, &ip));
    puts("Portal IPv4 / mapped IPv6 address checks: PASS");
}
