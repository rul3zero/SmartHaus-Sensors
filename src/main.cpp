/***************************************************
  ESP8266 Fingerprint Access Control - SUPER SIMPLIFIED
  Memory optimized - Real-time relay monitoring only
  Note: WireGuard is not available on ESP8266 (ESP32 only)
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
bool systemReady = false;

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

// Auto-lock timer
unsigned long doorUnlockedTime = 0;
const unsigned long AUTO_LOCK_TIMEOUT = 30000; // 30 seconds
bool autoLockEnabled = false;

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

// Water empty countdown (30 second delay before pump activation)
bool waterEmptyCountdownActive = false;
unsigned long waterEmptyDetectedTime = 0;
const unsigned long WATER_EMPTY_DELAY = 30000; // 30 seconds delay
const unsigned long WATER_DATABASE_UPDATE_ADVANCE = 10000; // Update database 10 seconds before pump
bool waterEmptyCommandSent = false;
bool waterEmptyDatabaseUpdated = false;
unsigned long lastCountdownLog = 0;

// Heartbeat/Health Check
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 10000; // Send heartbeat every 10 seconds
static int heartbeatCounter = 0;
const int HEARTBEAT_LOG_FREQUENCY = 6; // Log every 6th heartbeat (once per minute)

// Component Health Status
bool fingerprintSensorHealthy = false;
bool waterSensorHealthy = false;
bool megaI2CHealthy = false;
unsigned long lastFingerprintHealthCheck = 0;
unsigned long lastI2CHealthCheck = 0;
unsigned long lastWaterHealthCheck = 0;
const unsigned long HEALTH_CHECK_INTERVAL = 5000; // Check component health every 5 seconds
unsigned long lastSuccessfulI2C = 0;
const unsigned long I2C_TIMEOUT = 15000; // Consider I2C offline after 15 seconds of no success

// Buzzer pin (NodeMCU D7 -> GPIO13)
#define BUZZER_PIN 13

// Simple I2C sender with health tracking
bool sendI2CMessage(const char* msg) {
  Wire.beginTransmission(0x08);
  Wire.write(msg, strlen(msg));
  bool success = (Wire.endTransmission() == 0);
  
  // Track successful I2C communication for health monitoring
  if (success) {
    lastSuccessfulI2C = millis();
    megaI2CHealthy = true;
  }
  
  return success;
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
      
      // If door is manually unlocked from Firebase, start auto-lock timer
      if (!value) {
        doorUnlockedTime = millis();
        autoLockEnabled = true;
        Serial.println("⏱️ Auto-lock timer started (30 seconds)");
      } else {
        autoLockEnabled = false;
      }
    }
  } else {
    firebaseConnected = false;
  }
}

// Auto-lock door after timeout
void checkAutoLock() {
  if (!autoLockEnabled || isDoorLocked) return;
  
  unsigned long elapsed = millis() - doorUnlockedTime;
  
  // Show countdown every 10 seconds
  static unsigned long lastCountdown = 0;
  if (elapsed - lastCountdown >= 10000) {
    int remaining = (AUTO_LOCK_TIMEOUT - elapsed) / 1000;
    if (remaining > 0) {
      Serial.printf("⏱️ Auto-lock in %d seconds...\n", remaining);
    }
    lastCountdown = elapsed;
  }
  
  // Lock door after timeout
  if (elapsed >= AUTO_LOCK_TIMEOUT) {
    Serial.println("🔒 AUTO-LOCK: 30 seconds elapsed - locking door");
    
    // Lock the door
    sendI2CMessage("lock");
    isDoorLocked = true;
    autoLockEnabled = false;
    
    // Update Firebase door state
    if (app.ready() && firebaseConnected) {
      Database.set<bool>(aClient, "/smart_controls/relays/door/isLocked", true);
    }
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
    // Try to get the most recent timestamp from logs
    String lastUpdated = Database.get<String>(aClient, "/devices/fingerprint_door_001/last_updated");
    if (aClient.lastError().code() == 0 && lastUpdated.length() > 0) {
      // Use lastUpdated as fallback timestamp
      Database.set<String>(aClient, "/devices/fingerprint_door_001/last_updated", lastUpdated);
      Serial.printf("📝 Logged fingerprint access (no NTP): %s - %s (using last_updated: %s)\n",
                    success ? "SUCCESS" : "FAILED", userName.c_str(), lastUpdated.c_str());
    } else {
      // Fallback to simple incrementing counter if no previous timestamp
      static int logCounter = 0;
      logCounter++;
      char systemDate[12] = "system";
      char timeKey[20];
      snprintf(timeKey, sizeof(timeKey), "entry_%d", logCounter);

      char statusPath[100];
      char userPath[100];
      snprintf(statusPath, sizeof(statusPath), "/devices/fingerprint_door_001/logs/%s/%s/status", systemDate, timeKey);
      snprintf(userPath, sizeof(userPath), "/devices/fingerprint_door_001/logs/%s/%s/user", systemDate, timeKey);

      Database.set<String>(aClient, statusPath, success ? "success" : "failed");
      Database.set<String>(aClient, userPath, userName);

      Database.set<String>(aClient, "/devices/fingerprint_door_001/last_updated", String("fallback_counter_") + logCounter);

      Serial.printf("📝 Logged fingerprint access (counter %d): %s - %s\n",
                    logCounter, success ? "SUCCESS" : "FAILED", userName.c_str());
    }
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
    
    // Start auto-lock timer
    doorUnlockedTime = millis();
    autoLockEnabled = true;
    Serial.println("⏱️ Auto-lock timer started (30 seconds)");
    
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

// Setup NTP time synchronization
void setupNTP() {
  Serial.println("\n=== SYNCHRONIZING TIME (NTP) ===");
  Serial.println("Connecting to NTP server...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  
  // Wait for time to be set
  int retries = 0;
  time_t now = 0;
  struct tm timeinfo;
  memset(&timeinfo, 0, sizeof(timeinfo));
  
  while (timeinfo.tm_year < (2020 - 1900) && retries < 20) {
    delay(500);
    time(&now);
    localtime_r(&now, &timeinfo);
    retries++;
    Serial.print(".");
  }
  
  Serial.println();
  if (timeinfo.tm_year < (2020 - 1900)) {
    Serial.println("❌ Failed to obtain time from NTP server");
    return;
  }
  
  Serial.println("✅ NTP time synchronized successfully");
  Serial.print("📅 Current time: ");
  Serial.println(asctime(&timeinfo));
  Serial.println("=== NTP READY ===");
}

// Setup DNS (automatic via WiFi)
void setupDNS() {
  Serial.println("\n=== CONFIGURING DNS ===");
  Serial.print("DNS Server 1: ");
  Serial.println(WiFi.dnsIP(0));
  Serial.print("DNS Server 2: ");
  Serial.println(WiFi.dnsIP(1));
  Serial.println("✅ DNS configured successfully");
  Serial.println("=== DNS READY ===");
}

// Simple WiFi setup
void setupWiFi() {
  Serial.println("\n=== CONNECTING TO WIFI ===");
  Serial.println("⚠️  Note: WireGuard VPN is not available on ESP8266 (ESP32 only)");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to: %s\n", WIFI_SSID);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("✅ WiFi connected successfully");
    Serial.print("📍 Local IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("📶 Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.println("=== WIFI READY ===");
  } else {
    wifiConnected = false;
    Serial.println("❌ WiFi connection FAILED");
    Serial.println("⚠️  System cannot continue without WiFi");
  }
}

// Check fingerprint sensor health
void checkFingerprintSensorHealth() {
  if (millis() - lastFingerprintHealthCheck < HEALTH_CHECK_INTERVAL) return;
  lastFingerprintHealthCheck = millis();
  
  // Verify fingerprint sensor responds (non-blocking check)
  bool previousHealth = fingerprintSensorHealthy;
  fingerprintSensorHealthy = finger.verifyPassword();
  
  // Log status change
  if (previousHealth != fingerprintSensorHealthy) {
    if (fingerprintSensorHealthy) {
      Serial.println("👆 Fingerprint sensor: ONLINE");
    } else {
      Serial.println("❌ Fingerprint sensor: OFFLINE");
    }
  }
}

// Check water level sensor health
void checkWaterSensorHealth() {
  if (millis() - lastWaterHealthCheck < HEALTH_CHECK_INTERVAL) return;
  lastWaterHealthCheck = millis();
  
  // Basic check: verify we can read the GPIO pin
  // Note: This mainly detects if pin is functional, not physical disconnection
  bool previousHealth = waterSensorHealthy;
  pinMode(FLOAT_PIN, INPUT_PULLUP);
  int reading = digitalRead(FLOAT_PIN);
  
  // If we can read the pin (either HIGH or LOW), sensor is responsive
  waterSensorHealthy = (reading == HIGH || reading == LOW);
  
  // Log status change
  if (previousHealth != waterSensorHealthy) {
    if (waterSensorHealthy) {
      Serial.println("💧 Water sensor: ONLINE");
    } else {
      Serial.println("❌ Water sensor: OFFLINE");
    }
  }
}

// Check Arduino Mega I2C connection health
void checkMegaI2CHealth() {
  if (millis() - lastI2CHealthCheck < HEALTH_CHECK_INTERVAL) return;
  lastI2CHealthCheck = millis();
  
  bool previousHealth = megaI2CHealthy;
  
  // Check if we've had successful I2C communication recently
  if (millis() - lastSuccessfulI2C > I2C_TIMEOUT) {
    // No successful I2C communication in timeout period
    megaI2CHealthy = false;
  }
  // If we have recent successful communication, megaI2CHealthy is already true
  
  // Log status change
  if (previousHealth != megaI2CHealthy) {
    if (megaI2CHealthy) {
      Serial.println("🔌 Arduino Mega I2C: ONLINE");
    } else {
      Serial.println("❌ Arduino Mega I2C: OFFLINE (no successful communication)");
    }
  }
}

// Send heartbeat to Firebase for health monitoring
void sendHeartbeat() {
  // Check timing interval
  if (millis() - lastHeartbeat < HEARTBEAT_INTERVAL) return;
  lastHeartbeat = millis();
  
  // Only send if Firebase is ready and connected
  if (!app.ready() || !firebaseConnected) {
    // Silently skip if not connected - will retry on next interval
    return;
  }
  
  // Get current time for timestamp
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  
  // Check if NTP time is valid
  if (now < 1000000000) {
    // NTP time not synchronized yet - skip heartbeat
    // Only log occasionally to avoid spam
    if (heartbeatCounter % HEARTBEAT_LOG_FREQUENCY == 0) {
      Serial.println("⚠️  Heartbeat skipped: NTP time not synchronized");
    }
    heartbeatCounter++;
    return;
  }
  
  // Format timestamp as "YYYY-MM-DD HH:MM:SS"
  char timestamp[20];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
  
  // Track Firebase write success
  bool allSuccess = true;
  
  // 1. Update heartbeat for fingerprint_door_001 - only if component is healthy
  if (fingerprintSensorHealthy) {
    if (!Database.set<String>(aClient, "/devices/fingerprint_door_001/last_seen", String(timestamp))) {
      allSuccess = false;
    }
  }
  // Always update online status (even when offline)
  if (allSuccess) {
    if (!Database.set<bool>(aClient, "/devices/fingerprint_door_001/online", fingerprintSensorHealthy)) {
      allSuccess = false;
    }
  }
  
  // 2. Update heartbeat for water_level_001 - only if component is healthy
  if (allSuccess && waterSensorHealthy) {
    if (!Database.set<String>(aClient, "/devices/water_level_001/last_seen", String(timestamp))) {
      allSuccess = false;
    }
  }
  // Always update online status (even when offline)
  if (allSuccess) {
    if (!Database.set<bool>(aClient, "/devices/water_level_001/online", waterSensorHealthy)) {
      allSuccess = false;
    }
  }
  
  // 3. Update heartbeat for smart_controls - only if Arduino Mega I2C is healthy
  if (allSuccess && megaI2CHealthy) {
    if (!Database.set<String>(aClient, "/smart_controls/last_seen", String(timestamp))) {
      allSuccess = false;
    }
  }
  // Always update online status (even when offline)
  if (allSuccess) {
    if (!Database.set<bool>(aClient, "/smart_controls/online", megaI2CHealthy)) {
      allSuccess = false;
    }
  }
  
  // 4. Update heartbeat for door control - only if Arduino Mega I2C is healthy
  if (allSuccess && megaI2CHealthy) {
    if (!Database.set<String>(aClient, "/smart_controls/relays/door/last_seen", String(timestamp))) {
      allSuccess = false;
    }
  }
  // Always update online status (even when offline)
  if (allSuccess) {
    if (!Database.set<bool>(aClient, "/smart_controls/relays/door/online", megaI2CHealthy)) {
      allSuccess = false;
    }
  }
  
  // Check for errors
  if (!allSuccess) {
    // Firebase write failed - mark as disconnected
    firebaseConnected = false;
    
    // Log error occasionally
    if (heartbeatCounter % HEARTBEAT_LOG_FREQUENCY == 0) {
      Serial.println("❌ Heartbeat failed: Firebase write error");
      Serial.printf("   Error code: %d\n", aClient.lastError().code());
    }
  } else {
    // Success - log occasionally to minimize serial output
    if (heartbeatCounter % HEARTBEAT_LOG_FREQUENCY == 0) {
      Serial.printf("💓 Heartbeat sent: %s (4 systems: fingerprint, water, controls, door)\n", timestamp);
    }
  }
  
  // Increment counter
  heartbeatCounter++;
}

// Water level monitoring
void checkWaterLevel() {
  if (millis() - lastFloatRead < FLOAT_READ_INTERVAL) return;
  lastFloatRead = millis();
  
  bool state = (digitalRead(FLOAT_PIN) == LOW); // LOW = water present (closed switch)
  
  // Only act when state changes
  if (state != lastFloatState) {
    lastFloatState = state;
    
    if (state) {
      // Water is PRESENT - cancel any countdown and update immediately
      waterEmptyCountdownActive = false;
      waterEmptyCommandSent = false;
      waterEmptyDatabaseUpdated = false;
      
      sendI2CMessage("waterpresent");
      Serial.println("💧 WATER PRESENT");
      
      // Update Firebase immediately
      if (app.ready() && firebaseConnected) {
        Database.set<bool>(aClient, "/devices/water_level_001/water_level", true);
        Database.set<String>(aClient, "/devices/water_level_001/status", "water_present");
        Database.set<String>(aClient, "/devices/water_level_001/tank_status", "normal");
        Serial.printf("💾 Water level updated in Firebase: water_present\n");
      }
    } else {
      // Water is EMPTY - start 30-second countdown WITHOUT updating database yet
      Serial.println("🚨 WATER EMPTY DETECTED - Starting 30 second countdown...");
      Serial.println("📊 Database will update in 20 seconds, pump activates in 30 seconds");
      waterEmptyCountdownActive = true;
      waterEmptyDetectedTime = millis();
      waterEmptyCommandSent = false;
      waterEmptyDatabaseUpdated = false;
      lastCountdownLog = millis();
      
      // DO NOT update Firebase yet - let it stay as water_present
    }
  }
  
  // Handle water empty countdown (non-blocking)
  if (waterEmptyCountdownActive) {
    unsigned long elapsed = millis() - waterEmptyDetectedTime;
    unsigned long remaining = WATER_EMPTY_DELAY - elapsed;
    
    // Show countdown every 5 seconds
    if (millis() - lastCountdownLog >= 5000) {
      if (remaining > 0) {
        Serial.printf("⏱️  Water pump activation in %d seconds...\n", remaining / 1000);
      }
      lastCountdownLog = millis();
    }
    
    // Update Firebase at 20 seconds (10 seconds before pump activation)
    if (!waterEmptyDatabaseUpdated && elapsed >= (WATER_EMPTY_DELAY - WATER_DATABASE_UPDATE_ADVANCE)) {
      Serial.println("📊 20 SECONDS ELAPSED - Updating Firebase (pump activates in 10 seconds)");
      
      if (app.ready() && firebaseConnected) {
        Database.set<bool>(aClient, "/devices/water_level_001/water_level", false);
        Database.set<String>(aClient, "/devices/water_level_001/status", "water_empty");
        Database.set<String>(aClient, "/devices/water_level_001/tank_status", "alert");
        Serial.printf("💾 Water level updated in Firebase: water_empty\n");
      }
      
      waterEmptyDatabaseUpdated = true;
    }
    
    // Activate pump at 30 seconds
    if (!waterEmptyCommandSent && elapsed >= WATER_EMPTY_DELAY) {
      Serial.println("💦 30 SECONDS ELAPSED - Activating water pump");
      sendI2CMessage("waterempty");
      waterEmptyCommandSent = true;
      waterEmptyCountdownActive = false;
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

// Initialize all sensors and relays from database on boot
void initializeFromDatabase() {
  Serial.println("🔄 Initializing system state from database...");
  
  // Wait for Firebase to be ready
  int attempts = 0;
  while (!app.ready() && attempts < 30) {
    app.loop();
    delay(100);
    attempts++;
  }
  
  if (!app.ready()) {
    Serial.println("⚠️ Firebase not ready - skipping database initialization");
    return;
  }
  
  firebaseConnected = true;
  Serial.println("✅ Firebase connected");
  
  // Give Mega slave extra time to boot and be ready for I2C
  Serial.println("⏳ Waiting for Mega slave to be ready...");
  delay(2000);
  
  // 1. Initialize all relays from database
  Serial.println("🔌 Initializing relays...");
  for (int id = 1; id <= MAX_RELAY_ID; id++) {
    char path[50];
    snprintf(path, sizeof(path), "/smart_controls/relays/%d/state", id);
    
    bool state = Database.get<bool>(aClient, path);
    if (aClient.lastError().code() == 0) {
      char msg[8];
      snprintf(msg, sizeof(msg), "%d:%d", id, state ? 1 : 0);
      
      // Retry I2C send up to 3 times
      bool success = false;
      for (int retry = 0; retry < 3; retry++) {
        if (sendI2CMessage(msg)) {
          success = true;
          relayStateLast[id] = state;
          Serial.printf("  Relay %d: %s ✓\n", id, state ? "ON" : "OFF");
          break;
        }
        delay(5000);
      }
      
      if (!success) {
        Serial.printf("  Relay %d: FAILED ✗\n", id);
      }
      
      delay(5000); // Delay between relay commands
    }
  }
  relaysInitialized = true;
  
  // 2. Initialize door lock state
  Serial.println("🚪 Initializing door lock...");
  bool doorLocked = Database.get<bool>(aClient, "/smart_controls/relays/door/isLocked");
  if (aClient.lastError().code() == 0) {
    isDoorLocked = doorLocked;
    doorLockStateLast = doorLocked;
    
    // Retry I2C send up to 3 times
    bool success = false;
    for (int retry = 0; retry < 3; retry++) {
      if (sendI2CMessage(doorLocked ? "lock" : "unlock")) {
        success = true;
        Serial.printf("  Door: %s ✓\n", doorLocked ? "LOCKED" : "UNLOCKED");
        break;
      }
      delay(100);
    }
    
    if (!success) {
      Serial.println("  Door: FAILED ✗");
    }
  }
  
  // 3. Initialize failed attempts counter
  Serial.println("🔒 Initializing security state...");
  int failedAttempts = Database.get<int>(aClient, "/devices/fingerprint_door_001/failed_attempts");
  if (aClient.lastError().code() == 0) {
    currentFailedAttempts = failedAttempts;
    systemLocked = (currentFailedAttempts >= MAX_FAILED_ATTEMPTS);
    Serial.printf("  Failed attempts: %d/%d\n", currentFailedAttempts, MAX_FAILED_ATTEMPTS);
    Serial.printf("  System: %s\n", systemLocked ? "LOCKED" : "UNLOCKED");
  }
  
  // 4. Initialize water level state
  Serial.println("💧 Initializing water level...");
  bool waterPresent = Database.get<bool>(aClient, "/devices/water_level_001/water_level");
  if (aClient.lastError().code() == 0) {
    // Update local state and send to mega
    lastFloatState = waterPresent;
    
    // Retry I2C send up to 3 times
    bool success = false;
    for (int retry = 0; retry < 3; retry++) {
      if (sendI2CMessage(waterPresent ? "waterpresent" : "waterempty")) {
        success = true;
        Serial.printf("  Water: %s ✓\n", waterPresent ? "PRESENT" : "EMPTY");
        break;
      }
      delay(100);
    }
    
    if (!success) {
      Serial.println("  Water: FAILED ✗");
    }
  }
  
  Serial.println("✅ System state initialization complete");
}

void setup() {
  Serial.begin(57600);
  Serial.println("\n\n" + String('=').substring(0, 50));
  Serial.println("ESP8266 Fingerprint Access Control");
  Serial.println(String('=').substring(0, 50));
  Serial.printf("💾 Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.println();

  // Initialize I2C
  Wire.begin(D2, D1);
  Serial.println("✅ I2C initialized");
  
  // Initialize water level sensor pin
  pinMode(FLOAT_PIN, INPUT_PULLUP);
  lastFloatState = (digitalRead(FLOAT_PIN) == LOW);
  Serial.printf("💧 Water sensor initial: %s\n", lastFloatState ? "PRESENT" : "EMPTY");
  
  // Initialize buzzer pin (active low - HIGH = off)
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);
  Serial.println("🔊 Buzzer initialized");
  
  // Initialize fingerprint sensor and check health
  fingerprintSensorHealthy = finger.verifyPassword();
  if (fingerprintSensorHealthy) {
    Serial.println("👆 Fingerprint sensor OK");
  } else {
    Serial.println("❌ Fingerprint sensor FAILED");
  }
  
  preferences.begin("fingerprints", false);
  Serial.println("💾 Preferences initialized");
  
  // Initialize water sensor health (assume healthy on boot if pin is readable)
  waterSensorHealthy = true;
  
  // Initialize I2C health tracking
  lastSuccessfulI2C = millis(); // Assume healthy on boot
  megaI2CHealthy = true;
  
  // === CRITICAL: Sequential setup ===
  // 1. Connect to WiFi
  setupWiFi();
  
  if (!wifiConnected) {
    Serial.println("\n❌ SETUP FAILED - Cannot continue without WiFi");
    Serial.println("System will retry in 10 seconds...");
    delay(10000);
    ESP.restart();
    return;
  }
  
  // 2. Setup NTP time synchronization
  setupNTP();
  
  // 3. Setup DNS (automatic via WiFi)
  setupDNS();
  
  // 4. Setup Firebase
  Serial.println("\n=== INITIALIZING FIREBASE ===");
  setupFirebase();
  Serial.println("✅ Firebase initialized");
  Serial.println("=== FIREBASE READY ===");
  
  // Mark system as ready for main logic
  systemReady = true;
  
  Serial.println("\n" + String('=').substring(0, 50));
  Serial.println("=== SYSTEM STATUS ===");
  Serial.printf("WiFi:       %s\n", wifiConnected ? "✅" : "❌");
  Serial.printf("NTP:        ✅\n");
  Serial.printf("DNS:        ✅\n");
  Serial.printf("Firebase:   Initializing...\n");
  Serial.printf("System:     %s\n", systemReady ? "✅ READY" : "❌ NOT READY");
  Serial.println(String('=').substring(0, 50));
  
  // Initialize all sensors and relays from database after all systems are ready
  if (systemReady) {
    Serial.println("\n=== STARTING MAIN LOGIC ===");
    initializeFromDatabase();
  } else {
    Serial.println("\n⚠️ System not ready - skipping database initialization");
  }
  
  Serial.println("\n✅ SETUP COMPLETE - System operational");
  Serial.println(String('=').substring(0, 50) + "\n");
}

void loop() {
  ESP.wdtFeed();
  
  // Only proceed with main logic if all systems are ready
  if (!systemReady) {
    delay(100);
    return;
  }
  
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
    
    // Show connection lost warning periodically
    static unsigned long lastWarning = 0;
    if (millis() - lastWarning > 30000) { // Every 30 seconds
      Serial.println("⚠️ WARNING: WiFi connection lost");
      lastWarning = millis();
    }
  }
  
  // Component health monitoring (check before heartbeat)
  checkFingerprintSensorHealth();
  checkWaterSensorHealth();
  checkMegaI2CHealth();
  
  // Real-time relay monitoring
  fetchRelays();
  fetchDoorLock();
  checkFailedAttempts(); // Check for remote reset of failed attempts
  checkAutoLock(); // Check auto-lock timer
  
  // Water level monitoring
  checkWaterLevel();
  
  // Send heartbeat for health monitoring (uses actual component health status)
  sendHeartbeat();
  
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