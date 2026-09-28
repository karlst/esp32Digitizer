#include <Arduino.h>
#include <version.hpp>

static const int iTestPin = 9;   // Temporary test GPIO; do not connect ADC yet

void setup() {
   Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("s3Recorder starting...");
    Serial.print("CPU frequency: ");
    Serial.print(getCpuFrequencyMhz());
    Serial.println(" MHz");

    Serial.print("Free heap: ");
    Serial.print(ESP.getFreeHeap());
    Serial.println(" bytes");
}

void loop() {
  static unsigned long uHeartbeat = 0;

    Serial.print("Heartbeat ");
    Serial.println(uHeartbeat++);

    delay(1000);
}

