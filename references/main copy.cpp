/***************************************************
  ESP32 R307 Fingerprint Sensor - Simplified Production Version
  Continuous fingerprint reading with Firebase logging (no menu)
  
  ESP32 R307 Fingerprint Sensor Wiring:
  - Red wire (VCC) -> ESP32 3.3V or 5V
  - Green wire (GND) -> ESP32 GND
  - Yellow wire (TX from sensor) -> ESP32 GPIO16 (RX2)
  - Black wire (RX to sensor) -> ESP32 GPIO17 (TX2)
 ****************************************************/
#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include "secrets.h"
#include "pitches.h"

#include <ESP8266WiFi.h>
#include <Wire.h> // For I2C communication with Mega

// Use main Serial at 57600 for fingerprint sensor and monitor
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial);

// Buzzer pin definition (NodeMCU D7 -> GPIO13)
#define BUZZER_PIN 13

// Float water level switch (2-wire) - NodeMCU D6 -> GPIO12
#define FLOAT_PIN 12
unsigned long lastFloatRead = 0;
const unsigned long FLOAT_READ_INTERVAL = 2000; // 2 seconds between reports
bool lastFloatState = false;

// Use installed Preferences library (vshymanskyy/Preferences) for ESP8266
#include <Preferences.h>

// Preferences instance (persistent where supported by the library)
Preferences preferences;

// Firebase objects
#define SSL_CLIENT WiFiClientSecure
SSL_CLIENT ssl_client;

using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000);
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult databaseResult;

// NTP settings
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 8 * 3600;  // Philippine Time (UTC+8)
const int daylightOffset_sec = 0;  // Philippines doesn't observe daylight saving time

// Global variables
bool taskComplete = false;  // Track Firebase initialization completion
bool systemLocked = false;  // Track if system is locked due to failed attempts
int currentFailedAttempts = 0;  // Cache for failed attempts count
unsigned long lastFailedAttemptsCheck = 0;  // Last time we checked Firebase for failed attempts
const unsigned long FAILED_ATTEMPTS_CHECK_INTERVAL = 5000;  // Check every 5 seconds
const int MAX_FAILED_ATTEMPTS = 3;  // Maximum allowed failed attempts
// Relay polling
unsigned long lastRelaysCheck = 0;
const unsigned long RELAYS_CHECK_INTERVAL = 10000; // Check relays every 10s

// Relay state tracking to send one-shot messages on change
const int MAX_RELAY_ID = 16;
bool relaySeen[MAX_RELAY_ID + 1] = {false};
bool relayStateLast[MAX_RELAY_ID + 1] = {false};
bool relaysInitialSent = false; // true after we've sent initial values once

// Door lock one-shot tracking: read /devices/fingerprint_door_001/doorLock
unsigned long lastDoorLockCheck = 0;
const unsigned long DOORLOCK_CHECK_INTERVAL = 5000; // check every 5s
bool doorLockSeen = false;
bool doorLockStateLast = false;
bool doorLockInitialSent = false;

// Connection monitoring system
bool wifiConnected = false;
bool firebaseConnected = false;
bool ntpSynced = false;
unsigned long lastWiFiCheck = 0;
unsigned long lastFirebaseCheck = 0;
unsigned long lastNTPCheck = 0;
const unsigned long WIFI_CHECK_INTERVAL = 5000;      // Check WiFi every 5 seconds
const unsigned long FIREBASE_CHECK_INTERVAL = 10000; // Check Firebase every 10 seconds
const unsigned long NTP_CHECK_INTERVAL = 3600000;    // Check NTP every hour
bool connectionCheckInProgress = false;

// Function declarations
uint8_t getFingerprintID();
String getFingerprintName(uint8_t id);
void saveFingerprintName(uint8_t id, String name);
void deleteFingerprintName(uint8_t id);
void buzzerAlarm();
void setupWiFi();
void setupFirebase();
void setupNTP();
String getCurrentDateTime();
void logFingerprintAccess(String status, String user, int attempts = 0);
void processFirebaseData(AsyncResult &aResult);
void set_ssl_client_insecure_and_buffer(SSL_CLIENT &client);
void checkFailedAttempts();
bool isSystemLocked();

// Connection monitoring functions
void monitorConnections();
bool checkWiFiConnection();
bool checkFirebaseConnection();
bool checkNTPSync();
void reconnectWiFi();
void reconnectFirebase();
void resyncNTP();
bool testInternetConnectivity();
void syncFailedAttemptsFromFirebase();
void fetchAndPrintRelays();
void fetchAndSendDoorLock();
// Ensure sendI2CMessage is visible to functions defined below
bool sendI2CMessage(const String &msg);
// (menu removed)

// Helper to send a message to Mega via I2C
// Simple CRC8 (xor) for sender - matches Mega's crc8
uint8_t crc8_node(const uint8_t *data, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; ++i) c ^= data[i];
  return c;
}

// Read the doorLock boolean at /devices/fingerprint_door_001/doorLock and send one-shot
// I2C messages: "lock" when true, "unlock" when false. Send on first initialization
// and on any state change. Uses blocking Database.get since values are small and infrequent.
void fetchAndSendDoorLock() {
  // Rate-limit checks
  if (millis() - lastDoorLockCheck < DOORLOCK_CHECK_INTERVAL) return;
  lastDoorLockCheck = millis();

  if (!app.ready() || !wifiConnected || !firebaseConnected) {
    // Skip when DB not reachable
    return;
  }

  String path = "/devices/fingerprint_door_001/doorLock";

  // Blocking get for simplicity
  bool value = Database.get<bool>(aClient, path);
  if (aClient.lastError().code() != 0) {
    Serial.printf("❌ Failed to read doorLock: %s (code: %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
    return;
  }

  // If first time seeing the value in this run, initialize
  if (!doorLockSeen) {
    doorLockSeen = true;
    doorLockStateLast = value;
    // On first initialization (or when flagged), send the current state once
    if (!doorLockInitialSent) {
      String msg = value ? "lock" : "unlock";
      Serial.printf("Initial doorLock state: %s -> sending '%s'\n", value ? "true" : "false", msg.c_str());
      sendI2CMessage(msg);
      doorLockInitialSent = true;
    }
    return;
  }

  // Already seen previously; if changed, send once and update
  if (doorLockStateLast != value) {
    String msg = value ? "lock" : "unlock";
    Serial.printf("doorLock changed: %s -> %s; sending '%s'\n", doorLockStateLast ? "true" : "false", value ? "true" : "false", msg.c_str());
    sendI2CMessage(msg);
    doorLockStateLast = value;
  }
}


// Reliable send: format <msgId>|<payload>|<crc>\n, send via I2C, then request 1-byte ACK from slave.
// Retries on non-OK ack or transmission error.
uint16_t nextMsgId = 1;
bool sendReliableI2CMessage(const String &payload, int maxRetries = 3, unsigned long retryDelayMs = 150) {
  if (payload.length() == 0) return false;

  uint16_t attemptId = nextMsgId++;
  if (nextMsgId == 0) nextMsgId = 1; // wrap (avoid 0)

  String idStr = String(attemptId);
  String dataForCrc = idStr + "|" + payload;
  uint8_t crc = crc8_node((const uint8_t *)dataForCrc.c_str(), dataForCrc.length());
  String packet = dataForCrc + "|" + String(crc) + "\n";

  for (int attempt = 0; attempt < maxRetries; ++attempt) {
    Wire.beginTransmission(0x08);
    Wire.write(packet.c_str(), packet.length());
    int res = Wire.endTransmission();
    Serial.printf("I2C endTransmission=%d; pkt=%s", res, packet.c_str());

    if (res != 0) {
      Serial.printf(" - transmission error (attempt %d/%d)\n", attempt+1, maxRetries);
      delay(retryDelayMs);
      continue;
    }

    // Ask slave for 1-byte ACK
    Wire.requestFrom((uint8_t)0x08, (uint8_t)1);
    unsigned long start = millis();
    while (Wire.available() == 0 && (millis() - start) < 200) {
      delay(1);
    }
    uint8_t ack = 0x00;
    if (Wire.available()) ack = Wire.read();

    Serial.printf(" -> received ack=0x%02X (attempt %d/%d)\n", ack, attempt+1, maxRetries);

    if (ack == 0x01) {
      // OK
      return true;
    }

    // if CRC fail or no ack, retry
    delay(retryDelayMs);
  }

  Serial.printf("❌ sendReliableI2CMessage failed after %d attempts: %s\n", maxRetries, packet.c_str());
  return false;
}

// Backwards-compatible thin wrapper (keeps existing call sites)
bool sendI2CMessage(const String &msg) {
  return sendReliableI2CMessage(msg);
}

void setup() {
  // Initialize Serial at 57600 for both monitor and fingerprint sensor
  Serial.begin(57600);
  delay(50);
  Serial.println("=== ESP32 Fingerprint Access Control ===");
  // No separate fingerprint serial - using main Serial at 57600

  // Initialize I2C as master: SDA=D2 (GPIO4), SCL=D1 (GPIO5)
  Wire.begin(D2, D1); // D2=GPIO4, D1=GPIO5

  // Ensure passive (active-low) buzzer is silent at startup: set pin as OUTPUT and drive HIGH
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);

  // Initialize float switch input (INPUT_PULLUP). LOW = CLOSED (water present)
  pinMode(FLOAT_PIN, INPUT_PULLUP);
  lastFloatState = (digitalRead(FLOAT_PIN) == LOW);
  Serial.print("Float switch initial: ");
  Serial.println(lastFloatState ? "CLOSED (water)" : "OPEN (no water)");

  // Serial is used by fingerprint sensor on NodeMCU
  if (finger.verifyPassword()) {
    Serial.println("✓ Fingerprint sensor ready");
  } else {
    Serial.println("❌ Fingerprint sensor error");
    while (1) { 
      delay(100); 
    }
  }

  // Initialize preferences for name storage
  preferences.begin("fingerprints", false);
  
  // Setup network and services
  setupWiFi();
  setupNTP();
  setupFirebase();
  
  Serial.println("✓ System ready");
}

// menu removed

void loop() {
  // Maintain Firebase authentication and async tasks
  app.loop();
  
  // Process Firebase async results
  processFirebaseData(databaseResult);
  
  // Monitor and maintain all connections (non-blocking)
  monitorConnections();
  
  // Sync failed attempts from Firebase once when app is ready
  if (app.ready() && !taskComplete) {
    taskComplete = true;
    syncFailedAttemptsFromFirebase();
  }
  
  // Check failed attempts status periodically
  checkFailedAttempts();

  // Fetch and print relay states from Firebase periodically
  fetchAndPrintRelays();
  // Fetch doorLock value and send one-shot lock/unlock messages on change
  fetchAndSendDoorLock();
  
  // Only scan for fingerprints if system is not locked
  if (!isSystemLocked()) {
    getFingerprintID();
  } else {
    // System is locked, show lockout message periodically
    static unsigned long lastLockoutMessage = 0;
    if (millis() - lastLockoutMessage > 10000) { // Show message every 10 seconds
      Serial.println("🔒 SYSTEM LOCKED - Too many failed attempts");
      Serial.println("Reset failed_attempts to 0 in Firebase to unlock");
      lastLockoutMessage = millis();
    }
  }
  
  // Read float sensor periodically (non-blocking)
  if (millis() - lastFloatRead >= FLOAT_READ_INTERVAL) {
    lastFloatRead = millis();
    bool state = (digitalRead(FLOAT_PIN) == LOW); // LOW = closed when using INPUT_PULLUP

    // Only act when the state changes to avoid flooding
    if (state != lastFloatState) {
      lastFloatState = state;

      // Prepare timestamp and DB paths
      String currentDateTime = getCurrentDateTime(); // "YYYY-MM-DD HH:MM:SS"
      String date = currentDateTime.substring(0, 10);
      String time = currentDateTime.substring(11, 19);
      String logPath = "/devices/water_level_001/logs/" + date + "/" + time;

      String status = state ? "water_present" : "water_empty";
      String tank_status = state ? "normal" : "alert";

      // Send single I2C alert when water transitions to empty (state == false)
      if (!state) { // state == false means water is empty
        sendI2CMessage("waterempty");
      }
        // Send one-shot alert when water transitions to present (state == true)
        else {
          sendI2CMessage("waterpresent");
        }

      // Update Firebase if available
      if (app.ready() && wifiConnected && firebaseConnected) {
        bool ok1 = Database.set<String>(aClient, logPath + "/status", status);
        bool ok2 = Database.set<String>(aClient, logPath + "/tank_status", tank_status);
        bool ok3 = Database.set<bool>(aClient, logPath + "/water_level", state);

        // Update root-level status and timestamp
        bool ok4 = Database.set<String>(aClient, "/devices/water_level_001/last_updated", currentDateTime);
        bool ok5 = Database.set<bool>(aClient, "/devices/water_level_001/water_level", state);

        if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5) {
          // Mark firebase connection as unreliable so monitorConnections() can try to recover
          firebaseConnected = false;
          Serial.printf("❌ Failed to update water level in Firebase (code: %d)\n", aClient.lastError().code());
        }
      } else {
        // Firebase not available; mark firebaseConnected false so reconnect logic runs
        firebaseConnected = false;
        Serial.println("⚠️ Water level changed but Firebase not ready - will sync when available");
      }
    }
  }

  // ...no periodic test messages in production build

  // Small delay to prevent overwhelming the sensor
  delay(25); // Reduced from 50ms to 25ms for faster response
}

// Fetch relays array from Realtime Database and print id:state pairs
void fetchAndPrintRelays() {
  // Rate-limit checks
  if (millis() - lastRelaysCheck < RELAYS_CHECK_INTERVAL) return;
  lastRelaysCheck = millis();

  if (!app.ready() || !wifiConnected || !firebaseConnected) {
    // Skip when DB not reachable
    return;
  }

  String relaysPath = "/smart_controls/relays";

  // Blocking get to fetch current relays array
  String payload = Database.get<String>(aClient, relaysPath);
  if (aClient.lastError().code() != 0) {
    Serial.printf("❌ Failed to read relays: %s (code: %d)\n", aClient.lastError().message().c_str(), aClient.lastError().code());
    return;
  }

  // payload should be a JSON array. We'll do a minimal parse to extract id and state
  // Example payload snippet: [null, {"id":1,"last_updated":"...","name":"Motor","state":false}, ...]
  // We'll search for occurrences of '"id":' and the following '"state":'.

  String out = "";
  int idx = 0;
  while (true) {
    int idPos = payload.indexOf("\"id\":", idx);
    if (idPos == -1) break;
    int idStart = idPos + 5;
    // read number
    int idEnd = idStart;
    while (idEnd < payload.length() && isDigit(payload[idEnd])) idEnd++;
    String idStr = payload.substring(idStart, idEnd);

    // find state after id
    int statePos = payload.indexOf("\"state\":", idEnd);
    if (statePos == -1) break;
    int stateStart = statePos + 8;
    // skip possible spaces
    while (stateStart < payload.length() && isWhitespace(payload[stateStart])) stateStart++;
    // read true/false
    String stateStr = "";
    if (payload.substring(stateStart, stateStart + 4) == "true") {
      stateStr = "true";
    } else if (payload.substring(stateStart, stateStart + 5) == "false") {
      stateStr = "false";
    } else {
      // unknown value; skip
      idx = stateStart;
      continue;
    }

    // accumulate for printing
    if (out.length() > 0) out += ", ";
    out += idStr + ": " + stateStr;

    // detect change and send one-shot I2C messages per relay
    int id = idStr.toInt();
    bool stateBool = (stateStr == "true");
    if (id >= 0 && id <= MAX_RELAY_ID) {
      if (!relaySeen[id]) {
        // First time we see this relay in this run: initialize
        relaySeen[id] = true;
        relayStateLast[id] = stateBool;
        // If we haven't performed the initial send and Firebase is connected, send each relay once now
        if (!relaysInitialSent) {
          String msg = String(id) + ":" + (stateBool ? "true" : "false");
          sendI2CMessage(msg);
        }
      } else {
        // Already seen previously; if changed, send once and update
        if (relayStateLast[id] != stateBool) {
          String msg = String(id) + ":" + (stateBool ? "true" : "false");
          sendI2CMessage(msg);
          relayStateLast[id] = stateBool;
        }
      }
    }

    // advance idx
    idx = stateStart + stateStr.length();
  }

  if (out.length() > 0) {
    Serial.println(out);
  // mark that we've sent the initial batch (if not already)
  if (!relaysInitialSent) relaysInitialSent = true;
  }
}

uint8_t getFingerprintID() {
  uint8_t p = finger.getImage();
  switch (p) {
    case FINGERPRINT_OK:
      break;
    case FINGERPRINT_NOFINGER:
      return p;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("Communication error");
      return p;
    case FINGERPRINT_IMAGEFAIL:
      Serial.println("Imaging error");
      return p;
    default:
      Serial.println("Unknown error");
      return p;
  }

  // OK success!
  p = finger.image2Tz();
  switch (p) {
    case FINGERPRINT_OK:
      break;
    case FINGERPRINT_IMAGEMESS:
      Serial.println("Image too messy");
      return p;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("Communication error");
      return p;
    case FINGERPRINT_FEATUREFAIL:
      
      return p;
    case FINGERPRINT_INVALIDIMAGE:
      
      return p;
    default:
      Serial.println("Unknown error");
      return p;
  }

  // OK converted!
  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    String personName = getFingerprintName(finger.fingerID);
    Serial.println("✅ ACCESS GRANTED!");
    Serial.println("Welcome " + personName + "!");
    
    // Log successful access to Firebase
    logFingerprintAccess("success", personName);
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("Communication error");
    return p;
  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("❌ ACCESS DENIED - Fingerprint not recognized");
    
    // Log failed access to Firebase
    logFingerprintAccess("failed", "unknown");
    return p;
  } else {
    Serial.println("Unknown error");
    return p;
  }

  return FINGERPRINT_OK;
}

void saveFingerprintName(uint8_t id, String name) {
  preferences.begin("fingerprints", false);
  String key = "name_" + String(id);
  preferences.putString(key.c_str(), name);
  preferences.end();
  Serial.println("Saved name '" + name + "' for ID " + String(id));
}

String getFingerprintName(uint8_t id) {
  preferences.begin("fingerprints", true);
  String key = "name_" + String(id);
  String name = preferences.getString(key.c_str(), "Unknown");
  preferences.end();
  return name;
}

void deleteFingerprintName(uint8_t id) {
  preferences.begin("fingerprints", false);
  String key = "name_" + String(id);
  preferences.remove(key.c_str());
  preferences.end();
}

// Buzzer alarm function - security breach style alarm
// Multi-stage pattern: rapid staccato burst, quick rising/falling wail, alternating loud blasts,
// and a short repeating finale. Active-low buzzer is explicitly silenced after each tone.
void buzzerAlarm() {
  Serial.println("🚨 SECURITY BREACH ALARM! 🚨");

  // Stage 1: Immediate attention - aggressive rapid staccato (short sharp pulses)
  const int STACCATO_COUNT = 10;
  for (int i = 0; i < STACCATO_COUNT; i++) {
    tone(BUZZER_PIN, NOTE_A5, 80); // sharp high ping
    delay(90);
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(60);
  }

  delay(150);

  // Stage 2: Rising then falling wail - creates an urgent siren sweep
  const int SWEEP_STEPS = 20;
  const int SWEEP_MIN = NOTE_C5; // low
  const int SWEEP_MAX = NOTE_A5; // high
  // Rising
  for (int i = 0; i <= SWEEP_STEPS; i++) {
    int freq = SWEEP_MIN + ((SWEEP_MAX - SWEEP_MIN) * i) / SWEEP_STEPS;
    tone(BUZZER_PIN, freq, 60);
    delay(70);
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, HIGH);
  }
  delay(80);
  // Falling
  for (int i = SWEEP_STEPS; i >= 0; i--) {
    int freq = SWEEP_MIN + ((SWEEP_MAX - SWEEP_MIN) * i) / SWEEP_STEPS;
    tone(BUZZER_PIN, freq, 60);
    delay(70);
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, HIGH);
  }

  delay(120);

  // Stage 3: Alternating loud blasts (high/low contrast) - feels like an alarm horn
  const int BLAST_CYCLES = 6;
  for (int i = 0; i < BLAST_CYCLES; i++) {
    tone(BUZZER_PIN, NOTE_A5, 250);
    delay(280);
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(80);

    tone(BUZZER_PIN, NOTE_D5, 200);
    delay(220);
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(120);
  }

  delay(200);

  // Stage 4: Short repeating final salvo (three quick repeats)
  for (int r = 0; r < 3; r++) {
    for (int j = 0; j < 3; j++) {
      tone(BUZZER_PIN, NOTE_G5, 120);
      delay(130);
      noTone(BUZZER_PIN);
      digitalWrite(BUZZER_PIN, HIGH);
      delay(60);
    }
    delay(250);
  }

  // Ensure silence at the end
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, HIGH);
  Serial.println("🔊 Security breach alarm sequence complete");
}

// Helper function for SSL client
void set_ssl_client_insecure_and_buffer(SSL_CLIENT &client) {
#if defined(ESP32) || defined(ESP8266)
    client.setInsecure();
#if defined(ESP8266)
    client.setBufferSizes(4096, 1024);
#endif
#endif
}

// Test internet connectivity by pinging Google DNS
bool testInternetConnectivity() {
  WiFiClient client;
  client.setTimeout(2000); // 2 second timeout instead of default
  if (client.connect("8.8.8.8", 53)) {
    client.stop();
    return true;
  }
  return false;
}

// Setup WiFi connection
void setupWiFi() {
  WiFi.disconnect(true);
  delay(500);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 50) {
    Serial.print(".");
    delay(300);
    attempts++;
    
    // Retry connection if failed
    if (attempts % 15 == 0) {
      if (WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_NO_SSID_AVAIL) {
        WiFi.disconnect();
        delay(500);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("✓ WiFi connected: ");
    Serial.println(WiFi.localIP());
    
    if (testInternetConnectivity()) {
      wifiConnected = true;
    } else {
      Serial.println("❌ No internet access");
      wifiConnected = false;
    }
  } else {
    Serial.println();
    Serial.println("❌ WiFi failed");
    wifiConnected = false;
  }
}

// Setup NTP for time synchronization
void setupNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    delay(500);
    attempts++;
  }
  
  if (getLocalTime(&timeinfo)) {
    Serial.println("✓ NTP synchronized");
    ntpSynced = true;
  } else {
    Serial.println("❌ NTP sync failed");
    ntpSynced = false;
  }
}

// Setup Firebase
void setupFirebase() {
  // Configure SSL
  set_ssl_client_insecure_and_buffer(ssl_client);
  
  // Initialize Firebase app using the UserAuth object directly
  Firebase.initializeApp(aClient, app, getAuth(user_auth));
  
  // Get database instance
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
  
  Serial.println("✓ Firebase ready");
  firebaseConnected = true; // Will be verified in monitorConnections()
}

// Get current date and time in required format
String getCurrentDateTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "1970-01-01 00:00:00";
  }
  
  char timeStringBuff[50];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(timeStringBuff);
}

// Check failed attempts from Firebase
void checkFailedAttempts() {
  // Only check periodically to avoid overwhelming Firebase
  if (millis() - lastFailedAttemptsCheck < FAILED_ATTEMPTS_CHECK_INTERVAL) {
    return;
  }
  lastFailedAttemptsCheck = millis();
  
  // Check if Firebase is ready and connected
  if (!app.ready() || !wifiConnected || !firebaseConnected) {
    // If no connection, use local counter only
    systemLocked = (currentFailedAttempts >= MAX_FAILED_ATTEMPTS);
    return;
  }
  
  // Read failed attempts from Firebase using async approach (from example)
  String failedAttemptsPath = "/devices/fingerprint_door_001/failed_attempts";
  
  // Use async get to read the value (non-blocking)
  Database.get(aClient, failedAttemptsPath, databaseResult);
  
  // For immediate response, also check local counter
  systemLocked = (currentFailedAttempts >= MAX_FAILED_ATTEMPTS);
}

// Force immediate sync of failed attempts from Firebase
void syncFailedAttemptsFromFirebase() {
  // Check if Firebase is ready and connected
  if (!app.ready() || !wifiConnected || !firebaseConnected) {
    Serial.println("⚠️ Cannot sync failed attempts - Firebase not ready");
    return;
  }
  
  Serial.println("🔄 Force syncing failed attempts from Firebase...");
  String failedAttemptsPath = "/devices/fingerprint_door_001/failed_attempts";
  
  // Use blocking get to read the value immediately (from example)
  int firebaseFailedAttempts = Database.get<int>(aClient, failedAttemptsPath);
  
  // Check if the operation was successful
  if (aClient.lastError().code() == 0) {
    // Update local counter with Firebase value
    if (currentFailedAttempts != firebaseFailedAttempts) {
      Serial.printf("🔄 Syncing failed attempts: Local=%d → Firebase=%d\n", 
                    currentFailedAttempts, firebaseFailedAttempts);
      currentFailedAttempts = firebaseFailedAttempts;
      
      // Update lock status based on synced value
      bool previousLockStatus = systemLocked;
      systemLocked = (currentFailedAttempts >= MAX_FAILED_ATTEMPTS);
      
      if (previousLockStatus != systemLocked) {
        if (systemLocked) {
          Serial.println("🔒 SYSTEM LOCKED due to failed attempts");
        } else {
          Serial.println("🔓 SYSTEM UNLOCKED - Failed attempts reset");
        }
      }
    } else {
      Serial.printf("✓ Failed attempts already in sync: %d\n", currentFailedAttempts);
    }
  } else {
    Serial.printf("❌ Failed to read failed attempts: %s (code: %d)\n", 
                  aClient.lastError().message().c_str(), aClient.lastError().code());
  }
}

// Check if system is locked due to too many failed attempts
bool isSystemLocked() {
  return systemLocked;
}

// Log fingerprint access attempts to Firebase
void logFingerprintAccess(String status, String user, int attempts) {
  // Always update local counters first
  if (status == "failed") {
    currentFailedAttempts++;
    systemLocked = (currentFailedAttempts >= MAX_FAILED_ATTEMPTS);
    Serial.printf("🚨 Failed attempt recorded: %d/%d %s\n", 
                  currentFailedAttempts, MAX_FAILED_ATTEMPTS,
                  systemLocked ? "(SYSTEM LOCKED)" : "");
    
    // Sound buzzer alarm when system gets locked
    if (systemLocked) {
      // Send exact short alert string required
      sendI2CMessage("lock");
      Serial.println("🔊 SECURITY ALERT - Sounding alarm!");
      buzzerAlarm();
    }
  } else if (status == "success") {
    currentFailedAttempts = 0;
    systemLocked = false;
    Serial.println("✅ Access granted - Failed attempts counter reset");
  // Notify Mega to unlock (one-shot)
  Serial.println("-> Sending unlock to Mega");
  sendI2CMessage("unlock");
  }
  
  // Check if Firebase app is ready and connections are available
  if (!app.ready() || !wifiConnected || !firebaseConnected) {
    Serial.printf("⚠️ Cannot log to Firebase - connections not ready\n");
    Serial.printf("   WiFi: %s, Firebase: %s, App Ready: %s\n", 
                  wifiConnected ? "OK" : "FAIL",
                  firebaseConnected ? "OK" : "FAIL", 
                  app.ready() ? "OK" : "FAIL");
    Serial.printf("   Local tracking: %s - %s\n", status.c_str(), user.c_str());
    return;
  }
  
  String currentDateTime = getCurrentDateTime();
  
  // Extract date and time components
  String date = currentDateTime.substring(0, 10);  // YYYY-MM-DD
  String time = currentDateTime.substring(11, 19); // HH:MM:SS
  
  // Build the path according to database structure
  String logPath = "/devices/fingerprint_door_001/logs/" + date + "/" + time;
  
  Serial.printf("=== FIREBASE OPERATION START ===\n");
  Serial.printf("Logging access: %s - %s - %s\n", status.c_str(), user.c_str(), currentDateTime.c_str());
  Serial.printf("Path: %s\n", logPath.c_str());
  
  // Set status
  bool statusSet = Database.set<String>(aClient, logPath + "/status", status);
  
  // Set user
  bool userSet = Database.set<String>(aClient, logPath + "/user", user);
  
  if (statusSet && userSet) {
    Serial.println("✓ Access log saved to Firebase");
    
    // Update last_updated timestamp
    String lastUpdatedPath = "/devices/fingerprint_door_001/last_updated";
    Database.set<String>(aClient, lastUpdatedPath, currentDateTime);
    
    // Update failed attempts counter in Firebase
    String failedAttemptsPath = "/devices/fingerprint_door_001/failed_attempts";
    Database.set<int>(aClient, failedAttemptsPath, currentFailedAttempts);
    
  } else {
    Serial.printf("❌ Failed to save access log\n");
    Serial.printf("❌ Error message: %s\n", aClient.lastError().message().c_str());
    Serial.printf("❌ Error code: %d\n", aClient.lastError().code());
    
    // Mark Firebase as disconnected for monitoring system
    firebaseConnected = false;
  }
}

// Process Firebase async results
void processFirebaseData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;

  if (aResult.isEvent()) {
    Firebase.printf("Event task: %s, msg: %s, code: %d\n", 
                   aResult.uid().c_str(), 
                   aResult.eventLog().message().c_str(), 
                   aResult.eventLog().code());
  }

  if (aResult.isError()) {
    Firebase.printf("Error task: %s, msg: %s, code: %d\n", 
                   aResult.uid().c_str(), 
                   aResult.error().message().c_str(), 
                   aResult.error().code());
  }

  if (aResult.available()) {
    String payload = aResult.c_str();
    Firebase.printf("Task: %s, payload: %s\n", 
                   aResult.uid().c_str(), 
                   payload.c_str());
    
    // Always try to parse and update failed attempts from any numeric response
    // This is more robust than checking dataPath
    if (!payload.isEmpty() && payload != "null") {
      int firebaseFailedAttempts = payload.toInt();
      
      // Only update if it's a reasonable value (0-10)
      if (firebaseFailedAttempts >= 0 && firebaseFailedAttempts <= 10) {
        
        // Update local counter with Firebase value
        if (currentFailedAttempts != firebaseFailedAttempts) {
          Serial.printf("🔄 Background sync: Local=%d → Firebase=%d\n", 
                        currentFailedAttempts, firebaseFailedAttempts);
          currentFailedAttempts = firebaseFailedAttempts;
          
          // Update lock status based on synced value
          bool previousLockStatus = systemLocked;
          systemLocked = (currentFailedAttempts >= MAX_FAILED_ATTEMPTS);
          
          if (previousLockStatus != systemLocked) {
            if (systemLocked) {
              Serial.println("🔒 SYSTEM LOCKED due to failed attempts");
            } else {
              Serial.println("🔓 SYSTEM UNLOCKED - Failed attempts reset");
            }
          }
        }
      } else if (payload == "null" || payload.isEmpty()) {
        // Handle null/empty as 0 failed attempts
        if (currentFailedAttempts != 0) {
          Serial.printf("🔄 Background sync: Local=%d → Firebase=0 (null)\n", currentFailedAttempts);
          currentFailedAttempts = 0;
          
          bool previousLockStatus = systemLocked;
          systemLocked = false;
          
          if (previousLockStatus != systemLocked) {
            Serial.println("🔓 SYSTEM UNLOCKED - Failed attempts reset to 0");
          }
        }
      }
    }
  }
}

// Connection monitoring and auto-reconnection system
void monitorConnections() {
  // Skip if another check is in progress to avoid blocking
  if (connectionCheckInProgress) return;
  
  unsigned long currentTime = millis();
  
  // Check WiFi connection periodically
  if (currentTime - lastWiFiCheck >= WIFI_CHECK_INTERVAL) {
    lastWiFiCheck = currentTime;
    if (!checkWiFiConnection()) {
      reconnectWiFi();
    }
  }
  
  // Check Firebase connection periodically (only if WiFi is connected)
  if (wifiConnected && currentTime - lastFirebaseCheck >= FIREBASE_CHECK_INTERVAL) {
    lastFirebaseCheck = currentTime;
    if (!checkFirebaseConnection()) {
      reconnectFirebase();
  // mark initial relay send to be retried on reconnect
  relaysInitialSent = false;
  // also mark doorLock initial send to be retried on reconnect
  doorLockInitialSent = false;
    }
  }
  
  // Check NTP sync periodically (only if WiFi is connected)
  if (wifiConnected && currentTime - lastNTPCheck >= NTP_CHECK_INTERVAL) {
    lastNTPCheck = currentTime;
    if (!checkNTPSync()) {
      resyncNTP();
    }
  }
}

// Check WiFi connection status
bool checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    return false;
  }
  
  // Quick internet connectivity test
  if (!testInternetConnectivity()) {
    wifiConnected = false;
    return false;
  }
  
  wifiConnected = true;
  return true;
}

// Check Firebase connection status
bool checkFirebaseConnection() {
  if (!app.ready()) {
    firebaseConnected = false;
    return false;
  }
  
  firebaseConnected = true;
  return true;
}

// Check NTP synchronization status
bool checkNTPSync() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    ntpSynced = false;
    return false;
  }
  
  // Additional check: ensure time is not 1970 (epoch)
  if (timeinfo.tm_year < 120) { // 120 = 2020 (years since 1900)
    ntpSynced = false;
    return false;
  }
  
  ntpSynced = true;
  return true;
}

// Reconnect WiFi (non-blocking)
void reconnectWiFi() {
  connectionCheckInProgress = true;
  Serial.println("🔄 WiFi reconnecting...");
  
  // Proper disconnect and reset
  WiFi.disconnect(true);
  delay(500);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Quick non-blocking check with better timeout
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 15) {
    delay(400);
    attempts++;
    
    // Check status every 5 attempts
    if (attempts % 5 == 0) {
      Serial.printf("(%d)", WiFi.status());
      if (WiFi.status() == WL_CONNECT_FAILED) {
        Serial.print("retry-");
        WiFi.disconnect();
        delay(200);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    if (testInternetConnectivity()) {
      Serial.println("✓ WiFi reconnected");
      wifiConnected = true;
    } else {
      Serial.println("❌ No internet access");
      wifiConnected = false;
    }
  } else {
    Serial.println("❌ WiFi reconnection failed");
    wifiConnected = false;
  }
  
  connectionCheckInProgress = false;
}

// Reconnect Firebase (non-blocking)
void reconnectFirebase() {
  if (!wifiConnected) return; // Skip if no WiFi
  
  connectionCheckInProgress = true;
  Serial.println("🔄 Firebase reconnecting...");
  
  // Reinitialize Firebase app
  Firebase.initializeApp(aClient, app, getAuth(user_auth));
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
  
  // Brief wait for initialization
  delay(1000);
  
  if (app.ready()) {
    Serial.println("✓ Firebase reconnected");
    firebaseConnected = true;
  } else {
    Serial.println("❌ Firebase reconnection failed");
    firebaseConnected = false;
  }
  
  connectionCheckInProgress = false;
}

// Resync NTP (non-blocking)
void resyncNTP() {
  if (!wifiConnected) return; // Skip if no WiFi
  
  connectionCheckInProgress = true;
  Serial.println("🔄 NTP resyncing...");
  
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Quick non-blocking check
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 3) {
    delay(500);
    attempts++;
  }
  
  if (getLocalTime(&timeinfo) && timeinfo.tm_year >= 120) {
    Serial.println("✓ NTP resynchronized");
    ntpSynced = true;
  } else {
    Serial.println("❌ NTP resync failed");
    ntpSynced = false;
  }
  
  connectionCheckInProgress = false;
}
