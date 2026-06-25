set(_ws_source
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_websocket_client/esp_websocket_client.c")

if(EXISTS "${_ws_source}")
    file(READ "${_ws_source}" _ws_content)
    set(_ws_original "${_ws_content}")

    string(REPLACE
        "    esp_websocket_event_data_t event_data;"
        "    /* Smart Fridge compatibility: clear optional diagnostics. */\n    esp_websocket_event_data_t event_data = {0};"
        _ws_content "${_ws_content}")

    if(NOT _ws_content MATCHES
       "Smart Fridge compatibility: preserve esp-tls diagnostics before close")
        string(REPLACE
            "    esp_transport_close(client->transport);\n\n    if (!client->config->auto_reconnect)"
            "    /* Smart Fridge compatibility: preserve esp-tls diagnostics before close. */\n    client->error_handle.error_type = error_type;\n    esp_websocket_client_dispatch_event(client, WEBSOCKET_EVENT_DISCONNECTED, NULL, 0);\n\n    esp_transport_close(client->transport);\n\n    if (!client->config->auto_reconnect)"
            _ws_content "${_ws_content}")
    endif()

    string(REPLACE
        "    client->error_handle.error_type = error_type;\n    esp_websocket_client_dispatch_event(client, WEBSOCKET_EVENT_DISCONNECTED, NULL, 0);\n\ncleanup:"
        "cleanup:"
        _ws_content "${_ws_content}")

    string(REPLACE
        "    size_t needed_size = vsnprintf(NULL, 0, format, myargs);\n    needed_size++; // null terminator"
        "    va_list measure_args;\n    va_copy(measure_args, myargs);\n    size_t needed_size = vsnprintf(NULL, 0, format, measure_args);\n    va_end(measure_args);\n    needed_size++; // null terminator"
        _ws_content "${_ws_content}")

    if(NOT _ws_content STREQUAL _ws_original)
        file(WRITE "${_ws_source}" "${_ws_content}")
        message(STATUS
            "Applied esp_websocket_client 1.7.0 diagnostic compatibility patch")
    endif()
endif()
