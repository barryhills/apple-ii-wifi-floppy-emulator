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

#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "disk2.pio.h"
#include "player.h"

#define PIO_HZ  4000000     // 16 cycles per cell -> 4 us cells

static PIO pio;
static uint sm, offset;
static int ch_data = -1, ch_ctrl = -1;
static pio_sm_config cfg;
static const uint32_t *stream_ptr;     // the control channel's source

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
    pio_sm_set_pins_with_mask(pio, sm, 1u << PLAYER_PIN, 1u << PLAYER_PIN);    // idle high
    pio_sm_set_consecutive_pindirs(pio, sm, PLAYER_PIN, 1, true);

    cfg = disk2_program_get_default_config(offset);
    sm_config_set_sideset_pins(&cfg, PLAYER_PIN);
    sm_config_set_out_shift(&cfg, false, false, 32);   // left = MSB first; no autopull
    sm_config_set_fifo_join(&cfg, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv(&cfg, (float)clock_get_hz(clk_sys) / PIO_HZ);

    printf("player: PIO%u SM%u, DMA %d/%d, clkdiv %.3f\n", (unsigned)pio_get_index(pio), sm,
           ch_data, ch_ctrl, (double)clock_get_hz(clk_sys) / PIO_HZ);
    return true;
}

void player_stop(void)
{
    if (ch_data < 0)
        return;
    pio_sm_set_enabled(pio, sm, false);
    hw_clear_bits(&dma_hw->ch[ch_ctrl].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[ch_data].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_hw->abort = (1u << ch_data) | (1u << ch_ctrl);
    while (dma_hw->abort & ((1u << ch_data) | (1u << ch_ctrl)))
        tight_loop_contents();
    pio_sm_clear_fifos(pio, sm);
    pio_sm_set_pins_with_mask(pio, sm, 1u << PLAYER_PIN, 1u << PLAYER_PIN);
}

void player_play(const uint32_t *buf, uint32_t words)
{
    player_stop();
    stream_ptr = buf;

    dma_channel_config c = dma_channel_get_default_config(ch_ctrl);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_FORCE);
    dma_channel_configure(ch_ctrl, &c, &dma_hw->ch[ch_data].al3_read_addr_trig,
                          &stream_ptr, 1, false);

    c = dma_channel_get_default_config(ch_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&c, ch_ctrl);
    dma_channel_configure(ch_data, &c, &pio->txf[sm], buf, words, false);

    pio_sm_init(pio, sm, offset + disk2_offset_start, &cfg);
    dma_channel_start(ch_data);
    pio_sm_set_enabled(pio, sm, true);
}
