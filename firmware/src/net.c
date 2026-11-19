// WiFi station management: connect, watch the link, reconnect with backoff.
// Also brings up mDNS (<hostname>.local, _http._tcp), SNTP and the HTTP API.
//
// Setup mode (CLAUDE.md, "Network setup"): instead of joining a network the
// drive is an open access point SETUP_SSID at 192.168.4.1 (the cyw43
// driver's default AP address) with a DHCP server, serving the same page so
// the user can enter WiFi settings. Entered at boot (main.c decides): UP+DOWN
// held at power-on, no SSID configured, or a reboot requested by the
// fallback below.

#include <stdio.h>
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/mdns.h"

#include "config.h"
#include "net.h"
#include "api.h"
#include "clock.h"
#include "diag.h"
#include "third_party/dhcpserver/dhcpserver.h"

#define CONNECT_TIMEOUT_MS  30000
#define RETRY_DELAY_MS       5000
#define RSSI_PERIOD_MS       5000

typedef enum { NET_START, NET_CONNECTING, NET_UP, NET_BACKOFF, NET_SETUP } net_state_t;

static net_state_t state;
static absolute_time_t deadline;
static absolute_time_t next_rssi;
static int32_t rssi_cache;
static bool rssi_valid;
static char ip_str[16];

// Settings come from config.c (stored in flash). If the drive has not
// joined within FALLBACK_MS (Barry: 1 minute), it reboots into setup mode.
// Only before the first successful join: a later link loss just retries.
// A setup mode entered this way reboots to try again after SETUP_IDLE_MS with
// nobody on the access point, so a router that was slow to come back after a
// power cut doesn't leave the drive stranded in setup mode.
#define FALLBACK_MS   60000
#define SETUP_SSID    "a2drive-setup"
#define SETUP_IP      "192.168.4.1"
#define SETUP_IDLE_MS (5 * 60 * 1000)
static absolute_time_t stage_start;     // current settings first tried
static bool setup_auto;
static absolute_time_t setup_idle_until, next_sta_check;
static dhcp_server_t dhcp;
#define cfg_ssid     config_ssid()
#define cfg_password config_password()
#define cfg_hostname config_hostname()
static bool ever_up;

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

static void start_setup(void)
{
    cyw43_arch_enable_ap_mode(SETUP_SSID, NULL, CYW43_AUTH_OPEN);
    ip_addr_t ip, nm;
    IP_ADDR4(&ip, 192, 168, 4, 1);
    IP_ADDR4(&nm, 255, 255, 255, 0);
    cyw43_arch_lwip_begin();
    dhcp_server_init(&dhcp, &cyw43_state.netif[CYW43_ITF_AP], &ip, &nm);
    api_init();
    cyw43_arch_lwip_end();
    state = NET_SETUP;
    setup_idle_until = make_timeout_time_ms(SETUP_IDLE_MS);
    next_sta_check = get_absolute_time();
    printf("wifi: setup mode: join \"%s\" (open), then http://%s/\n", SETUP_SSID, SETUP_IP);
}

void net_init(bool setup, bool retry_when_idle)
{
    if (setup) {
        setup_auto = retry_when_idle;
        start_setup();
        return;
    }
    stage_start = get_absolute_time();
    cyw43_arch_enable_sta_mode();   // creates the STA netif and starts DHCP

    cyw43_arch_lwip_begin();
    netif_set_hostname(sta_netif(), cfg_hostname);
    mdns_resp_init();
    mdns_resp_add_netif(sta_netif(), cfg_hostname);
    mdns_resp_add_service(sta_netif(), cfg_hostname, "_http", DNSSD_PROTO_TCP,
                          80, mdns_txt, NULL);
    api_init();
    clock_start();
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
    if (!ever_up && absolute_time_diff_us(stage_start, get_absolute_time()) > FALLBACK_MS * 1000ll) {
        printf("wifi: no network joined in %d s; rebooting into setup mode\n", FALLBACK_MS / 1000);
        sleep_ms(50);                           // let the console drain
        diag_reboot_into_setup();
    }
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
            ever_up = true;
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

    case NET_SETUP:
        if (setup_auto && time_reached(next_sta_check)) {
            next_sta_check = make_timeout_time_ms(5000);
            int n = 0;
            cyw43_wifi_ap_get_stas(&cyw43_state, &n, NULL);
            if (n > 0) {
                setup_idle_until = make_timeout_time_ms(SETUP_IDLE_MS);
            } else if (time_reached(setup_idle_until)) {
                printf("wifi: setup mode unused for %d min; rebooting to try the network again\n",
                       SETUP_IDLE_MS / 60000);
                sleep_ms(50);
                watchdog_reboot(0, 0, 0);
                for (;;)
                    ;
            }
        }
        break;
    }
}

bool net_setup_mode(void)     { return state == NET_SETUP; }
const char *net_setup_ssid(void) { return SETUP_SSID; }

bool net_is_up(void)          { return state == NET_UP; }
const char *net_hostname(void){ return cfg_hostname; }
const char *net_ssid(void)    { return cfg_ssid; }
const char *net_ip_str(void)  { return state == NET_UP ? ip_str : state == NET_SETUP ? SETUP_IP : ""; }

const char *net_state_str(void)
{
    switch (state) {
    case NET_START:      return "starting";
    case NET_CONNECTING: return "connecting";
    case NET_UP:         return "up";
    case NET_BACKOFF:    return "retry wait";
    case NET_SETUP:      return "setup mode";
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
