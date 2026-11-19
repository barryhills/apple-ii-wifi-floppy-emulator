// SSD1306 128x64 OLED on I2C0 (GP12 SDA, GP13 SCL; schematic r0.3).
//
// Init sequence and I2C framing (control byte 0x80 = one command, 0x40 =
// data stream follows; horizontal addressing so the whole 1 KB frame goes in
// one write) follow Raspberry Pi's pico-examples i2c/ssd1306_i2c
// (BSD-3-Clause), with the 128x64 values it selects (MUX 63, COM pins 0x12).
// Not yet checked against the SSD1306 datasheet itself.
//
// Text: 4 lines of 21 characters, 5x7 glyphs drawn double height (5x14) +
// 1 column gap: Barry found single height too small on the 0.96" panel. The screen is
// redrawn from the main loop at most every OLED_PERIOD_MS (at once after a
// button press), and only sent when the frame changed: a full frame is
// ~23 ms of blocking I2C at 400 kHz.
//
// Buttons (GP9 SELECT/ENTER*, GP14 UP*, GP15 DOWN*; to GND, internal
// pull-ups): any press on the home screen opens the picker, a scrolling list
// of the carousel's filled rows with the cursor (inverse video) on the mounted
// image; '*' marks the mounted image. UP = next image, DOWN = previous (held: repeat), SELECT
// mounts the image under the cursor. The drive's buttons change the mounted
// image only (division of control, CLAUDE.md). Idle PICK_IDLE_MS -> home.

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "font5x7.h"
#include "api.h"
#include "net.h"
#include "drive.h"
#include "oled.h"
#include "store.h"

#define OLED_I2C        i2c0
#define OLED_SDA        12
#define OLED_SCL        13
#define OLED_ADDR       0x3C
#define OLED_HZ         400000
#define OLED_PERIOD_MS  250
#define W 128
#define H 64
#define COLS (W / 6)            // 21
#define LINES (H / 8)           // 8

static uint8_t frame[1 + W * H / 8];       // [0] = 0x40 data control byte
static uint8_t sent[W * H / 8];
static bool present, have_sent;
static absolute_time_t next_draw;

#define BTN_SELECT      9
#define BTN_UP          14
#define BTN_DOWN        15
#define DEBOUNCE_US     20000
#define REPEAT_FIRST_MS 500
#define REPEAT_MS       150
#define PICK_IDLE_MS    15000
#define NOTE_MS         1500

static bool cmd(uint8_t c)
{
    uint8_t b[2] = {0x80, c};
    return i2c_write_timeout_us(OLED_I2C, OLED_ADDR, b, 2, false, 2000) == 2;
}

static bool cmds(const uint8_t *c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!cmd(c[i]))
            return false;
    return true;
}

// Spread the low 4 bits of b over 8 bits, each bit doubled (vertical 2x).
static uint8_t dbl(uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 4; i++)
        if (b & (1u << i))
            r |= 3u << (2 * i);
    return r;
}

// Text line 0..3, each two 8-pixel pages tall: glyphs drawn at double height.
static void put_line(int line, const char *s)
{
    uint8_t *top = &frame[1 + 2 * line * W], *bot = top + W;
    memset(top, 0, 2 * W);
    for (int col = 0; col < COLS && *s; col++, s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch < FONT_FIRST || ch > FONT_LAST)
            ch = '?';
        for (int x = 0; x < 5; x++) {
            uint8_t g = font5x7[(ch - FONT_FIRST) * 5 + x];
            top[col * 6 + x] = dbl(g);
            bot[col * 6 + x] = dbl(g >> 4);
        }
    }
}

void oled_init(void)
{
    i2c_init(OLED_I2C, OLED_HZ);
    gpio_set_function(OLED_SDA, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA);     // the module has its own pull-ups; these are weak extras
    gpio_pull_up(OLED_SCL);

    static const uint8_t init[] = {
        0xAE,           // display off
        0x20, 0x00,     // memory mode: horizontal
        0x40,           // start line 0
        0xA1,           // segment remap: column 127 -> SEG0
        0xA8, H - 1,    // multiplex ratio
        0xC8,           // COM scan direction: remapped
        0xD3, 0x00,     // display offset 0
        0xDA, 0x12,     // COM pins config for 128x64
        0xD5, 0x80,     // clock divide
        0xD9, 0xF1,     // precharge
        0xDB, 0x30,     // VCOMH deselect
        0x81, 0xFF,     // contrast
        0xA4,           // follow RAM
        0xA6,           // normal (not inverted)
        0x8D, 0x14,     // charge pump on
        0x2E,           // scrolling off
        0xAF,           // display on
    };
    present = cmds(init, sizeof init);
    printf("oled: %s\n", present ? "found at 0x3C" : "not found");
    frame[0] = 0x40;
    next_draw = get_absolute_time();

    const uint pins[] = {BTN_SELECT, BTN_UP, BTN_DOWN};
    for (int i = 0; i < 3; i++) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_IN);
        gpio_pull_up(pins[i]);
    }
}

// ---------------------------------------------------------------- buttons

// Boot-time check for setup mode: both held across three samples 10 ms apart.
bool oled_setup_keys_held(void)
{
    for (int i = 0; i < 3; i++) {
        if (gpio_get(BTN_UP) || gpio_get(BTN_DOWN))
            return false;
        sleep_ms(10);
    }
    return true;
}

enum { B_SELECT, B_UP, B_DOWN, B_N };
static const uint btn_pin[B_N] = {BTN_SELECT, BTN_UP, BTN_DOWN};
static bool btn_down[B_N];              // debounced state
static uint64_t btn_change[B_N];        // last raw change, us
static bool btn_raw[B_N];
static absolute_time_t btn_repeat[B_N];

// Returns a bit mask of buttons that produced a press event this call:
// the debounced press itself, then auto-repeat for UP/DOWN while held.
static unsigned buttons_poll(void)
{
    unsigned ev = 0;
    uint64_t now = time_us_64();
    static bool started;
    if (!started) {
        // Keys already held at boot (UP+DOWN for setup mode) count as down,
        // so they produce no press until released and pressed again.
        started = true;
        for (int i = 0; i < B_N; i++) {
            btn_raw[i] = btn_down[i] = !gpio_get(btn_pin[i]);
            btn_repeat[i] = at_the_end_of_time;     // and no auto-repeat either
        }
        return 0;
    }
    for (int i = 0; i < B_N; i++) {
        bool raw = !gpio_get(btn_pin[i]);          // active low
        if (raw != btn_raw[i]) {
            btn_raw[i] = raw;
            btn_change[i] = now;
        }
        if (raw != btn_down[i] && now - btn_change[i] >= DEBOUNCE_US) {
            btn_down[i] = raw;
            if (raw) {
                ev |= 1u << i;
                btn_repeat[i] = make_timeout_time_ms(REPEAT_FIRST_MS);
            }
        } else if (btn_down[i] && i != B_SELECT && time_reached(btn_repeat[i])) {
            ev |= 1u << i;
            btn_repeat[i] = make_timeout_time_ms(REPEAT_MS);
        }
    }
    return ev;
}

// ---------------------------------------------------------------- screens

static bool picking;
static int cursor;                      // index into the filled-row list
static absolute_time_t pick_idle;
static char note[COLS + 1];             // short message shown on the home screen
static absolute_time_t note_until;

// The filled rows of the carousel, in row order.
static int filled(int rows[STORE_ROWS])
{
    int n = 0;
    image_info_t im;
    for (int r = 0; r < STORE_ROWS; r++)
        if (store_row(r, &im))
            rows[n++] = r;
    return n;
}

static void invert_line(int line)
{
    uint8_t *p = &frame[1 + 2 * line * W];
    for (int i = 0; i < 2 * W; i++)
        p[i] ^= 0xFF;
}

static void set_note(const char *t)
{
    snprintf(note, sizeof note, "%s", t);
    note_until = make_timeout_time_ms(NOTE_MS);
}

static void draw_home(void)
{
    char s[COLS + 8];
    image_info_t im;
    if (net_setup_mode()) {
        // Instructions only; a picker message briefly replaces the title.
        put_line(0, time_reached(note_until) ? "WiFi setup mode" : note);
        snprintf(s, sizeof s, "SSID: %s", net_setup_ssid());
        put_line(1, s);
        put_line(2, "Password: none");
        snprintf(s, sizeof s, "http://%s", net_ip_str());
        put_line(3, s);
        return;
    }
    snprintf(s, sizeof s, "%s.local", net_hostname());
    put_line(0, s);
    if (net_is_up())
        snprintf(s, sizeof s, "%s", net_ip_str());
    else
        snprintf(s, sizeof s, "%s...", net_state_str());
    put_line(1, s);
    if (time_reached(note_until))
        drive_oled_line(s, sizeof s);           // head position, ON while selected
    put_line(2, time_reached(note_until) ? s : note);   // brief messages win
    put_line(3, store_image(&im) ? im.name : "(drive empty)");
}

// Header line, then three list lines; the window scrolls to keep the cursor
// visible, cursor line in inverse video.
static void draw_picker(const int *rows, int n)
{
    char s[IMAGE_NAME_MAX + 2];       // put_line shows the first COLS characters
    snprintf(s, sizeof s, "Mount %d of %d", cursor + 1, n);
    put_line(0, s);
    int first = cursor - 1;                     // cursor in the middle when possible
    if (first > n - 3)
        first = n - 3;
    if (first < 0)
        first = 0;
    int mounted = store_mounted();
    for (int line = 1; line < 4; line++) {
        int i = first + line - 1;
        image_info_t im;
        if (i < n && store_row(rows[i], &im)) {
            snprintf(s, sizeof s, "%c%s", rows[i] == mounted ? '*' : ' ', im.name);
            put_line(line, s);
            if (i == cursor)
                invert_line(line);
        } else {
            put_line(line, "");
        }
    }
}

static void mount_selected(const int *rows, int n)
{
    picking = false;
    if (cursor >= n || rows[cursor] == store_mounted())
        return;
    if (api_busy()) {
        set_note("busy, try again");
        return;
    }
    char err[96];
    if (store_mount(rows[cursor], err, sizeof err) == 0) {
        set_note("mounted");
        printf("buttons: mounted row %d\n", rows[cursor] + 1);
    } else {
        set_note("mount failed");
        printf("buttons: mount failed: %s\n", err);
    }
}

void oled_poll(void)
{
    unsigned ev = buttons_poll();
    if (!present)
        return;

    int rows[STORE_ROWS], n = filled(rows);
    if (ev) {
        if (!picking) {
            picking = true;                     // any button opens the picker
            cursor = 0;
            for (int i = 0; i < n; i++)
                if (rows[i] == store_mounted())
                    cursor = i;
        } else if (ev & (1u << B_SELECT)) {
            mount_selected(rows, n);
        } else {
            // Barry, 2026-11-19 (rev B board): DOWN (GP15, pin 20) goes to
            // the next image (higher number), UP (GP14, pin 19) to the
            // previous one. The 2026-11-14 opposite mapping came from a
            // breadboard with the two buttons swapped.
            if ((ev & (1u << B_DOWN)) && cursor < n - 1)
                cursor++;
            if ((ev & (1u << B_UP)) && cursor > 0)
                cursor--;
        }
        pick_idle = make_timeout_time_ms(PICK_IDLE_MS);
        next_draw = get_absolute_time();
    }
    if (picking && (n == 0 || time_reached(pick_idle))) {
        if (n == 0)
            set_note("carousel empty");
        picking = false;
    }
    if (picking && cursor >= n)
        cursor = n - 1;                         // a row was deleted meanwhile

    if (!time_reached(next_draw))
        return;
    next_draw = make_timeout_time_ms(OLED_PERIOD_MS);

    if (picking)
        draw_picker(rows, n);
    else
        draw_home();

    if (have_sent && memcmp(sent, frame + 1, sizeof sent) == 0)
        return;
    static const uint8_t window[] = {0x21, 0, W - 1, 0x22, 0, LINES - 1};
    if (cmds(window, sizeof window) &&
        i2c_write_timeout_us(OLED_I2C, OLED_ADDR, frame, sizeof frame, false, 100000)
            == (int)sizeof frame) {
        memcpy(sent, frame + 1, sizeof sent);
        have_sent = true;
    }
}
