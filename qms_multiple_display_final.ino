#include <Ticker.h>
#include <PxMatrix.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
extern "C" {
  #include <espnow.h>
}
#include <ESP8266WiFi.h>
#include <espnow.h>

// Pin Definition for Nodemcu to HUB75 LED MODULE
#define P_LAT 16  // D0
#define P_A 5     // D1
#define P_B 4     // D2
#define P_C 15    // D8
#define P_OE 2    // D4
#define P_D 12    // D6
#define P_E 0     // Not used
Ticker display_ticker;
PxMATRIX display(512, 32, P_LAT, P_OE, P_A, P_B, P_C, P_D);


uint16_t myRED = display.color565(255, 0, 0);
uint16_t myBLUE = display.color565( 0, 0,255);
// Struct for received patient queue data
typedef struct struct_message {
  int id;
  float temp;
  char msg[32];
} struct_message;

struct SyncRequest {
  char type[10]; // should be "SYNC_REQ"
};
// Known MACs for four doctor nodes
const uint8_t doctorMACs[][6] = {
  {0x78,0x42,0x1C,0x6C,0xA8,0x3C},
  {0x5C,0x01,0x3B,0x98,0x3C,0xEC},
 {0x5C,0x01,0x3B,0x98,0xE8,0x2C},
  {0x78,0x1C,0x3C,0xE5,0x50,0x0C}
};

int docNumbers[4] = {0, 0, 0, 0};

// Flags for safe updating outside ISR
volatile bool newDataAvailable = false;

// Display refresh
void display_updater() {
  display.display(255);
}

// Safe draw call in loop
void drawAllNumbers() {
  display.clearDisplay();
  
  for (int i = 0; i < 4; i++) {
  int baseX = 10 + i * 128;
  int number = docNumbers[i];
   display.setTextSize(2);
   display.setTextColor(myRED);
   display.setCursor(baseX, 8);
   display.print("NEXT");
   display.setTextSize(3);
  int centerX = (number < 10) ? (baseX + 76) : (number < 100) ? (baseX + 66) : (baseX + 56);
    display.setCursor(centerX, 6);
    display.print(number);
    Serial.printf("Doctor %d: %d\n", i + 1, number);
  }

  display.showBuffer();
}

// Receive handler (ISR-safe)
void onReceive(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  if (len == sizeof(int)) {
    int nextPatient;
    memcpy(&nextPatient, incomingData, sizeof(int));

    for (int i = 0; i < 4; i++) {
      if (memcmp(mac, doctorMACs[i], 6) == 0) {
        docNumbers[i] = nextPatient;
        newDataAvailable = true;  // Trigger display update in main loop
        break;
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  display.begin(16);
  display.setTextColor(myRED);
  display.clearDisplay();
  display_ticker.attach(0.015, display_updater);

  WiFi.mode(WIFI_STA);
  Serial.printf("ESP8266 MAC: %s\n", WiFi.macAddress().c_str());
  display.setCursor(5, 1);
  display.setTextSize(2);
  display.print("MEDIBOARDS");
  display.setCursor(5, 17);
  display.print("SONVISAGE");
  display.showBuffer();
  delay(6000);

  display.clearDisplay();
  drawAllNumbers();

  if (esp_now_init() != 0) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onReceive);
}

void loop() {
  if (newDataAvailable) {
    drawAllNumbers();
    newDataAvailable = false;
  }

  // Optional: small delay to reduce CPU usage
  delay(10);
}
