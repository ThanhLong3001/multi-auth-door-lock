#ifndef FINGERPRINT_DOOR_H
#define FINGERPRINT_DOOR_H

#include <Arduino.h>

// ĐỊNH NGHĨA TEMPLATE BLYNK 
#define BLYNK_TEMPLATE_ID   "TMPL6eFT6Wg-0"
#define BLYNK_TEMPLATE_NAME "Smart Door"
#define BLYNK_AUTH_TOKEN    "T9qbNcjUSZd5_VNayw-Ocp1PVn8xvFN7"
#define BLYNK_PRINT Serial

// THƯ VIỆN DÙNG CHUNG 
#include <Wire.h>
#include <SPI.h>
#include <Preferences.h>
#include <MFRC522.h>
#include <Adafruit_Fingerprint.h>
#include <LiquidCrystal_I2C.h>
#include <esp_task_wdt.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

// Ưi-Fi , Blynk
extern char auth[];
extern char ssid[];
extern char pass[];

// 2 hàm “API” 
void fingerprintDoorSetup();
void fingerprintDoorLoop();

#endif
