idf_component_register(SRCS "gui_app.cpp" "gui_launcher.cpp" "gui_standby.cpp" "gui_overlays.cpp" "app_inventory.cpp" "app_recipes.cpp" "app_settings.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES log lvgl gui_port inventory_system gui_bridge system_manager)
