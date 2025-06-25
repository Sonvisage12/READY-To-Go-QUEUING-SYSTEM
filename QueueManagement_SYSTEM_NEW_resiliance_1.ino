#include <SPI.h>
#include <MFRC522.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "PatientRFIDMappings.h"
#include <esp_now.h>
#include <WiFi.h>
#include <map>
#include <vector>
#include <algorithm>
#include "SharedQueue.h"
#include <Wire.h>
#include "RTClib.h"
#define IR_SENSOR_PIN 16  // GPIO 0
#include <ESP32Servo.h>
#define SERVO_PIN 26
Servo myServo;
bool isOpen = false;
#define RST_PIN  5
#define SS_PIN   4
#define GREEN_LED_PIN 15
#define RED_LED_PIN   2
//#define BLUE_LED_PIN   
#define BUZZER   27
MFRC522 mfrc522(SS_PIN, RST_PIN);
Preferences prefs;
bool waitingForCard = false;
SharedQueue sharedQueue("rfid-patients");
SharedQueue sharedQueueA("queue-A");
SharedQueue sharedQueueB("queue-B");
SharedQueue sharedQueueC("queue-C");

RTC_DS3231 rtc;
bool isArrivalNode = false, isDoctorNode = false;
std::map<String, unsigned long> recentUIDs;
const unsigned long UID_CACHE_TTL_MS = 2000;
const uint8_t arrivalMACs[][6] = {
    
    {0x78, 0x1C, 0x3C, 0x2D, 0xA2, 0xA4},
    {0x00, 0x4B, 0x12, 0x97, 0x2E, 0xA4},  //78:1C:3C:2D:A2:A4
    {0x5C, 0x01, 0x3B, 0x97, 0x54, 0xB4}, //00:4B:12:97:2E:A4
    {0x78, 0x1C, 0x3C, 0xE6, 0x6C, 0xB8}, //78:1C:3C:E6:6C:B8
    {0x78, 0x1C, 0x3C, 0xE3, 0xAB, 0x30}, //78:1C:3C:E3:AB:30
    {0x5C, 0x01, 0x3B, 0x98, 0xDB, 0x04}, //5C:01:3B:98:DB:04
    {0x78, 0x42, 0x1C, 0x6C, 0xE4, 0x9C} //78:42:1C:6C:E4:9C
};
const int numArrivalNodes = sizeof(arrivalMACs) / sizeof(arrivalMACs[0]);

const uint8_t doctorMACs[][6] = {
    {0x78, 0x42, 0x1C, 0x6C, 0xA8, 0x3C},
    {0x5C, 0x01, 0x3B, 0x98, 0x3C, 0xEC},
    {0x6C, 0xC8, 0x40, 0x06, 0x2C, 0x8C},
    {0x78, 0x1C, 0x3C, 0xE5, 0x50, 0x0C}
};
void broadcastToArrivalNodes(const QueueItem &item) {
    for (int i = 0; i < numArrivalNodes; i++) {
        esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
    }
}

void cleanupRecentUIDs() {
    unsigned long now = millis();
    for (auto it = recentUIDs.begin(); it != recentUIDs.end(); ) {
        if (now - it->second > UID_CACHE_TTL_MS)
            it = recentUIDs.erase(it);
        else
            ++it;
    }
}

void handleQueuePlacement(String uid, String timestamp, int number) {
    if (sharedQueueA.exists(uid)) {
        sharedQueueA.removeByUID(uid);
        sharedQueueB.add(uid, timestamp, number);
        Serial.println("🔄 UID moved from SharedQueueA to SharedQueueB.");
    } else if (sharedQueue.exists(uid) || sharedQueueB.exists(uid)) {
        Serial.println("⚠️ UID already in a queue. Skipping addition.");
    } else {
        sharedQueue.add(uid, timestamp, number);
        Serial.println("✅ UID added to SharedQueue.");
    }

    sharedQueue.print(); sharedQueueA.print(); sharedQueueB.print(); sharedQueueC.print();
}


void handleQueuePlacement1(String uid, String timestamp, int number, String queueID) {
    if (queueID == "queueA") {
        // move from SharedQueue to A (doctor call)
        if (sharedQueue.exists(uid)) {
            sharedQueue.removeByUID(uid);
            sharedQueueA.add(uid, timestamp, number);
            Serial.println("🟡 UID moved from SharedQueue to SharedQueueA.");
        } else {
            Serial.println("⚠️ UID not in SharedQueue; cannot move to A.");
        }

    } else if (queueID == "queueB") {
        // move from A to B (patient re-registers)
        if (sharedQueueA.exists(uid)) {
            sharedQueueA.removeByUID(uid);
            sharedQueueB.add(uid, timestamp, number);
            Serial.println("🔵 UID moved from SharedQueueA to SharedQueueB.");
        } else {
            Serial.println("⚠️ UID not in SharedQueueA; cannot move to B.");
        }

    } else if (queueID == "mix") {
        // C is a mixed display queue — just add if not present
        if (!sharedQueueC.exists(uid)) {
            sharedQueueC.add(uid, timestamp, number);
            Serial.println("🟢 UID added to SharedQueueC (mixed queue).");
        }

    } else {
        // Default: add to SharedQueue if not already in A or B
        if (!sharedQueue.exists(uid) && !sharedQueueA.exists(uid) && !sharedQueueB.exists(uid)) {
            sharedQueue.add(uid, timestamp, number);
            Serial.println("✅ UID added to SharedQueue (initial).");
        } else {
            Serial.println("⚠️ UID already in another queue; not added to SharedQueue.");
        }
    }

    // Optional: Print all queues
    printAllQueues();
}


void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *incomingData, int len) {
    QueueItem item;
    memcpy(&item, incomingData, sizeof(item));

    const uint8_t* mac = recvInfo->src_addr;
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.print("📩 Received from: "); Serial.println(macStr);

    isArrivalNode = isDoctorNode = false;
    for (int i = 0; i < numArrivalNodes; i++)
        if (memcmp(mac, arrivalMACs[i], 6) == 0) { isArrivalNode = true; break; }
    for (int i = 0; i < 4; i++)
        if (memcmp(mac, doctorMACs[i], 6) == 0) { isDoctorNode = true; break; }

    // Only handle messages from arrival nodes
    if (!isArrivalNode) return;

    Serial.println("🔄 Handling Arrival Node message...");
    Serial.printf("Flags: addToQueue=%d, removeFromQueue=%d\n", item.addToQueue, item.removeFromQueue);
    Serial.printf("UID: %s | Timestamp: %s | Number: %d\n", item.uid, item.timestamp, item.number);

    String uidStr = String(item.uid);
    cleanupRecentUIDs();

    if (item.addToQueue && !item.removeFromQueue) {
        if (recentUIDs.count("add:" + uidStr)) {
            Serial.println("⏳ UID recently added, skipping to prevent rebroadcast loop.");
        } else {
            recentUIDs["add:" + uidStr] = millis();
            handleQueuePlacement1(uidStr, String(item.timestamp), item.number, String(item.queueID));
        }

    } else if (!item.addToQueue && item.removeFromQueue) {
        if (recentUIDs.count("rem:" + uidStr)) {
            Serial.println("⏳ UID recently removed, skipping to prevent rebroadcast loop.");
        } else {
            recentUIDs["rem:" + uidStr] = millis();

            if (sharedQueue.exists(uidStr)) {
                sharedQueue.removeByUID(uidStr);
                sharedQueueC.removeByUID(uidStr);
                sharedQueueA.add(uidStr, String(item.timestamp), item.number);
                Serial.printf("🗑️ UID %s removed from SharedQueue and added to SharedQueueA.\n", item.uid);
            } else {
                sharedQueueC.removeByUID(uidStr);
                sharedQueueB.removeByUID(uidStr);
                Serial.printf("🗑️ UID %s removed from SharedQueueC and SharedQueueB.\n", item.uid);
            }
        }
    }

    // Always print queues to see latest state
    printAllQueues();
}



int getPermanentNumber(String uid) {
    prefs.begin("rfidMap", true); int pid=-1;
    if (prefs.isKey(uid.c_str())) pid = prefs.getUInt(uid.c_str(),-1);
    prefs.end(); return pid;
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Sent 🟢" : "Failed 🔴");
}

void processCard1(String uid) {
    DateTime now = rtc.now();
    char timeBuffer[25];
    snprintf(timeBuffer, sizeof(timeBuffer), "%04d-%02d-%02d %02d:%02d:%02d",
             now.year(), now.month(), now.day(), now.hour(), now.minute(), now.second());
    String timeStr = String(timeBuffer);

    int pid = getPermanentNumber(uid);
    if (pid == -1) pid = 0;

    QueueItem item;
    strncpy(item.uid, uid.c_str(), sizeof(item.uid));
    strncpy(item.timestamp, timeStr.c_str(), sizeof(item.timestamp));
    item.number = pid;
    item.addToQueue = true; item.removeFromQueue = false;

    if (sharedQueueA.exists(uid)) {
        
       esp_now_send(arrivalMACs[0], (uint8_t*)&item, sizeof(item));
        Serial.println("🔄 UID moved from SharedQueueA to SharedQueueB.");
        blinkLED(GREEN_LED_PIN);  // Success indicator digitalWrite(GREEN_LED_PIN, LOW );
    } else if (sharedQueue.exists(uid) || sharedQueueB.exists(uid)) {
        Serial.println("⚠️ UID already in queue.");
        blinkLED(RED_LED_PIN);  // Warning indicator
    } else {
        esp_now_send(arrivalMACs[0], (uint8_t*)&item, sizeof(item));
        Serial.println("✅ UID added to SharedQueue.");
        blinkLED(GREEN_LED_PIN);  // Success indicator
    }
    sharedQueue.print();sharedQueueA.print();sharedQueueB.print();sharedQueueC.print();
}


void printAllQueues() {
    Serial.println("📋 All Queues:");
    Serial.print("🔸 sharedQueue: "); sharedQueue.print();
    Serial.print("🔸 sharedQueueA: "); sharedQueueA.print();
    Serial.print("🔸 sharedQueueB: "); sharedQueueB.print();
    Serial.print("🔸 sharedQueueC: "); sharedQueueC.print();
}

void clearAllQueues() {
    sharedQueue.clear(); sharedQueueA.clear(); sharedQueueB.clear(); sharedQueueC.clear();
    Serial.println("🔄 All queues cleared.");
}


 void setup() {
    Serial.begin(115200);
    prefs.begin("rfidMap", false);
    loadRFIDMappings(prefs);  
     pinMode(IR_SENSOR_PIN, INPUT);
  myServo.attach(SERVO_PIN, 500, 2500);  // Correct for ESP32 + SG90
  myServo.write(0);  // Start at 0 degrees
   clearAllQueues();
   
    SPI.begin(); WiFi.mode(WIFI_STA);
    Serial.print("WiFi MAC: "); Serial.println(WiFi.macAddress());

    mfrc522.PCD_Init();
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(BUZZER, OUTPUT);
    pinMode(RED_LED_PIN, OUTPUT);
   //  pinMode(BLUE_LED_PIN, OUTPUT);
    digitalWrite(GREEN_LED_PIN, LOW );
    digitalWrite(RED_LED_PIN, LOW);
    //digitalWrite(BLUE_LED_PIN, HIGH);
    if (!rtc.begin()) {
        Serial.println("❌ Couldn't find RTC module! Check wiring.");
        while (1);
    }

    if (rtc.lostPower()) {
        Serial.println("⚠️ RTC lost power, setting to compile time.");
        rtc.adjust(DateTime(__DATE__, __TIME__));
    }

    if (esp_now_init() != ESP_OK) {
        Serial.println("❌ ESP-NOW Init Failed");
        return;
    }

    for (int i = 0; i < numArrivalNodes; i++) {
        esp_now_peer_info_t p = {};
        memcpy(p.peer_addr, arrivalMACs[i], 6);
        p.channel = 1;
        esp_now_add_peer(&p);
    }

    for (int i = 0; i < 4; i++) {
        esp_now_peer_info_t p = {};
        memcpy(p.peer_addr, doctorMACs[i], 6);
        p.channel = 1;
        esp_now_add_peer(&p);
    }

    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    sharedQueue.load();
    sharedQueueA.load();
    sharedQueueB.load();
    sharedQueueC.load();
    printAllQueues();
pinMode(IR_SENSOR_PIN, INPUT);
myServo.attach(SERVO_PIN, 500, 2500);  // Correct for ESP32 + SG90
myServo.write(0);// digitalWrite(BLUE_LED_PIN , HIGH);
}



//bool waitingForCard = false;

void loop() {
  if (!waitingForCard) {
    // Check for object using IR sensor
    int sensorValue = digitalRead(IR_SENSOR_PIN); delay(50);
    if (sensorValue == LOW) {
      Serial.println("🚫 Object detected by IR sensor.");
      Serial.println("🔄 Dispensing card...");

      myServo.write(95);  // Open or dispense
      delay(1000);        // Let the card fall
      myServo.write(0);   // Close again
      delay(500);         // Optional debounce delay

      waitingForCard = true;  // Now expect card scan
    }
  } 
  else {
    // Wait for RFID card scan
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = getUIDString(mfrc522.uid.uidByte, mfrc522.uid.size);
      processCard1(uid);
      mfrc522.PICC_HaltA(); 
      mfrc522.PCD_StopCrypto1(); 
      delay(1200);
      //createMixedQueue(); 
      printAllQueues();
      waitingForCard = false;  // Return to sensor check
    }
  }
}

String getUIDString(byte *buf, byte size) {
    String uid=""; for (byte i=0;i<size;i++)
    {if(buf[i]<0x10)uid+="0"; uid+=String(buf[i],HEX);} uid.toUpperCase(); return uid;
}

void blinkLED(int pin) {
    //digitalWrite(BLUE_LED_PIN , LOW);
    digitalWrite(pin, HIGH); digitalWrite(BUZZER,HIGH ); 
    delay(500); 
    //digitalWrite(BLUE_LED_PIN , HIGH);
    digitalWrite(pin, LOW); digitalWrite(BUZZER, LOW );
}
