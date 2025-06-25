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
int k;
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
void sendQueueToAllArrivalNodes(SharedQueue& queue, const char* queueName) {
    std::vector<QueueEntry> entries = queue.getAll();
    for (const auto& entry : entries) {
        QueueItem item;
        strncpy(item.uid, entry.uid.c_str(), sizeof(item.uid));
        strncpy(item.timestamp, entry.timestamp.c_str(), sizeof(item.timestamp));
        item.number = entry.number;
        item.node = 0; // Not tied to a specific doctor node
        item.addToQueue = true;
        item.removeFromQueue = false;
        strncpy(item.queueID, queueName, sizeof(item.queueID));

        broadcastToArrivalNodes(item);
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

        if (isArrivalNode) { Serial.println("🔄 Handling Arrival Node message...");
                  
    Serial.println("🔄 Handling Arrival Node message...");
    String uidStr = String(item.uid);
    cleanupRecentUIDs();

    if (item.addToQueue && !item.removeFromQueue) {
        if (recentUIDs.count("add:" + uidStr)) {
            Serial.println("⏳ UID recently added, skipping to prevent rebroadcast loop.");
        } else {
            recentUIDs["add:" + uidStr] = millis();
          // handleQueuePlacement(uidStr, String(item.timestamp), item.number, String(item.queueID));
          processCard(String(item.uid));
          broadcastAllQueues();
          printAllQueues();
        }
              // Broadcast updated sharedQueueC entries to all Arrival Nodes
  
    } else if (!item.addToQueue && item.removeFromQueue) {
        if (recentUIDs.count("rem:" + uidStr)) {
            Serial.println("⏳ UID recently removed, skipping to prevent rebroadcast loop.");
        } else {
            recentUIDs["rem:" + uidStr] = millis();

            if (sharedQueue.exists(uidStr)) {
                sharedQueue.removeByUID(uidStr);
                sharedQueueC.removeByUID(uidStr);
                sharedQueueA.add(uidStr, String(item.timestamp), item.number);
                     strncpy(item.queueID, "queueA", sizeof(item.queueID));
                Serial.printf("🗑️ UID %s removed from SharedQueue and added to SharedQueueA.\n", item.uid);
            } else {
                sharedQueueC.removeByUID(uidStr);
                sharedQueueB.removeByUID(uidStr);
                Serial.printf("🗑️ UID %s removed from SharedQueueC and SharedQueueB.\n", item.uid);
            }
        }
       
        sharedQueue.print(); sharedQueueA.print(); sharedQueueB.print(); sharedQueueC.print();
    }
createMixedQueue();broadcastAllQueues();
sendQueueToAllArrivalNodes(sharedQueue, "main");
sendQueueToAllArrivalNodes(sharedQueueA, "queueA");
sendQueueToAllArrivalNodes(sharedQueueB, "queueB");
sendQueueToAllArrivalNodes(sharedQueueC, "mix");




    } else if (isDoctorNode) {
         Serial.println("👨‍⚕️ Doctor Node message received...");
        if (strcmp(item.uid, "REQ_NEXT") == 0) {
             Serial.println("📬 Handling 'REQ_NEXT' from Doctor...");
            int doctorNodeID = -1;
            for (int i = 0; i < 4; i++){
                if (memcmp(mac, doctorMACs[i], 6) == 0) {
                doctorNodeID = i + 1;
                break;
                }
            }
               sharedQueue.print();sharedQueueA.print();sharedQueueB.print();sharedQueueC.print();
            if (doctorNodeID == -1){
                 Serial.println("❌ Unknown Doctor Node MAC!");
                  return;}
            if (!sharedQueueC.empty()) {
    QueueEntry entry = sharedQueueC.front();
    QueueItem sendItem;
    strncpy(sendItem.uid, entry.uid.c_str(), sizeof(sendItem.uid));
    strncpy(sendItem.timestamp, entry.timestamp.c_str(), sizeof(sendItem.timestamp));
    strncpy(item.queueID, "queueC", sizeof(item.queueID));
    sendItem.number = entry.number;
    sendItem.node = doctorNodeID;
    sendItem.addToQueue = false;
    sendItem.removeFromQueue = false;

    // Send to doctor
    esp_now_send(mac, (uint8_t*)&sendItem, sizeof(sendItem));

    // Remove from MixedQueue
    // Move to bottom of the correct original queue
    if (sharedQueue.exists(entry.uid)) {
       // sharedQueue.removeByUID(entry.uid); 
        sharedQueue.pop();      // Remove original position
        sharedQueue.push(entry); 
         sharedQueueC.pop(); sharedQueueC.push(entry); 
        strncpy(item.queueID, "main", sizeof(item.queueID));
     // Push to back
    } else if (sharedQueueB.exists(entry.uid)) {
        sharedQueueB.pop(); sharedQueueC.pop();
        sharedQueueB.push(entry);sharedQueueC.push(entry);
    strncpy(item.queueID, "queueC", sizeof(item.queueID));
  // Tag the origin
    } 
                QueueItem updateItem = sendItem;
                updateItem.addToQueue = true;
                updateItem.removeFromQueue = false;
                broadcastToArrivalNodes(updateItem);
                Serial.printf("🛱 Broadcasted update to Arrival Nodes: UID %s\n", updateItem.uid);
           
}
 else {
                QueueItem zeroItem = {};
                strncpy(zeroItem.uid, "NO_PATIENT", sizeof(zeroItem.uid));
                 //strncpy(item.queueID, queueName, sizeof(item.queueID));  // Tag the origin
                zeroItem.number = 0;
                zeroItem.node = doctorNodeID;
                zeroItem.addToQueue = false;
                zeroItem.removeFromQueue = false;
                esp_now_send(mac, (uint8_t*)&zeroItem, sizeof(zeroItem));
                Serial.printf("⚠️ Queue empty. Sent 'NO_PATIENT' to Doctor Node %d.\n", doctorNodeID);
          
            }
        } else if (item.removeFromQueue) {
            item.addToQueue = false;
            item.removeFromQueue = true;
            // strncpy(item.queueID, queueName, sizeof(item.queueID));  // Tag the origin
            for (int i = 0; i < numArrivalNodes; i++) {
                if (memcmp(arrivalMACs[i], WiFi.macAddress().c_str(), 6) != 0)
                    esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
            }
            Serial.println("📤 Broadcasted removal to Arrival Nodes.");

            if (sharedQueue.exists(String(item.uid))) {
                sharedQueue.removeByUID(String(item.uid));
                sharedQueueC.removeByUID(String(item.uid));
                sharedQueueA.add(String(item.uid), String(item.timestamp), item.number);
                strncpy(item.queueID, "queueA", sizeof(item.queueID));
                Serial.printf("🗑️ UID %s removed from SharedQueue and added to SharedQueueA.\n", item.uid);
            } else {
                sharedQueueC.removeByUID(String(item.uid));
                sharedQueueB.removeByUID(String(item.uid));
                Serial.printf("🗑️ UID %s removed from SharedQueueC and SharedQueueB.\n", item.uid);
            }
           broadcastToArrivalNodes(item);
           broadcastAllQueues();
           
            if (sharedQueueC.empty()) {
                Serial.println("⚠️ Queue is now empty. Sending NO_PATIENT to all Doctor Nodes...");
                for (int i = 0; i < 4; i++) {
                    QueueItem zeroItem = {};
                    strncpy(zeroItem.uid, "NO_PATIENT", sizeof(zeroItem.uid));

                    zeroItem.number = 0;
                    zeroItem.node = i + 1;
                    zeroItem.addToQueue = false;
                    zeroItem.removeFromQueue = false;
                    esp_now_send(doctorMACs[i], (uint8_t*)&zeroItem, sizeof(zeroItem));
                }
                Serial.println("📤 Sent NO_PATIENT to all doctors.");
            }
        }
    }
    // createMixedQueue(); 

}






int getPermanentNumber(String uid) {
    prefs.begin("rfidMap", true); int pid=-1;
    if (prefs.isKey(uid.c_str())) pid = prefs.getUInt(uid.c_str(),-1);
    prefs.end(); return pid;
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Sent 🟢" : "Failed 🔴");
}

void processCard(String uid) {
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
        sharedQueueA.removeByUID(uid);
        sharedQueueB.add(uid, timeStr, pid);
        strncpy(item.queueID, "queueB", sizeof(item.queueID));
// Tag the origin
        for (int i = 0; i < numArrivalNodes; i++) esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
        Serial.println("🔄 UID moved from SharedQueueA to SharedQueueB.");
        if(k==1){ blinkLED(GREEN_LED_PIN);}
         // Success indicator digitalWrite(GREEN_LED_PIN, LOW );
    } else if (sharedQueue.exists(uid) || sharedQueueB.exists(uid)) {
       //strncpy(item.queueID, queueName, sizeof(item.queueID));  // Tag the origin
        Serial.println("⚠️ UID already in queue.");
         if(k==1){ blinkLED(RED_LED_PIN); }
         // Warning indicator
    } else {
        sharedQueue.add(uid, timeStr, pid);
         strncpy(item.queueID, "main", sizeof(item.queueID));
        for (int i = 0; i < numArrivalNodes; i++) esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
        Serial.println("✅ UID added to SharedQueue.");
         if(k==1){blinkLED(GREEN_LED_PIN);}
          // Success indicator
    }
    sharedQueue.print();sharedQueueA.print();sharedQueueB.print();sharedQueueC.print();
}

void broadcastAllQueues() {
    auto broadcastQueue = [](SharedQueue& queue, const char* queueName) {
        std::vector<QueueEntry> entries = queue.getAll();
        for (const auto& entry : entries) {
            QueueItem item;
            strncpy(item.uid, entry.uid.c_str(), sizeof(item.uid));
            strncpy(item.timestamp, entry.timestamp.c_str(), sizeof(item.timestamp));
            item.number = entry.number;
            item.node = 0;
            item.addToQueue = true;
            item.removeFromQueue = false;
            strncpy(item.queueID, queueName, sizeof(item.queueID));  // Tag the origin
            broadcastToArrivalNodes(item);
        }
    };

    broadcastQueue(sharedQueue, "main");
    broadcastQueue(sharedQueueA, "queueA");
    broadcastQueue(sharedQueueB, "queueB");
    broadcastQueue(sharedQueueC, "mix");

    Serial.println("📡 Broadcasted all queues to arrival nodes with queue IDs.");
}

//Only arbiter should contain the following function createMixedQueue() 

void createMixedQueue() {
    sharedQueueC.clear();

    std::vector<QueueEntry> entriesA = sharedQueue.getAll();
    std::vector<QueueEntry> entriesB = sharedQueueB.getAll();
    size_t indexA = 0, indexB = 0;

    while (indexA < entriesA.size() || indexB < entriesB.size()) {
        if (indexB < entriesB.size()) {
            for (int i = 0; i < 10 && indexB < entriesB.size(); i++, indexB++) {
                sharedQueueC.add1(entriesB[indexB].uid, entriesB[indexB].timestamp, entriesB[indexB].number);
            }
        }

        if (indexA < entriesA.size()) {
            for (int i = 0; i < 5 && indexA < entriesA.size(); i++, indexA++) {
                sharedQueueC.add1(entriesA[indexA].uid, entriesA[indexA].timestamp, entriesA[indexA].number);
            }
        }
    }

    // Broadcast updated sharedQueueC entries to all Arrival Nodes
    std::vector<QueueEntry> entriesC = sharedQueueC.getAll();
    for (const auto& entry : entriesC) {
        QueueItem item;
        strncpy(item.uid, entry.uid.c_str(), sizeof(item.uid));
        strncpy(item.timestamp, entry.timestamp.c_str(), sizeof(item.timestamp));
        strncpy(item.queueID, "mix", sizeof(item.queueID));
        item.number = entry.number;
        item.addToQueue = true;
        item.removeFromQueue = false;
        broadcastToArrivalNodes(item);

    }
    Serial.println("📡 Broadcasted sharedQueueC to all Arrival Nodes.");
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
    //clearAllQueues();
   
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

    createMixedQueue();broadcastAllQueues(); //Only arbiter should contain the following
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
      k=1;                    //Only arbiter should contain the following
      processCard(uid);
      k=0;                    //Only arbiter should contain the following
      mfrc522.PICC_HaltA(); 
      mfrc522.PCD_StopCrypto1(); 
      delay(1200);

      createMixedQueue(); broadcastAllQueues();//Only arbiter should contain the following

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


