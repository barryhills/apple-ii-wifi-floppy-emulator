// Disk II read-data player. See player.h and src/disk2.pio.
//
// DMA: the data channel copies the track buffer (count word + cells) into
// the state machine's TX FIFO, paced by its DREQ, then chains to the control
// channel. The control channel writes the buffer's address into the data
// channel's READ_ADD_TRIG alias, which restarts it with its transfer count
// reloaded from the value last written (TRANS_COUNT: "Writing this register
// sets the RELOAD value ... Each time this channel is triggered, the RELOAD
// value is copied into the live transfer counter", SDK hardware/regs/dma.h,
// from the RP2350 datasheet; RP2350-Floppy-Emulator relies on it too). So the
// stream loops: count, cells, count, cells, ... with the CPU out of it.
//
// Starting mid-revolution at cell s (word w = s / 32, bit k = s % 32), the
// method disk2.pio's comment describes: the CPU puts the cells left in this
// revolution (less one) into the FIFO, the data channel's first pass runs
// from word w to the end of the buffer (n - w words), and the state machine
// is entered by executing pull / mov y, osr / pull / out null, k / jmp cell.
// Once the first pass is running, TRANS_COUNT is rewritten to a whole
// revolution (1 + n), which changes only the reload value, so every pass
// the control channel triggers after that is a full revolution from word 0.
//
// player_start/player_stop are RAM-resident and use only register writes
// (no SDK calls that might be out-of-line in flash): instruction words and
// DMA control values are prepared by player_init on core 0.

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "disk2.pio.h"
#include "player.h"

#define PIO_HZ  4000000     // 16 cycles per cell -> 4 us cells

static PIO pio;
static uint sm, offset;
static int ch_data = -1, ch_ctrl = -1;
static const uint32_t *stream_ptr;      // the control channel's source
static uint32_t data_ctrl, ctrl_ctrl;   // CTRL register values (EN set)
static uint32_t i_pull, i_mov_y, i_out_null, i_jmp_cell, i_idle;
static bool idle_high;

uint32_t player_pack(uint32_t *buf, const uint8_t *bits, uint32_t cells)
{
    uint32_t n = cells / 32 + 1;
    buf[0] = cells - 1;
    for (uint32_t w = 0; w < n; w++) {
        uint32_t v = 0;
        for (int b = 0; b < 4; b++) {
            uint32_t i = w * 4 + (uint32_t)b;
            v = v << 8 | (i < (cells + 7) / 8 ? bits[i] : 0);
        }
        buf[1 + w] = v;
    }
    return 1 + n;
}

bool player_init(void)
{
    if (!pio_claim_free_sm_and_add_program_for_gpio_range(&disk2_program, &pio, &sm, &offset,
                                                          PLAYER_PIN, 1, true)) {
        printf("player: no free PIO state machine\n");
        return false;
    }
    ch_data = dma_claim_unused_channel(true);
    ch_ctrl = dma_claim_unused_channel(true);

    pio_gpio_init(pio, PLAYER_PIN);
    pio_sm_set_pins_with_mask(pio, sm, 1u << PLAYER_PIN, 1u << PLAYER_PIN);    // raw idle (side 1)
    pio_sm_set_consecutive_pindirs(pio, sm, PLAYER_PIN, 1, true);
    player_set_idle_high(false);

    pio_sm_config cfg = disk2_program_get_default_config(offset);
    sm_config_set_sideset_pins(&cfg, PLAYER_PIN);
    sm_config_set_out_shift(&cfg, false, false, 32);   // left = MSB first; no autopull
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&cfg, (float)clock_get_hz(clk_sys) / PIO_HZ);
    pio_sm_init(pio, sm, offset + disk2_offset_start, &cfg);    // leaves it disabled

    // Instructions executed to enter the program mid-revolution. The
    // program's side-set is mandatory (.side_set 1), so each carries side 1
    // (RDDATA raw idle), or it would drive a pulse.
    uint32_t side1 = pio_encode_sideset(1, 1);
    i_pull     = pio_encode_pull(false, true) | side1;
    i_mov_y    = pio_encode_mov(pio_y, pio_osr) | side1;
    i_out_null = pio_encode_out(pio_null, 1) | side1;          // bit count patched per use
    i_jmp_cell = pio_encode_jmp(offset + disk2_offset_cell) | side1;
    i_idle     = pio_encode_nop() | side1;

    dma_channel_config c = dma_channel_get_default_config(ch_ctrl);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_FORCE);
    ctrl_ctrl = channel_config_get_ctrl_value(&c);

    c = dma_channel_get_default_config(ch_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&c, ch_ctrl);
    data_ctrl = channel_config_get_ctrl_value(&c);

    printf("player: PIO%u SM%u, DMA %d/%d, clkdiv %.3f\n", (unsigned)pio_get_index(pio), sm,
           ch_data, ch_ctrl, (double)clock_get_hz(clk_sys) / PIO_HZ);
    return true;
}

void player_set_idle_high(bool high)
{
    idle_high = high;
    gpio_set_outover(PLAYER_PIN, high ? GPIO_OVERRIDE_NORMAL : GPIO_OVERRIDE_INVERT);
}

bool player_idle_high(void) { return idle_high; }

void __not_in_flash_func(player_stop)(void)
{
    if (ch_data < 0)
        return;
    hw_clear_bits(&pio->ctrl, 1u << (PIO_CTRL_SM_ENABLE_LSB + sm));
    hw_clear_bits(&dma_hw->ch[ch_ctrl].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[ch_data].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    uint32_t mask = (1u << ch_data) | (1u << ch_ctrl);
    dma_hw->abort = mask;
    while (dma_hw->abort & mask)
        tight_loop_contents();
    hw_xor_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);    // clears both FIFOs
    hw_xor_bits(&pio->sm[sm].shiftctrl, PIO_SM0_SHIFTCTRL_FJOIN_RX_BITS);
    pio->sm[sm].instr = i_idle;          // pin to idle, in case it stopped mid-pulse
}

void __not_in_flash_func(player_start)(const uint32_t *buf, uint32_t s)
{
    player_stop();
    uint32_t cells = buf[0] + 1;
    uint32_t n = cells / 32 + 1;                // data words per revolution
    // The reload is rewritten after the first pass starts. That pass cannot
    // finish first only while it is longer than what the FIFO takes with the
    // state machine stopped (8 words joined, one holding the count); a start
    // within the last 8 words begins the next revolution instead (at most
    // 256 cells early, 0.5% of a turn).
    if (s >= cells || (s / 32) + 8 > n)
        s = 0;
    uint32_t w = s / 32, k = s % 32;
    stream_ptr = buf;

    hw_set_bits(&pio->ctrl, (1u << (PIO_CTRL_SM_RESTART_LSB + sm)) |
                            (1u << (PIO_CTRL_CLKDIV_RESTART_LSB + sm)));
    pio->txf[sm] = cells - s - 1;               // ahead of the DMA's words

    dma_channel_hw_t *cc = &dma_hw->ch[ch_ctrl], *dc = &dma_hw->ch[ch_data];
    cc->read_addr = (uintptr_t)&stream_ptr;
    cc->write_addr = (uintptr_t)&dc->al3_read_addr_trig;
    cc->transfer_count = 1;
    cc->al1_ctrl = ctrl_ctrl;
    dc->read_addr = (uintptr_t)(buf + 1 + w);
    dc->write_addr = (uintptr_t)&pio->txf[sm];
    dc->transfer_count = n - w;                 // the first, partial pass
    dc->ctrl_trig = data_ctrl;                  // go
    dc->transfer_count = 1 + n;                 // reload only: later passes are whole

    // Wait for the count and the first data word, then enter the program.
    while (((pio->flevel >> (sm * 8)) & 0xF) < 2)
        tight_loop_contents();
    pio->sm[sm].instr = i_pull;                 // y = cells left, less one
    pio->sm[sm].instr = i_mov_y;
    pio->sm[sm].instr = i_pull;                 // word w
    if (k)
        pio->sm[sm].instr = (i_out_null & ~0x1Fu) | k;  // skip to cell s
    pio->sm[sm].instr = i_jmp_cell;
    hw_set_bits(&pio->ctrl, 1u << (PIO_CTRL_SM_ENABLE_LSB + sm));
}
