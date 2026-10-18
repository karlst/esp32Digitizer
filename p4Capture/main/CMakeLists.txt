idf_component_register(SRCS "main.cpp" "frameDecoder.cpp" "sdRecorder.cpp" "sdWriteTest.cpp"
    "../../s3Recorder/lib/bufferedWriter/src/bufferedWriter.cpp"
    INCLUDE_DIRS "." "../../shared" "../../s3Recorder/lib/bufferedWriter/src"
    REQUIRES esp_driver_parlio esp_driver_uart esp_driver_gpio esp_timer fatfs sdmmc esp_driver_sdmmc esp_psram esp_rom esp_hw_support)
target_compile_definitions(${COMPONENT_LIB} PRIVATE ADS127_P4_ENDURANCE=1
    BUFFERED_WRITER_EXTERNAL=1 BUFFERED_WRITER_CAPACITY=4194304 BUFFERED_WRITER_BLOCK_BYTES=131072)
