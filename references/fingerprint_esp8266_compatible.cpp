/***************************************************
  ESP8266/NodeMCU R307 Fingerprint Sensor - Simplified Production Version
  Continuous fingerprint reading with Firebase logging (no menu)
  
  ESP8266/NodeMCU R307 Fingerprint Sensor Wiring:
  - Red wire (VCC) -> NodeMCU 3.3V or 5V
  - Green wire (GND) -> NodeMCU GND
  - Yellow wire (TX from sensor) -> NodeMCU D7 (GPIO13)
  - Black wire (RX to sensor) -> NodeMCU D8 (GPIO15)
 ****************************************************/
#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>
#include <Preferences.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include "secrets.h"

// ESP8266 SoftwareSerial for fingerprint sensor
SoftwareSerial fingerprintSerial(D7, D8); // RX=D7(GPIO13), TX=D8(GPIO15)
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerprintSerial);

// Use installed Preferences library (vshymanskyy/Preferences) for ESP8266
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

void setup() {
  // Initialize Serial at 115200 for Serial Monitor
  Serial.begin(115200);
  delay(50);
  Serial.println("=== ESP8266 Fingerprint Access Control ===");

  // Initialize SoftwareSerial for fingerprint sensor
  fingerprintSerial.begin(57600);
  delay(100);
  
  if (finger.verifyPassword()) {
    Serial.println("✓ Fingerprint sensor ready");
  } else {
    Serial.println("❌ Fingerprint sensor error");
    Serial.println("Check wiring: D7->TX(yellow), D8->RX(black), 3.3V->VCC(red), GND->GND(green)");
    while (1) { delay(100); }
  }

  // Initialize preferences for name storage
  preferences.begin("fingerprints", false);
  
  // Setup network and services
  setupWiFi();
  setupNTP();
  setupFirebase();
  
  Serial.println("✓ System ready");
}

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
  
  // Small delay to prevent overwhelming the sensor
  delay(25); // Reduced from 50ms to 25ms for faster response
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
      Serial.println("Could not find fingerprint features");
      return p;
    case FINGERPRINT_INVALIDIMAGE:
      Serial.println("Could not find fingerprint features");
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
  } else if (status == "success") {
    currentFailedAttempts = 0;
    systemLocked = false;
    Serial.println("✅ Access granted - Failed attempts counter reset");
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
