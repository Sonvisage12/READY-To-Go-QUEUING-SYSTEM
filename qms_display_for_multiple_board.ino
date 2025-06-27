#include <PxMatrix.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Ticker.h>


#define P_LAT 16  // D0
#define P_A 5     // D1
#define P_B 4     // D2
#define P_C 15    // D8
#define P_OE 2    // D4
#define P_D 12    // D6
#define P_E 0     // GND (no connection)
// Note: P_E not used (PANELS are 1/16 scan)

PxMATRIX display(512, 32, P_LAT, P_OE, P_A, P_B, P_C, P_D);

uint16_t myRED = display.color565(255, 0, 0);

// Known MACs for four doctor nodes
const uint8_t doctorMACs[][6] = {
    {0x78,0x42,0x1C,0x6C,0xA8,0x3C},
    {0x5C,0x01,0x3B,0x98,0x3C,0xEC},
    {0x6C,0xC8,0x40,0x06,0x2C,0x8C},
    {0x78,0x1C,0x3C,0xE5,0x50,0x0C}
};
int docNumbers[4] = {0, 0, 0, 0};  // latest numbers from each doctor

Ticker display_ticker;  // for refreshing the display

// ISR: called by ticker to refresh the display
void display_updater() {
  display.display(90);  // multiplex with ~70µs refresh (adjust as needed)
}

// ESP-NOW receive callback
void onReceive(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  Serial.print("Received ESP-NOW from MAC: ");
  for (int i = 0; i < 6; i++) Serial.printf("%02X%s", mac[i], i < 5 ? ":" : "\n");

  // Case 1: If message is just an int
  if (len == sizeof(int)) {
    int nextPatient;
    memcpy(&nextPatient, incomingData, sizeof(int));

    for (int i = 0; i < 4; i++) {
      if (memcmp(mac, doctorMACs[i], 6) == 0) {
        docNumbers[i] = nextPatient;
        Serial.printf(" -> Doctor %d number updated to %d\n", i + 1, nextPatient);
        drawAllNumbers();
        break;
      }
    }
  }

  // ✅ Case 2: Full QueueItem struct (Doctor sends full packet instead of just int)
  else if (len == sizeof(QueueItem)) {
    QueueItem item;
    memcpy(&item, incomingData, sizeof(item));
    
    int doctorIdx = item.node - 1;
    if (doctorIdx >= 0 && doctorIdx < 4) {
      docNumbers[doctorIdx] = item.number;
      Serial.printf(" -> Doctor %d number updated (via QueueItem) to %d\n", doctorIdx + 1, item.number);
      drawAllNumbers();
    }
  }

  else {
    Serial.println("❌ Unknown packet length. Ignored.");
  }
}



// Draw all four "NEXT n" lines on the display
void drawAllNumbers() {
  display.clearDisplay();
  display.setTextSize(3);
  display.setTextColor(myRED);
// for (int i = 0; i < 4; i++) {
//   int x = 2 + i * 128;  // horizontal offset for each doctor (8px margin)
//   display.setCursor(x, 5);  // Fixed Y for middle alignment
//   display.print("NEXT");
//   display.print(docNumbers[i]);
// }

for (int i = 0; i < 4; i++) {
  int baseX = 0 + i * 128;  // base offset per doctor column
  int number = docNumbers[i];

  // Draw label
  display.setCursor(baseX, 5);
  display.print("NEXT");

  // Determine horizontal shift for number
  int digitCount = (number < 10) ? 1 : (number < 100) ? 2 : 3;
  int digitWidth = 12;  // Approximate width per digit (depends on font/size)
  int totalWidth = digitCount * digitWidth;
int centerX = (number < 10) ?(baseX + 90) : (number < 100) ? (baseX + 87) : (baseX + 76);
  //int centerX = baseX + 73;  // center number in 128px-wide column// 101, 

  // Draw number
  display.setCursor(centerX, 5);  // draw number below "NEXT"
  display.print(number);
}

  display.showBuffer();
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);

  // Initialize the display for 128x128 with two panels horizontally
  display.begin(16);           // 1/16 scan pattern (for 64x32 panels)
  display.setPanelsWidth(2);   // chain two 64x32 panels = 128px wide
  display.clearDisplay();
  display_ticker.attach(0.01, display_updater);  // refresh at ~100Hz

  // Print own MAC for reference
  Serial.print("Receiver MAC: ");
  Serial.println(WiFi.macAddress());
  drawAllNumbers();
  // Initialize ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed!");
    while (true) { delay(1000); }
  }
  esp_now_register_recv_cb(onReceive);
}

void loop() {
  //drawAllNumbers();
  // (No delay; drawing happens continuously)
}
