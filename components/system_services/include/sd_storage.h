#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SD card hardware and mount FATFS filesystem
 * Mount point: /sdcard
 * Uses SPI mode with LDO channel 4 for I/O pull-ups
 * @return 0 on success, -1 on failure
 */
int sd_card_init(void);

#ifdef __cplusplus
}
#endif
