/***************************************************
  ESP8266 Fingerprint Access Control - SUPER SIMPLIFIED
  Memory optimized - Real-time relay monitoring only
 ****************************************************/
#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <WiFiClientSecure.h>
#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include "secrets.h"
#include <ESP8266WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFiUdp.h>
#include <time.h>

// Hardware setup
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&Serial);
Preferences preferences;

// Firebase minimal setup
WiFiClientSecure ssl_client;
AsyncClientClass aClient(ssl_client);
UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000);
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult databaseResult;

// Connection status
bool wifiConnected = false;
bool firebaseConnected = false;

// Relay monitoring
unsigned long lastRelaysCheck = 0;
const unsigned long RELAYS_CHECK_INTERVAL = 1500; // 1.5 seconds for real-time
const int MAX_RELAY_ID = 8;
bool relayStateLast[MAX_RELAY_ID + 1] = {false};
bool relaysInitialized = false;

// Door lock
unsigned long lastDoorLockCheck = 0;
const unsigned long DOORLOCK_CHECK_INTERVAL = 1000; // 1 second
bool doorLockStateLast = false;
bool isDoorLocked = true;

// Failed attempts tracking
int currentFailedAttempts = 0;
const int MAX_FAILED_ATTEMPTS = 3;
bool systemLocked = false;
unsigned long lastFailedAttemptsCheck = 0;
const unsigned long FAILED_ATTEMPTS_CHECK_INTERVAL = 5000; // Check every 5 seconds

// Water level monitoring
#define FLOAT_PIN 12  // NodeMCU D6 -> GPIO12
unsigned long lastFloatRead = 0;
const unsigned long FLOAT_READ_INTERVAL = 2000; // Check every 2 seconds
bool lastFloatState = false;

// Buzzer pin
#define BUZZER_PIN 13  // NodeMCU D7 -> GPIO13

// Buzzer pin (NodeMCU D7 -> GPIO13)
#define BUZZER_PIN 13

// Simple I2C sender
bool sendI2CMessage(const char* msg) {
  Wire.beginTransmission(0x08);
  Wire.write(msg, strlen(msg));
  return (Wire.endTransmission() == 0);
}

// Simple buzzer alarm for security breach
void buzzerAlarm() {
  Serial.println("🚨 SECURITY BREACH ALARM! 🚨");
  
  // Quick aggressive alarm pattern
  for (int i = 0; i < 10; i++) {
    digitalWrite(BUZZER_PIN, LOW);  // Turn on buzzer
    delay(200);
    digitalWrite(BUZZER_PIN, HIGH); // Turn off buzzer
    delay(100);
  }
  
  Serial.println("🔊 Security alarm complete");
}

// Simple relay check
void fetchRelays() {
  if (millis() - lastRelaysCheck < RELAYS_CHECK_INTERVAL) return;
  lastRelaysCheck = millis();
  if (!app.ready() || !firebaseConnected) return;

  for (int id = 1; id <= MAX_RELAY_ID; id++) {
    char path[50];
    snprintf(path, sizeof(path), "/smart_controls/relays/%d/state", id);
    
    bool state = Database.get<bool>(aClient, path);
    if (aClient.lastError().code() != 0) {
      firebaseConnected = false;
      return;
    }

    if (!relaysInitialized || relayStateLast[id] != state) {
      char msg[8];
      snprintf(msg, sizeof(msg), "%d:%d", id, state ? 1 : 0);
      sendI2CMessage(msg);
      relayStateLast[id] = state;
    }
  }
  relaysInitialized = true;
}

// Simple door lock check
void fetchDoorLock() {
  if (millis() - lastDoorLockCheck < DOORLOCK_CHECK_INTERVAL) return;
  lastDoorLockCheck = millis();
  if (!app.ready() || !firebaseConnected) return;

  bool value = Database.get<bool>(aClient, "/smart_controls/relays/door/isLocked");
  if (aClient.lastError().code() == 0) {
    isDoorLocked = value;
    if (doorLockStateLast != value) {
      sendI2CMessage(value ? "lock" : "unlock");
      doorLockStateLast = value;
    }
  } else {
    firebaseConnected = false;
  }
}

// Check failed attempts from Firebase (allows remote reset)
void checkFailedAttempts() {
  if (millis() - lastFailedAttemptsCheck < FAILED_ATTEMPTS_CHECK_INTERVAL) return;
  lastFailedAttemptsCheck = millis();
  
  if (!app.ready() || !firebaseConnected) return;
  
  int firebaseFailedAttempts = Database.get<int>(aClient, "/devices/fingerprint_door_001/failed_attempts");
  if (aClient.lastError().code() == 0) {
    // If Firebase value is different from local, sync
    if (currentFailedAttempts != firebaseFailedAttempts) {
      Serial.printf("🔄 Syncing failed attempts: Local=%d → Firebase=%d\n", 
                    currentFailedAttempts, firebaseFailedAttempts);
      currentFailedAttempts = firebaseFailedAttempts;
      
      // Update system lock status
      bool wasLocked = systemLocked;
      systemLocked = (currentFailedAttempts >= MAX_FAILED_ATTEMPTS);
      
      if (wasLocked && !systemLocked) {
        Serial.println("🔓 SYSTEM UNLOCKED - Failed attempts reset remotely");
      } else if (!wasLocked && systemLocked) {
        Serial.println("🔒 SYSTEM LOCKED - Failed attempts updated from Firebase");
      }
    }
  }
}

// Get fingerprint user name from preferences
String getFingerprintUserName(int id) {
  String key = "fp_" + String(id);
  String name = preferences.getString(key.c_str(), "Unknown");
  return name;
}

// Create timestamped log entry in Firebase
void logFingerprintAccess(bool success, String userName) {
  if (!app.ready() || !firebaseConnected) return;
  
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  
  // Check if we have valid time from NTP
  if (now < 1000000000) { // If time is not properly set (before year 2001)
    // Fallback to simple incrementing counter with system date
    static int logCounter = 0;
    logCounter++;
    
    char statusPath[100];
    char userPath[100];
    char systemDate[12] = "system";
    char timeKey[20];
    snprintf(timeKey, sizeof(timeKey), "entry_%d", logCounter);
    
    // Create individual property paths
    snprintf(statusPath, sizeof(statusPath), "/devices/fingerprint_door_001/logs/%s/%s/status", systemDate, timeKey);
    snprintf(userPath, sizeof(userPath), "/devices/fingerprint_door_001/logs/%s/%s/user", systemDate, timeKey);
    
    // Write status and user as separate properties
    Database.set<String>(aClient, statusPath, success ? "success" : "failed");
    Database.set<String>(aClient, userPath, userName);
    
    // Update last_updated with basic timestamp
    Database.set<String>(aClient, "/devices/fingerprint_door_001/last_updated", "system_time_unavailable");
    
    Serial.printf("📝 Logged fingerprint access (counter %d): %s - %s\n", 
                  logCounter, success ? "SUCCESS" : "FAILED", userName.c_str());
    return;
  }
  
  // Format date as YYYY-MM-DD (matching your database structure)
  char dateStr[12];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", timeinfo);
  
  // Format time as HH:MM:SS (matching your database structure)
  char timeStr[10];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", timeinfo);
  
  // Create log entry paths - follows exact structure from your JSON
  char statusPath[120];
  char userPath[120];
  
  snprintf(statusPath, sizeof(statusPath), "/devices/fingerprint_door_001/logs/%s/%s/status", dateStr, timeStr);
  snprintf(userPath, sizeof(userPath), "/devices/fingerprint_door_001/logs/%s/%s/user", dateStr, timeStr);
  
  // Write status and user as separate properties (matching your JSON structure)
  Database.set<String>(aClient, statusPath, success ? "success" : "failed");
  Database.set<String>(aClient, userPath, userName);
  
  // Update last_updated timestamp (matching format: "2025-09-15 01:35:31")
  char lastUpdatedStr[20];
  snprintf(lastUpdatedStr, sizeof(lastUpdatedStr), "%s %s", dateStr, timeStr);
  Database.set<String>(aClient, "/devices/fingerprint_door_001/last_updated", lastUpdatedStr);
  
  Serial.printf("📝 Logged fingerprint access: %s - %s (%s)\n", 
                success ? "SUCCESS" : "FAILED", userName.c_str(), lastUpdatedStr);
}

// Simple fingerprint scan with failed attempts tracking and logging
uint8_t getFingerprintID() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return p;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return p;

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK) {
    Serial.println("✅ ACCESS GRANTED!");
    Serial.printf("ID: %d, Confidence: %d\n", finger.fingerID, finger.confidence);
    
    // Get user name from preferences
    String userName = getFingerprintUserName(finger.fingerID);
    Serial.printf("👤 User: %s\n", userName.c_str());
    
    // Log successful access
    logFingerprintAccess(true, userName);
    
    // Reset failed attempts on successful access
    currentFailedAttempts = 0;
    systemLocked = false;
    
    // Update failed attempts in Firebase
    if (app.ready() && firebaseConnected) {
      Database.set<int>(aClient, "/devices/fingerprint_door_001/failed_attempts", 0);
    }
    
    // Unlock door
    sendI2CMessage("unlock");
    isDoorLocked = false;
    
    // Update Firebase door state
    if (app.ready() && firebaseConnected) {
      Database.set<bool>(aClient, "/smart_controls/relays/door/isLocked", false);
    }
  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("❌ ACCESS DENIED");
    
    // Log failed access attempt
    logFingerprintAccess(false, "unknown");
    
    // Increment failed attempts
    currentFailedAttempts++;
    Serial.printf("🚨 Failed attempt: %d/%d\n", currentFailedAttempts, MAX_FAILED_ATTEMPTS);
    
    // Update failed attempts in Firebase
    if (app.ready() && firebaseConnected) {
      Database.set<int>(aClient, "/devices/fingerprint_door_001/failed_attempts", currentFailedAttempts);
    }
    
    // Check if system should be locked
    if (currentFailedAttempts >= MAX_FAILED_ATTEMPTS) {
      systemLocked = true;
      Serial.println("🔒 SYSTEM LOCKED - Too many failed attempts!");
      
      // Sound alarm
      buzzerAlarm();
      
      // Send alert to mega slave for SMS notification
      sendI2CMessage("alert");
      
      // Lock the door
      sendI2CMessage("lock");
      
      // Update Firebase door state to locked
      if (app.ready() && firebaseConnected) {
        Database.set<bool>(aClient, "/smart_controls/relays/door/isLocked", true);
      }
    }
  }
  return p;
}

// Simple WiFi setup
void setupWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK");
    wifiConnected = true;
  } else {
    Serial.println(" FAILED");
    wifiConnected = false;
  }
}

// Water level monitoring
void checkWaterLevel() {
  if (millis() - lastFloatRead < FLOAT_READ_INTERVAL) return;
  lastFloatRead = millis();
  
  bool state = (digitalRead(FLOAT_PIN) == LOW); // LOW = water present (closed switch)
  
  // Only act when state changes
  if (state != lastFloatState) {
    lastFloatState = state;
    
    // Send I2C message for water state change
    if (state) {
      sendI2CMessage("waterpresent");
      Serial.println("💧 WATER PRESENT");
    } else {
      sendI2CMessage("waterempty");
      Serial.println("🚨 WATER EMPTY");
    }
    
    // Update Firebase if available
    if (app.ready() && firebaseConnected) {
      String status = state ? "water_present" : "water_empty";
      String tank_status = state ? "normal" : "alert";
      
      // Update water level status
      Database.set<bool>(aClient, "/devices/water_level_001/water_level", state);
      Database.set<String>(aClient, "/devices/water_level_001/status", status);
      Database.set<String>(aClient, "/devices/water_level_001/tank_status", tank_status);
      
      Serial.printf("💾 Water level updated in Firebase: %s\n", status.c_str());
    }
  }
}

// Simple Firebase setup
void setupFirebase() {
  ssl_client.setInsecure();
  Firebase.initializeApp(aClient, app, getAuth(user_auth));
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
  firebaseConnected = false; // Will be set when first operation succeeds
}

void setup() {
  Serial.begin(57600);
  Serial.println("ESP8266 Simple Fingerprint Control");
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  Wire.begin(D2, D1);
  
  // Initialize water level sensor pin
  pinMode(FLOAT_PIN, INPUT_PULLUP);
  lastFloatState = (digitalRead(FLOAT_PIN) == LOW);
  Serial.printf("💧 Water sensor initial: %s\n", lastFloatState ? "PRESENT" : "EMPTY");
  
  // Initialize buzzer pin (active low - HIGH = off)
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);
  
  if (finger.verifyPassword()) {
    Serial.println("Fingerprint sensor OK");
  }
  
  preferences.begin("fingerprints", false);
  setupWiFi();
  
  // Configure time for logging
  if (wifiConnected) {
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // UTC+8 (adjust timezone as needed)
    Serial.println("⏰ Time configured for logging");
  }
  
  setupFirebase();
  
  Serial.println("Setup complete");
}

void loop() {
  ESP.wdtFeed();
  app.loop();
  
  // Simple connection check
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    if (app.ready()) {
      firebaseConnected = true;
    }
  } else {
    wifiConnected = false;
    firebaseConnected = false;
  }
  
  // Real-time relay monitoring
  fetchRelays();
  fetchDoorLock();
  checkFailedAttempts(); // Check for remote reset of failed attempts
  
  // Water level monitoring
  checkWaterLevel();
  
  // Only scan if door is locked AND system is not locked due to failed attempts
  if (isDoorLocked && !systemLocked) {
    getFingerprintID();
  } else if (systemLocked) {
    // Show lockout message periodically
    static unsigned long lastLockoutMessage = 0;
    if (millis() - lastLockoutMessage > 10000) { // Every 10 seconds
      Serial.println("🔒 SYSTEM LOCKED - Reset failed_attempts in Firebase to unlock");
      lastLockoutMessage = millis();
    }
  }
}