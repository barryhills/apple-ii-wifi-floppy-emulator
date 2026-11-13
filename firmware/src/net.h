#pragma once
#include <stdbool.h>
#include <stdint.h>

// WiFi station + DHCP + mDNS + HTTP. Call net_init() once after
// cyw43_arch_init(), then net_poll() regularly from the main loop.
void net_init(void);
void net_poll(void);

bool        net_is_up(void);
const char *net_hostname(void);
const char *net_ssid(void);          // never expose the password
const char *net_ip_str(void);        // "" when not up
const char *net_state_str(void);
bool        net_rssi(int32_t *rssi); // cached, refreshed by net_poll()
