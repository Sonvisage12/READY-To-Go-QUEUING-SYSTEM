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
bool arbiter = true;
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
bool isMaster = false;          // True if this node is the current master
int myIndex = -1;               // This node's index in arrivalMACs
int currentMasterIndex = 0;     // Index of the currently known master
//const int numDoctorNodes = sizeof(doctorMACs) / sizeof(doctorMACs[0]);

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

const int numDoctorNodes = sizeof(doctorMACs) / sizeof(doctorMACs[0]);  // 👈 Must be AFTER doctorMACs is declared

void broadcastToArrivalNodes(const QueueItem &item) {
String myMAC = WiFi.macAddress();
if (myMAC == String(item.sourceMAC)) {
    Serial.println("🔁 Received own broadcast. Skipping rebroadcast.");
    return;
    for (int i = 0; i < numArrivalNodes; i++) {
        esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
    }
    }
}
void sendQueueToAllArrivalNodes(SharedQueue& queue, const char* queueName) {
   QueueItem item;
    std::vector<QueueEntry> entries = queue.getAll();
  String myMAC = WiFi.macAddress();
if (myMAC == String(item.sourceMAC)) {
    Serial.println("🔁 Received own broadcast. Skipping rebroadcast.");
    return;
    //std::vector<QueueEntry> entries = queue.getAll();
    for (const auto& entry : entries) {
       // QueueItem item;
        strncpy(item.uid, entry.uid.c_str(), sizeof(item.uid));
        strncpy(item.timestamp, entry.timestamp.c_str(), sizeof(item.timestamp));
        item.number = entry.number;
        item.node = 0; // Not tied to a specific doctor node
        item.addToQueue = true;
        item.removeFromQueue = false;
        strncpy(item.queueID, queueName, sizeof(item.queueID));

        broadcastToArrivalNodes(item);}
    }
}
void handleAddToQueueAsMaster(String uid, String timestamp, int number, String queueID) {
    if (queueID == "queueA") {
        if (sharedQueue.exists(uid)) {
            sharedQueue.removeByUID(uid);
            sharedQueueA.add(uid, timestamp, number);
        }
    } else if (queueID == "queueB") {
        if (sharedQueueA.exists(uid)) {
            sharedQueueA.removeByUID(uid);
            sharedQueueB.add(uid, timestamp, number);
        }
    } else if (queueID == "mix") {
        if (!sharedQueueC.exists(uid)) {
            sharedQueueC.add(uid, timestamp, number);
        }
    } else {
        if (!sharedQueue.exists(uid) && !sharedQueueA.exists(uid) && !sharedQueueB.exists(uid)) {
            sharedQueue.add(uid, timestamp, number);
        }
    }

    createMixedQueue();         // only master runs this
    broadcastAllQueues();       // master updates others
    printAllQueues();
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
    String uidStr(item.uid);
    // Identify source role
    bool fromArrival = false, fromDoctor = false;
    int srcIndex = -1, doctorNodeID = -1;
    const uint8_t* mac = recvInfo->src_addr;
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X", 
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    // Check if source is a known arrival node
    for (int i = 0; i < numArrivalNodes; ++i) {
        if (memcmp(mac, arrivalMACs[i], 6) == 0) { fromArrival = true; srcIndex = i; break; }
    }
    // Check if source is a known doctor node
    for (int i = 0; i < numDoctorNodes; ++i) {
        if (memcmp(mac, doctorMACs[i], 6) == 0) { fromDoctor = true; doctorNodeID = i+1; break; }
    }

    if (fromArrival) {
        Serial.printf("📩 Arrival message from node %d (%s)\n", srcIndex, macStr);
        cleanupRecentUIDs();
        // Handle add-to-queue events
        if (item.addToQueue && !item.removeFromQueue) {
            if (recentUIDs.count("add:" + uidStr)) {
                Serial.println("⏳ Duplicate add event, ignoring.");
                return;
            }
            recentUIDs["add:" + uidStr] = millis();
            // Check for master change
            if (!isMaster) {
                if (srcIndex != currentMasterIndex) {
                    // Not from current master -> potential master change or failover
                    if (srcIndex > myIndex) {
                        // A node with higher index sent this -> promote self to master
                        Serial.println("⚠️ Master down. Promoting self to master.");
                        isMaster = true;
                        currentMasterIndex = myIndex;
                    } else if (srcIndex != myIndex) {
                        // A lower-index node (higher priority) sent this -> it is new master
                        Serial.printf("🔁 Updating current master to node %d.\n", srcIndex);
                        currentMasterIndex = srcIndex;
                    }
                }
            }
            // Now process the add event
            if (isMaster) {
                // Master mode: handle queue placement fully and broadcast to others
                handleAddToQueueAsMaster(String(item.uid), String(item.timestamp), item.number, String(item.queueID));
            } else {
                // Secondary mode: update local queue state (no rebroadcast)
                handleQueuePlacement1(uidStr, String(item.timestamp), item.number, String(item.queueID));
            }
        }
        // Handle remove-from-queue events
        else if (!item.addToQueue && item.removeFromQueue) {
            if (recentUIDs.count("rem:" + uidStr)) {
                Serial.println("⏳ Duplicate remove event, ignoring.");
                return;
            }
            recentUIDs["rem:" + uidStr] = millis();
            if (!isMaster) {
                if (srcIndex != currentMasterIndex) {
                    // Source is not the known master -> update master (likely a new master broadcasting removal)
                    if (srcIndex != myIndex) {
                        Serial.printf("🔁 Master updated to node %d (for removal event).\n", srcIndex);
                        currentMasterIndex = srcIndex;
                    }
                    // (If srcIndex > myIndex, theoretically could mean a higher-index tried removal, 
                    // but normal nodes don't send removal events, so we ignore that case.)
                }
            }
            // Update local queues for removal
            if (sharedQueue.exists(uidStr)) {
                sharedQueue.removeByUID(uidStr);
                sharedQueueC.removeByUID(uidStr);
                sharedQueueA.add(uidStr, String(item.timestamp), item.number);
                Serial.printf("🗑️ %s removed from main queue, added to QueueA.\n", item.uid);
            } else {
                // If not in main, remove from secondary queues B/C
                sharedQueueC.removeByUID(uidStr);
                sharedQueueB.removeByUID(uidStr);
                Serial.printf("🗑️ %s removed from QueueC/QueueB (if present).\n", item.uid);
            }
        }
        printAllQueues();
        return;  // done handling arrival message
    }

    if (fromDoctor && doctorNodeID != -1) {
        Serial.printf("📩 Doctor message from Node %d (%s)\n", doctorNodeID, macStr);
        if (strcmp(item.uid, "REQ_NEXT") == 0) {
            Serial.println("👨‍⚕️ Doctor requests next patient.");
            if (!isMaster) {
                // No response from true master, take over
                Serial.println("⚠️ Current master unresponsive. Becoming master to serve doctor.");
                isMaster = true;
                currentMasterIndex = myIndex;
            }
            if (!sharedQueueC.empty()) {
                // Prepare next patient info
                QueueEntry entry = sharedQueueC.front();
                // Determine origin queue and rotate it
                if (sharedQueue.exists(entry.uid)) {
                    sharedQueue.pop();
                    sharedQueue.push(entry); // move to back of main queue
                } else if (sharedQueueB.exists(entry.uid)) {
                    sharedQueueB.pop();
                    sharedQueueB.push(entry); // move to back of re-registration queue B
                }
                // Remove from front of mixed queue and re-add to back (to maintain ordering for round-robin)
                sharedQueueC.pop();
                sharedQueueC.push(entry);
                // Send patient info to requesting doctor
                QueueItem response{};
                strncpy(response.uid, entry.uid.c_str(), sizeof(response.uid));
                strncpy(response.timestamp, entry.timestamp.c_str(), sizeof(response.timestamp));
                response.number = entry.number;
                response.node   = doctorNodeID;
                response.addToQueue = false;
                response.removeFromQueue = false;
                esp_now_send(mac, (uint8_t*)&response, sizeof(response));  // send to this doctor
                Serial.printf("✅ Sent next patient %s to Doctor %d.\n", response.uid, doctorNodeID);
                // Broadcast an update to arrival nodes about this patient's new position (rotated to queueA or back of queue)
                QueueItem update = response;
                update.addToQueue = true;
                update.removeFromQueue = false;
                if (sharedQueue.exists(entry.uid)) {
                    strncpy(update.queueID, "main", sizeof(update.queueID));
                } else {
                    strncpy(update.queueID, "queueB", sizeof(update.queueID));
                }
                broadcastToArrivalNodes(update);
                Serial.println("🔄 Broadcasted queue update to all arrival nodes (post-doctor call).");
            } else {
                // No patient waiting
                QueueItem noPat{};
                strncpy(noPat.uid, "NO_PATIENT", sizeof(noPat.uid));
                noPat.number = 0;
                noPat.node   = doctorNodeID;
                noPat.addToQueue = false;
                noPat.removeFromQueue = false;
                esp_now_send(mac, (uint8_t*)&noPat, sizeof(noPat));
                Serial.println("⚠️ No patients in queue. Informed doctor.");
            }
        } else if (item.removeFromQueue) {
            // Doctor signals patient has been handled/removed
            Serial.println("👨‍⚕️ Doctor signals removal of patient from queue.");
            if (!isMaster) {
                // If this node wasn't master, assume master failed to handle this and take over
                Serial.println("⚠️ Master not responding to removal. Becoming master to handle removal.");
                isMaster = true;
                currentMasterIndex = myIndex;
            }
            // Broadcast removal command to all other arrival nodes
            item.addToQueue = false;
            item.removeFromQueue = true;
            strncpy(item.queueID, "", sizeof(item.queueID));  // not strictly needed to set
            for (int i = 0; i < numArrivalNodes; ++i) {
                if (i == myIndex) continue;
                esp_now_send(arrivalMACs[i], (uint8_t*)&item, sizeof(item));
            }
            Serial.println("📢 Broadcasted removal to all nodes.");
            // Update local queues for removal (same as above)
            if (sharedQueue.exists(uidStr)) {
                sharedQueue.removeByUID(uidStr);
                sharedQueueC.removeByUID(uidStr);
                sharedQueueA.add(uidStr, String(item.timestamp), item.number);
                Serial.printf("🗑️ %s removed from main queue, added to QueueA.\n", item.uid);
            } else {
                sharedQueueC.removeByUID(uidStr);
                sharedQueueB.removeByUID(uidStr);
                Serial.printf("🗑️ %s removed from QueueC/QueueB.\n", item.uid);
            }
            // If no one left waiting, inform all doctors to stop calling
            if (sharedQueueC.empty()) {
                QueueItem noPat{};
                strncpy(noPat.uid, "NO_PATIENT", sizeof(noPat.uid));
                noPat.number = 0;
                noPat.addToQueue = false;
                noPat.removeFromQueue = false;
                for (int i = 0; i < numDoctorNodes; ++i) {
                    noPat.node = i+1;
                    esp_now_send(doctorMACs[i], (uint8_t*)&noPat, sizeof(noPat));
                }
                Serial.println("⚠️ Queue empty. Notified all doctors with NO_PATIENT.");
            }
        }
        printAllQueues();
        return;
    }
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
    printAllQueues();
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
    printAllQueues();
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
   
   size_t myIndex = 0;
for (; myIndex < numArrivalNodes; ++myIndex) {
    if (memcmp(WiFi.macAddress().c_str(), arrivalMACs[myIndex], 6) == 0) break;
}
bool isMaster = (myIndex == 0);
size_t currentMasterIndex = 0;

String myMAC = WiFi.macAddress();
Serial.print("This node MAC: "); Serial.println(myMAC);

for (int i = 0; i < numArrivalNodes; i++) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             arrivalMACs[i][0], arrivalMACs[i][1], arrivalMACs[i][2],
             arrivalMACs[i][3], arrivalMACs[i][4], arrivalMACs[i][5]);
    if (myMAC.equalsIgnoreCase(String(macStr))) {
        myIndex = i;
        break;
    }
}

isMaster = (myIndex == 0);  // Node 0 is default master
currentMasterIndex = 0;




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
if(arbiter){
createMixedQueue();broadcastAllQueues(); //Only arbiter should contain the following  
}
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
      k=1; 
      if(arbiter){ processCard(uid);
      mfrc522.PICC_HaltA(); 
      mfrc522.PCD_StopCrypto1(); 
      delay(1200);

      createMixedQueue(); broadcastAllQueues();//Only arbiter should contain the following

   
      }
      else{ processCard1(uid);
      mfrc522.PICC_HaltA(); 
      mfrc522.PCD_StopCrypto1(); 
      delay(1200);
      // createMixedQueue();
      }                   //Only arbiter should contain the following
     
      k=0;                    //Only arbiter should contain the following
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


