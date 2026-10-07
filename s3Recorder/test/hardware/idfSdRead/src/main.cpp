/**
 * @file main.cpp
 * @brief Run the unchanged read-only comparison using the newer ESP-IDF runtime.
 */
void setup();
void loop();

/**
 * @brief ESP-IDF entry point; call shared startup once and repeat its command loop.
 * The default main task owns every buffer and SD operation. loop() yields using
 * FreeRTOS, and runs a new comparison only when its existing logic requests it.
 */
extern "C" void app_main()
{
    setup();
    for (;;)
    {
        loop();
    }
}
