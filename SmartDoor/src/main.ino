#include <Arduino.h>
#include "FingerprintDoor.h"

void setup() {
  fingerprintDoorSetup();   // gọi setup của thư viện
}

void loop() {
  fingerprintDoorLoop();    // gọi loop của thư viện