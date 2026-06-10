idf_component_register(SRCS "web_server.cpp" "api_routes.cpp"
                    INCLUDE_DIRS "include"
                    REQUIRES esp_http_server json inventory_system ai_agent system_services esp_wifi
                    EMBED_TXTFILES "../../web_frontend/index.html"
                                   "../../web_frontend/style.css"
                                   "../../web_frontend/app.js")
