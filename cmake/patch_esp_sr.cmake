set(_esp_sr_movemodel
    "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-sr/model/movemodel.py")

if(EXISTS "${_esp_sr_movemodel}")
    file(READ "${_esp_sr_movemodel}" _esp_sr_content)
    set(_esp_sr_original "${_esp_sr_content}")

    if(NOT _esp_sr_content MATCHES "SDKCONFIG_ENCODING = \"utf-8\"")
        string(REPLACE
            "from pack_model import pack_models\n"
            "from pack_model import pack_models\n\nSDKCONFIG_ENCODING = \"utf-8\"\n"
            _esp_sr_content "${_esp_sr_content}")
    endif()

    string(REPLACE
        "io.open(sdkconfig_path, \"r\")"
        "io.open(sdkconfig_path, \"r\", encoding=SDKCONFIG_ENCODING)"
        _esp_sr_content "${_esp_sr_content}")

    if(NOT _esp_sr_content STREQUAL _esp_sr_original)
        file(WRITE "${_esp_sr_movemodel}" "${_esp_sr_content}")
        message(STATUS
            "Applied esp-sr movemodel.py UTF-8 compatibility patch")
    endif()
endif()
