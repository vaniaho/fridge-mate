idf_component_register(SRCS "task_manager.cpp" "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES ai_agent inventory_system gui_bridge system_services env_sensors web_panel gui_port gui_app fatfs esp_lvgl_port system_manager dashboard)

# 打包内部文件系统数据（含 TTF 字体）到 storage 分区
fatfs_create_spiflash_image(storage ../internal_fs_data FLASH_IN_PROJECT)
