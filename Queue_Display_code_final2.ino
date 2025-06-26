#include <Ticker.h>
#include <PxMatrix.h> // https://github.com/2dom/PxMatrix
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
extern "C" {
  #include <espnow.h>
}
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <map>
#include <queue>
//#include <Preferences.h>
int number;
int Number1=0;
Ticker display_ticker;
//Preferences preferences;
// Pin Definition for Nodemcu to HUB75 LED MODULE
#define P_LAT 16  // D0
#define P_A 5     // D1
#define P_B 4     // D2
#define P_C 15    // D8
#define P_OE 2    // D4
#define P_D 12    // D6
#define P_E 0     // GND (no connection)

PxMATRIX display(128, 32, P_LAT, P_OE, P_A, P_B, P_C, P_D);



// Define colors
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

// ISR for refreshing LED matrix display
void display_updater() {
  display.display(100);
}

// Callback function when data is received
void onReceive(uint8_t *mac, uint8_t *incomingData, uint8_t len) {
  Serial.print("📩 Received: ");
   if(len == sizeof(int)){
    int nextPatient;
    memcpy(&nextPatient, incomingData, sizeof(int));
    //Serial.print(" Now Serving");
    Serial.println(nextPatient);
    display.clearDisplay();
    number = nextPatient;
    //drawPatientNumber(nextPatient);
    }
  Serial.println();
}


// Function to draw patient's number on LED
void drawPatientNumber(int number) {
  //display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(12, 8);
  display.setTextColor(myRED);
  display.print("NEXT ");

  Serial.printf("Patient No: %d\n", number);
if(number == Number1){
  if(number<10){
  display.setCursor(88, 6 );
  }else{
    display.setCursor(77, 6 );
    }
  display.setTextSize(3);
  display.print(number);
  display.showBuffer();}
  else{ 
    display.clearDisplay();
    Number1=number;
     if(number<10){
  display.setCursor(88, 6 );
  }else{
    display.setCursor(77, 6 );
    }
  display.setTextSize(3);
  display.print(number);
  display.showBuffer();}
    
    
}

void setup() {
  Serial.begin(115200);

  // Display Setup
  display.begin(16);
  display.setTextColor(myBLUE);
  display.clearDisplay();
  display_ticker.attach(0.002, display_updater);  // Attach ticker to refresh display

  // WiFi and MAC Address
  WiFi.mode(WIFI_STA);
  Serial.print("ESP8266 MAC: "); Serial.println(WiFi.macAddress());
  // Show intro message
  display.setCursor(5, 1);
  display.setTextSize(2);
  display.print("MEDIBOARDS");
  display.setCursor(5, 17);
  display.print("SONVISAGE");
  display.showBuffer();
  delay(6000);

  display.clearDisplay();

  // Initialize ESP-NOW
  if (esp_now_init() != 0) {
    Serial.println("❌ ESP-NOW init failed");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);  // Both send/recv (optional)
  esp_now_register_recv_cb(onReceive);
  // Show initial waiting message

}

void loop() {
  
  drawPatientNumber(number);
  //Nothing in loop — waiting for ESP-NOW messages
}
