#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>

static const char *TAG = "SD_Init";

#define MOUNT_POINT "/sdcard"

// SD 卡 SPI 模式引脚映射 (与原 SDMMC 模式共用相同的物理接线)
// 原 SDMMC CMD  (GPIO44) → SPI MOSI
// 原 SDMMC D0   (GPIO39) → SPI MISO
// 原 SDMMC CLK  (GPIO43) → SPI SCLK
// 原 SDMMC D3   (GPIO42) → SPI CS
#define PIN_NUM_MOSI  44
#define PIN_NUM_MISO  39
#define PIN_NUM_CLK   43
#define PIN_NUM_CS    42

int sd_card_init(void) {
    esp_err_t ret;

    // FATFS 挂载配置
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, // 如果挂载失败自动格式化为 FAT32
        .max_files = 5,                 // 允许同时打开的最大文件数
        .allocation_unit_size = 4096    // 4K 簇大小，减少 FAT 表异常概率
    };

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;

    ESP_LOGI(TAG, "Initializing SD card via SPI");

    // ====================================================================
    // 关键：使能 ESP_LDO_VO4 (3.3V)
    // 根据原理图，SD 卡数据线的上拉电阻 R5~R10 连接到 ESP_LDO_VO4。
    // 如果不使能此 LDO，上拉电阻无供电，SD 卡信号线浮空导致通信超时。
    // ====================================================================
    esp_ldo_channel_handle_t ldo_handle = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = 4,
        .voltage_mv = 3300,
    };
    ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable LDO channel 4 for SD card I/O (%s).", esp_err_to_name(ret));
        return -1;
    }
    ESP_LOGI(TAG, "LDO channel 4 enabled at 3.3V for SD card I/O pull-ups");

    // 根据原理图，GPIO45 控制 SD 卡电源，低电平导通 (P-MOS AO3401 Q1)
    gpio_config_t pwr_conf = {
        .pin_bit_mask = (1ULL << GPIO_NUM_45),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pwr_conf);
    gpio_set_level(GPIO_NUM_45, 0); // 开启 SD 卡电源
    vTaskDelay(pdMS_TO_TICKS(100)); // 等待上电稳定

    // 配置 SPI 总线 (使用独立的 SPI2 外设，与 ESP-Hosted 的 SDMMC 外设完全隔离)
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus (%s).", esp_err_to_name(ret));
        return -1;
    }

    // 使用 SPI 主机接口驱动 SD 卡
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SPI2_HOST;

    ESP_LOGI(TAG, "Mounting filesystem to %s", mount_point);
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s).", esp_err_to_name(ret));
        }
        spi_bus_free(SPI2_HOST);
        return -1;
    }

    ESP_LOGI(TAG, "Filesystem mounted successfully.");
    // 打印 SD 卡基本信息
    sdmmc_card_print_info(stdout, card);

    // 写入测试：确认文件系统可写，尽早发现只读/写保护问题
    const char *test_path = "/sdcard/.write_test";
    FILE *f = fopen(test_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Write test FAILED: fopen(%s) errno=%d (%s)",
                 test_path, errno, strerror(errno));
        ESP_LOGE(TAG, "Possible causes:");
        ESP_LOGE(TAG, "  1. SD card write-protect tab is ON (physical slider on card)");
        ESP_LOGE(TAG, "  2. Filesystem mounted as read-only");
        ESP_LOGE(TAG, "  3. SD card is damaged");
        return -1;
    }
    fclose(f);
    remove(test_path);
    ESP_LOGI(TAG, "Write test passed. SD card is writable.");

    return 0;
}
