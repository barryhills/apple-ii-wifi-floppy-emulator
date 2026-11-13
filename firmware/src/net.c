// WiFi station management: connect, watch the link, reconnect with backoff.
// Also brings up mDNS (<hostname>.local, _http._tcp) and the HTTP server.

#include <stdio.h>
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/mdns.h"

#include "config_defaults.h"
#include "net.h"
#include "http.h"

#define CONNECT_TIMEOUT_MS  30000
#define RETRY_DELAY_MS       5000
#define RSSI_PERIOD_MS       5000

typedef enum { NET_START, NET_CONNECTING, NET_UP, NET_BACKOFF } net_state_t;

static net_state_t state;
static absolute_time_t deadline;
static absolute_time_t next_rssi;
static int32_t rssi_cache;
static bool rssi_valid;
static char ip_str[16];

// Runtime config. For now always the build-time defaults; later loaded from
// flash. Must stay valid for the life of the netif (lwIP keeps the pointer).
static const char *cfg_ssid     = CFG_DEFAULT_SSID;
static const char *cfg_password = CFG_DEFAULT_PASSWORD;
static const char *cfg_hostname = CFG_DEFAULT_HOSTNAME;

static struct netif *sta_netif(void)
{
    return &cyw43_state.netif[CYW43_ITF_STA];
}

static const char *link_str(int s)
{
    switch (s) {
    case CYW43_LINK_DOWN:    return "down";
    case CYW43_LINK_JOIN:    return "joined, no IP yet";
    case CYW43_LINK_NOIP:    return "joined, no IP";
    case CYW43_LINK_UP:      return "up";
    case CYW43_LINK_FAIL:    return "connection failed";
    case CYW43_LINK_NONET:   return "SSID not found";
    case CYW43_LINK_BADAUTH: return "authentication failed (password?)";
    default:                 return "?";
    }
}

static void mdns_txt(struct mdns_service *service, void *userdata)
{
    (void)userdata;
    mdns_resp_add_service_txtitem(service, "path=/", 6);
}

void net_init(void)
{
    cyw43_arch_enable_sta_mode();   // creates the STA netif and starts DHCP

    cyw43_arch_lwip_begin();
    netif_set_hostname(sta_netif(), cfg_hostname);
    mdns_resp_init();
    mdns_resp_add_netif(sta_netif(), cfg_hostname);
    mdns_resp_add_service(sta_netif(), cfg_hostname, "_http", DNSSD_PROTO_TCP,
                          80, mdns_txt, NULL);
    http_init();
    cyw43_arch_lwip_end();

    state = NET_START;
}

static void start_connect(void)
{
    uint32_t auth = cfg_password[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    printf("wifi: connecting to \"%s\"\n", cfg_ssid);
    int rc = cyw43_arch_wifi_connect_async(cfg_ssid,
                                           cfg_password[0] ? cfg_password : NULL,
                                           auth);
    if (rc) {
        printf("wifi: connect_async error %d\n", rc);
        state = NET_BACKOFF;
        deadline = make_timeout_time_ms(RETRY_DELAY_MS);
        return;
    }
    state = NET_CONNECTING;
    deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
}

static void give_up(const char *why)
{
    printf("wifi: %s; retrying in %d s\n", why, RETRY_DELAY_MS / 1000);
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    ip_str[0] = 0;
    rssi_valid = false;
    state = NET_BACKOFF;
    deadline = make_timeout_time_ms(RETRY_DELAY_MS);
}

void net_poll(void)
{
    int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);

    switch (state) {
    case NET_START:
        start_connect();
        break;

    case NET_CONNECTING:
        if (link == CYW43_LINK_UP) {
            cyw43_arch_lwip_begin();
            ip4addr_ntoa_r(netif_ip4_addr(sta_netif()), ip_str, sizeof ip_str);
            mdns_resp_announce(sta_netif());
            cyw43_arch_lwip_end();
            printf("wifi: up, IP %s, http://%s.local/\n", ip_str, cfg_hostname);
            state = NET_UP;
            next_rssi = get_absolute_time();
        } else if (link < 0) {
            give_up(link_str(link));
        } else if (time_reached(deadline)) {
            give_up("timed out");
        }
        break;

    case NET_UP:
        if (link != CYW43_LINK_UP) {
            printf("wifi: link lost (%s)\n", link_str(link));
            give_up("link lost");
        } else if (time_reached(next_rssi)) {
            rssi_valid = cyw43_wifi_get_rssi(&cyw43_state, &rssi_cache) == 0;
            next_rssi = make_timeout_time_ms(RSSI_PERIOD_MS);
        }
        break;

    case NET_BACKOFF:
        if (time_reached(deadline))
            start_connect();
        break;
    }
}

bool net_is_up(void)          { return state == NET_UP; }
const char *net_hostname(void){ return cfg_hostname; }
const char *net_ssid(void)    { return cfg_ssid; }
const char *net_ip_str(void)  { return state == NET_UP ? ip_str : ""; }

const char *net_state_str(void)
{
    switch (state) {
    case NET_START:      return "starting";
    case NET_CONNECTING: return "connecting";
    case NET_UP:         return "up";
    case NET_BACKOFF:    return "retry wait";
    }
    return "?";
}

bool net_rssi(int32_t *rssi)
{
    if (!rssi_valid || state != NET_UP)
        return false;
    *rssi = rssi_cache;
    return true;
}
