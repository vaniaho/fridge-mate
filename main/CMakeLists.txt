idf_component_register(SRCS "task_manager.cpp" "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES ai_agent inventory_system gui_bridge system_services env_sensors web_panel)
