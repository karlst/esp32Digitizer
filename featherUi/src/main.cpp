#include <Arduino.h>

static const int iLedPin = 13;

void setup()
{
    Serial.begin(115200);
    delay(1000);

    pinMode(iLedPin, OUTPUT);
    digitalWrite(iLedPin, LOW);

    Serial.println();
    Serial.println("featherUi starting...");
}

void loop()
{
    static unsigned long uHeartbeat = 0;
    static bool bLedOn = false;

    bLedOn = !bLedOn;
    digitalWrite(iLedPin, bLedOn ? HIGH : LOW);

    Serial.print("Heartbeat ");
    Serial.println(uHeartbeat);

    uHeartbeat++;
    delay(1000);
}

