#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
// Set the LCD I2C address (commonly 0x27 or 0x3F)
LiquidCrystal_I2C lcd(0x27, 16, 2); 
// ==== 🔥 Define QueueItem ====
struct QueueItem {
  char uid[20];
  char type[10]; 
  char timestamp[25];
  int number;
  int node;
  bool removeFromQueue;
  bool addToQueue;
  char queueID[10];
  char sourceMAC[18];
};

#define GREEN_LED_PIN 2
#define RED_LED_PIN  15
#define RST_PIN       5
#define SS_PIN        4
#define BLUE_LED_PIN   0
MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences preferences;

uint8_t myMAC[6];
int patientNum = 0;
QueueItem currentPatient;

// Node ID unique to this node (adjust this for each node: e.g., 1, 2, 3, 4)
const int nodeID = 2;
//uint8_t displayMAC[] = {0x84, 0xCC, 0xA8, 0xA4, 0x64, 0xB8};
uint8_t displayMAC[] = {0x68, 0xC6, 0x3A, 0xFC, 0x61, 0x3E};
//uint8_t displayMAC[] = {0xA4, 0xCF, 0x12, 0xF1, 0x6B, 0xA5};
//uint8_t displayMAC[] = {0x8C, 0xAA, 0xB5, 0x53, 0x09, 0xE5};

uint8_t peer3[] = {0x78, 0x42, 0x1C, 0x6C, 0xE4, 0x9C};
uint8_t peer1[] = {0x78, 0x1C, 0x3C, 0x2D, 0xA2, 0xA4};
uint8_t peer5[] = {0x00, 0x4B, 0x12, 0x97, 0x2E, 0xA4};
uint8_t peer4[] = {0x78, 0x1C, 0x3C, 0xE6, 0x6C, 0xB8};
uint8_t peer2[] = {0x78, 0x1C, 0x3C, 0xE3, 0xAB, 0x30};
uint8_t peer6[] = {0x5C, 0x01, 0x3B, 0x98, 0xDB, 0x04};
uint8_t peer7[] = {0x5C, 0x01, 0x3B, 0x97, 0x54, 0xB4};
std::vector<uint8_t*> arrivalNodes = { peer1, peer2, peer3,peer4, peer5, peer6, peer7  };

// const uint8_t arrivalMACs[][6] = {
//     {0x08, 0xD1, 0xF9, 0xD7, 0x50, 0x98},
//     {0x78, 0x1C, 0x3C, 0x2D, 0xA2, 0xA4},//78:1C:3C:2D:A2:A4
//     {0x00, 0x4B, 0x12, 0x97, 0x2E, 0xA4},//00:4B:12:97:2E:A4
//     {0x78, 0x1C, 0x3C, 0xE6, 0x6C, 0xB8},//78:1C:3C:E6:6C:B8
//     {0x78, 0x1C, 0x3C, 0xE3, 0xAB, 0x30},//78:1C:3C:E3:AB:30
//     {0x5C, 0x01, 0x3B, 0x98, 0xDB, 0x04},//5C:01:3B:98:DB:04
//     {0x78, 0x42, 0x1C, 0x6C, 0xE4, 0x9C}//78:42:1C:6C:E4:9C
// };




int currentArrivalIndex = 0;
bool waitingForPatient = false;
bool patientReady = false;

void sendRequestToArrival() {
  QueueItem requestItem;
  strncpy(requestItem.uid, "REQ_NEXT", sizeof(requestItem.uid));
  strncpy(requestItem.timestamp, "", sizeof(requestItem.timestamp));
  requestItem.number = 0;
  requestItem.node = nodeID;  // Specify requesting node ID
  requestItem.removeFromQueue = false;
  requestItem.addToQueue = false;

  esp_now_send(arrivalNodes[currentArrivalIndex], (uint8_t*)&requestItem, sizeof(requestItem));
  Serial.printf("📤 Sent REQ_NEXT to Arrival Node %d\n", currentArrivalIndex + 1);
  waitingForPatient = true;
}


void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  Serial.printf("📥 Received from ");
  for (int i = 0; i < 6; i++) {
    Serial.printf("%02X", info->src_addr[i]);
    if (i < 5) Serial.print(":");
  }
  Serial.printf(" | Length: %d\n", len);

  if (len == sizeof(QueueItem)) {
    QueueItem receivedItem;
    memcpy(&receivedItem, data, sizeof(QueueItem));

if (strcmp(receivedItem.uid, "MASTER_CHANGE") == 0) {
        // (Optionally, also check receivedItem.type == "MASTER" if used)
        Serial.printf("🔄 Master change notification received! New master index: %d, MAC: %s\n",
                      receivedItem.number, receivedItem.sourceMAC);
        // Update the target arrival node index (and optionally MAC)
        currentArrivalIndex = receivedItem.number;
        // (Optional) If not already in peer list, add the new master's MAC as peer. 
        // In this case, all arrival nodes are likely pre-added to peers.

        waitingForPatient = false;   // reset any waiting state
        patientReady = false;
        // Optionally, immediately send a new request to the new master if a patient was being requested:
        // sendRequestToArrival();

        return;  // Exit here, do not process further as normal patient data
    }


    // 🔥 Check if intended for this node
    if (receivedItem.node != nodeID) {
      Serial.printf("🚫 Ignored message intended for Node %d (This is Node %d).\n", receivedItem.node, nodeID);
      return;
    }

    // 🔥 Ignore if this message is meant to add a patient
    if (receivedItem.addToQueue) {
      Serial.println("🚫 Ignored message meant for adding a patient to the queue.");
      return;
    }

    patientNum = receivedItem.number;
    currentPatient = receivedItem;

    if (patientNum == 0) {
      Serial.println("⚠️ Queue empty. No patient assigned.");
      esp_now_send(displayMAC, (uint8_t*)&patientNum, sizeof(patientNum));lcd.clear();
       lcd.setCursor(0, 0); // First column, first row
       lcd.print("NEXT PATIENT");
       lcd.setCursor(7, 1); // First column, second row
       lcd.print(patientNum);
      waitingForPatient = false;
      patientReady = false;
    } else {
      Serial.printf("🔎 Patient Data: UID: %s | Number: %d\n", currentPatient.uid, patientNum);
      esp_now_send(displayMAC, (uint8_t*)&patientNum, sizeof(patientNum));lcd.clear();
       lcd.setCursor(0, 0); // First column, first row
       lcd.print("NEXT PATIENT");
       lcd.setCursor(7, 1); // First column, second row
       lcd.print(patientNum);
      Serial.printf("📤 Sent Patient Number %d to Display Node\n", patientNum);
      waitingForPatient = false;
      patientReady = true;
    }
  } else if (len == sizeof(int)) {
    memcpy(&patientNum, data, sizeof(int));
    Serial.printf("🔎 Received patient number directly: %d\n", patientNum);

    esp_now_send(displayMAC, (uint8_t*)&patientNum, sizeof(patientNum));lcd.clear();
 lcd.setCursor(0, 0); // First column, first row
  lcd.print("NEXT PATIENT");
     lcd.setCursor(7, 1); // First column, second row
     lcd.print(patientNum);
    Serial.printf("📤 Sent Patient Number %d to Display Node\n", patientNum);

    waitingForPatient = false;
    patientReady = (patientNum != 0);
    if (patientNum == 0) {
      Serial.println("⚠️ Queue empty. No patient assigned.");
    }
  } else {
    Serial.println("⚠️ Unexpected data length or format. Ignored.");
    int zero = 0;
    esp_now_send(displayMAC, (uint8_t*)&zero, sizeof(zero));lcd.clear();
 lcd.setCursor(0, 0); // First column, first row
  lcd.print("NO PATIENT");
     lcd.setCursor(7, 1); // First column, second row
       lcd.print("QUEUE IS EMPTY");
  }
}

void setup() {
  Serial.begin(115200);
  SPI.begin();
  mfrc522.PCD_Init();
 Wire.begin(21, 22);  // SDA = GPIO 21, SCL = GPIO 22 (default for ESP32)
  lcd.init();          // Initialize the LCD
  lcd.backlight();     // Turn on backlight

  lcd.setCursor(0, 0); // First column, first row
  lcd.print("NEXT PATIENT");

  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(BLUE_LED_PIN, OUTPUT);
  WiFi.mode(WIFI_STA);
  WiFi.macAddress(myMAC);
 digitalWrite( BLUE_LED_PIN, HIGH);
  digitalWrite( RED_LED_PIN,LOW);
   digitalWrite( GREEN_LED_PIN, LOW);
  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ ESP-NOW Init Failed");
    return;
  }

  esp_now_register_recv_cb(onDataRecv);

  for (auto mac : arrivalNodes) {
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 1;
    peerInfo.encrypt = false;
    if (!esp_now_is_peer_exist(mac)) esp_now_add_peer(&peerInfo);
  }

  esp_now_peer_info_t displayPeer = {};
  memcpy(displayPeer.peer_addr, displayMAC, 6);
  displayPeer.channel = 1;
  displayPeer.encrypt = false;
  if (!esp_now_is_peer_exist(displayMAC)) esp_now_add_peer(&displayPeer);

  Serial.printf("👨‍⚕️ Doctor Node %d Ready\n", nodeID);
  sendRequestToArrival();
}

void loop() {
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

  String uid = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
  uid.toUpperCase();

  if (patientReady && uid == String(currentPatient.uid)) {
    Serial.printf("✅ Patient %d attended\n", patientNum);

    currentPatient.removeFromQueue = true;
    esp_now_send(arrivalNodes[currentArrivalIndex], (uint8_t*)&currentPatient, sizeof(currentPatient));
    delay(100);
   
    currentArrivalIndex = 0; patientReady = false;
    sendRequestToArrival();
    blinkLED(GREEN_LED_PIN);
  } else {
    Serial.println("❌ Access Denied");
    blinkLED(RED_LED_PIN);
  }

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(1000);
}

String getUIDString(byte *buffer, byte bufferSize) {
  String uid = "";
  for (byte i = 0; i < bufferSize; i++) {
    if (buffer[i] < 0x10) uid += "0";
    uid += String(buffer[i], HEX);
  }
  return uid;
}
void blinkLED(int pin) {  digitalWrite( BLUE_LED_PIN, LOW);
  digitalWrite(pin, HIGH);
  delay(500);
  digitalWrite(pin, LOW);
  digitalWrite( BLUE_LED_PIN, HIGH);
}

