idf_component_register(
    SRCS "inventory_manager.cpp" "inventory_history.cpp" "recipe_matcher.cpp" "category_table.c"
    INCLUDE_DIRS "include"
    REQUIRES json system_services
)
