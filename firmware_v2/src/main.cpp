#include <Arduino.h>

#include "App.h"

namespace {
App app;
}

void setup() {
    Serial.begin(115200);
    delay(250);
    app.begin();
}

void loop() {
    app.update();
    delay(50);
}
