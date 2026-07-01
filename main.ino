#include <WiFi.h>
#include <ESP32Ping.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "secrets.h"

const char* ssid = WIFI_SSID;
const char* pw = WIFI_PASSWORD;

const String tg_token = TELEGRAM_BOT_TOKEN;
const String chatID = TELEGRAM_CHAT_ID;

// Static IP Configuration
IPAddress local_IP(192, 168, 1, 220);
IPAddress gateway(192, 168, 1, 1); 
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(1, 1, 1, 1);

// Ping Targets
IPAddress server_IP(192, 168, 1, 200);
IPAddress internet_IP1(1, 1, 1, 1);
IPAddress internet_IP2(8, 8, 8, 8);

// Timers and Counters
unsigned long lastPingTime = 0;
const unsigned long PING_INTERVAL = 60000; // 1 minute
int failedServerPings = 0;

// LED Flashing Variables
unsigned long flashTimer = 0;
bool flashState = false;

// S3 Built-in LED is usually on 48
#ifndef RGB_BUILTIN
#define RGB_BUILTIN 48
#endif

// System States
enum DeviceState {
  WIFI_DOWN,
  NO_INTERNET_WIFI_OK,
  SERVER_UP,
  SERVER_MISSED_LATEST,
  SERVER_DEAD_INTERNET_UP,
  WIFI_CONNECTED_IDLE
};

DeviceState currentState = WIFI_DOWN;
DeviceState lastState = WIFI_DOWN;

void setLED(uint8_t r, uint8_t g, uint8_t b) {
  // Scale down heavily to absolute single digits because the hardware curve is clapped
  uint8_t dim_r = r > 0 ? 8 : 0; 
  uint8_t dim_g = g > 0 ? 8 : 0;
  uint8_t dim_b = b > 0 ? 8 : 0;

  // Custom multi-color mappings for states
  if (r == 128 && b == 128) { // Purple state
    dim_r = 4; 
    dim_b = 4;
  }
  if (r == 255 && g == 255) { // Yellow state
    dim_r = 4;
    dim_g = 4;
  }

  if (currentState == SERVER_DEAD_INTERNET_UP) {
    neopixelWrite(RGB_BUILTIN, r, g, b);
  } else {
    neopixelWrite(RGB_BUILTIN, dim_r, dim_g, dim_b);
  }
}

void handleLEDs() {
  switch (currentState) {
    case WIFI_DOWN:
      setLED(255, 0, 0); // Red
      break;
    case WIFI_CONNECTED_IDLE:
      setLED(128, 0, 128); // Purple
      break;
    case NO_INTERNET_WIFI_OK: // Flash purple
      if (millis() - flashTimer > 500) {
        flashTimer = millis();
        flashState = !flashState;
        if (flashState) setLED(128, 0, 128);
        else setLED(0, 0, 0);
      }
      break;
    case SERVER_UP:
      setLED(0, 255, 0); // Green
      break;
    case SERVER_MISSED_LATEST:
      setLED(255, 255, 0); // Yellow
      break;
    case SERVER_DEAD_INTERNET_UP: // Flash red
      if (millis() - flashTimer > 500) {
        flashTimer = millis();
        flashState = !flashState;
        if (flashState) setLED(255, 0, 0);
        else setLED(0, 0, 0);
      }
      break;
  }
}

bool checkInternet() {
  return Ping.ping(internet_IP1, 1) || Ping.ping(internet_IP2, 1);
}

// Generic function so we don't repeat this massive block of code
void send_telegram_alert(String message) {
  WiFiClientSecure client;
  client.setInsecure(); // Ignore SSL certs

  HTTPClient http;
  message.replace(" ", "%20"); // URL encode spaces
  String url = "https://api.telegram.org/bot" + tg_token + "/sendMessage?chat_id=" + chatID + "&text=" + message;

  Serial.println("[*] Sending HTTP request to Telegram...");
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    
    if (httpCode > 0) {
      Serial.printf("[+] Webhook fired. Response Code: %d\n", httpCode);
    } else {
      Serial.printf("[-] Network request choked. Error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println("[-] Unable to initialize secure client.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }

  Serial.println("Initialising...");
  WiFi.begin(ssid, pw);
  Serial.println("Connecting...");

  while (WiFi.status() != WL_CONNECTED) {
    currentState = WIFI_DOWN;
    handleLEDs();
    Serial.print(".");
    delay(500);
  }

  Serial.println("\nConnected!");
  
  if (checkInternet()) {
    Serial.println("[+] Internet ping successful.");
    currentState = WIFI_CONNECTED_IDLE; 
  } else {
    Serial.println("[-] WiFi connected but internet is clapped.");
    currentState = NO_INTERNET_WIFI_OK; 
  }
  handleLEDs();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    currentState = WIFI_DOWN;
    handleLEDs();
    return; 
  }

  

  if (millis() - lastPingTime >= PING_INTERVAL || lastPingTime == 0) {
    lastPingTime = millis();
    Serial.println("[*] Pinging Internet...");
    if (!checkInternet()) {
      currentState = NO_INTERNET_WIFI_OK;
      handleLEDs();
      return;
    }

    Serial.println("[*] Pinging Server...");

    if (Ping.ping(server_IP, 1)) {
      // If server was previously dead (missed 3+ pings), it's now back online
      if (failedServerPings >= 3) {
        Serial.println("[!] Server has recovered. Sending alert...");
        send_telegram_alert("mainhomeserver is back online.");
      }
      
      failedServerPings = 0; // Reset counter
      currentState = SERVER_UP;
      Serial.println("[+] Server is alive.");
      
    } else {
      failedServerPings++;
      Serial.printf("[-] Server missed ping. Strike %d\n", failedServerPings);

      if (failedServerPings < 3) {
        currentState = SERVER_MISSED_LATEST;
      } else {
        if (checkInternet()) {
          currentState = SERVER_DEAD_INTERNET_UP;
          if (failedServerPings == 3) {
            Serial.println("[!] Server missed 3 pings, launching down alert...");
            send_telegram_alert("mainhomeserver is down: missed 3 consecutive pings");
          }
        } else {
          currentState = NO_INTERNET_WIFI_OK;
        }
      }
    }
  }

  if (currentState != lastState || currentState == SERVER_DEAD_INTERNET_UP || currentState == NO_INTERNET_WIFI_OK) {
    handleLEDs();
    lastState = currentState; // Update the historical record
  }
}