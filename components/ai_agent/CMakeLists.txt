idf_component_register(
    SRCS "llm_client.cpp"
         "prompt_builder.cpp"
         "ai_worker.cpp"
         "response_parser.cpp"
         "response_executor.cpp"
         "llm_context.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls json inventory_system system_services main freertos
)
