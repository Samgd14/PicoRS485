// Host-side stub of the Pico SDK <hardware/dma.h>. Channels are modelled well
// enough to exercise a PIO-paced transfer: the test services them by hand, so a
// channel only moves a word when its DREQ says the FIFO has room or data.
#ifndef _STUB_HARDWARE_DMA_H
#define _STUB_HARDWARE_DMA_H

#include <cstdint>

typedef unsigned int uint;

#define NUM_DMA_CHANNELS 12

enum dma_channel_transfer_size {
    DMA_SIZE_8  = 0,
    DMA_SIZE_16 = 1,
    DMA_SIZE_32 = 2,
};

// The real struct aliases its registers; only these two are read by the driver.
typedef struct {
    uint32_t write_addr;
    uint32_t transfer_count;
} dma_channel_hw_t;
typedef struct {
    enum dma_channel_transfer_size data_size;
    bool read_increment;
    bool write_increment;
    bool enable;
    uint dreq;
} dma_channel_config;

dma_channel_config dma_channel_get_default_config(uint channel);
void channel_config_set_transfer_data_size(dma_channel_config *c,
                                           enum dma_channel_transfer_size size);
void channel_config_set_read_increment(dma_channel_config *c, bool increment);
void channel_config_set_write_increment(dma_channel_config *c, bool increment);
void channel_config_set_dreq(dma_channel_config *c, uint dreq);

int  dma_claim_unused_channel(bool required);
void dma_channel_unclaim(uint channel);
void dma_channel_configure(uint channel, const dma_channel_config *config,
                           volatile void *write_addr, const volatile void *read_addr,
                           uint transfer_count, bool trigger);
void dma_channel_cleanup(uint channel);
void dma_channel_abort(uint channel);
bool dma_channel_is_busy(uint channel);
dma_channel_hw_t *dma_channel_hw_addr(uint channel);

#endif
