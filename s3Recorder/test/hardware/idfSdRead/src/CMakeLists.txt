# Compile exactly the same comparison logic used by the Arduino diagnostic.
idf_component_register(SRCS "main.cpp" "diagnosticSerial.cpp" "../../sdCardDiagnostic.cpp"
    INCLUDE_DIRS "." REQUIRES sdmmc esp_driver_sdmmc esp_driver_uart)
