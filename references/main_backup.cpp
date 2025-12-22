/***************************************************
  This is an example sketch for our optical Fingerprint sensor

  Designed specifically to work with the Adafruit BMP085 Breakout
  ----> http://www.adafruit.com/products/751

  These displays use TTL Serial to communicate, 2 pins are required to
  interface
  Adafruit invests time and resources providing this open source code,
  please support Adafruit and open-source hardware by purchasing
  products from Adafruit!

  Written by Limor Fried/Ladyada for Adafruit Industries.
  BSD license, all text above must be included in any redistribution

  ESP32 R307 Fingerprint Sensor Wiring:
  - Red wire (VCC) -> ESP32 3.3V or 5V (check sensor voltage requirements)
  - Green wire (GND) -> ESP32 GND
  - Yellow wire (TX from sensor) -> ESP32 GPIO16 (RX2)
  - Black wire (RX to sensor) -> ESP32 GPIO17 (TX2)

  BSD license, all text above must be included in any redistribution
 ****************************************************/
#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <sys/time.h>

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include "secrets.h"

// ESP32 UART2 for fingerprint sensor
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// Preferences for storing names
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
uint8_t id;
bool taskComplete = false;  // Track Firebase initialization completion

// Function declarations
uint8_t getFingerprintID(void);
String getFingerprintName(uint8_t id);

// Firebase and NTP functions
void setupWiFi();
void setupFirebase();
void testFirebaseWrite();
void setupNTP();
String getCurrentDateTime();
void logFingerprintAccess(String status, String user, int attempts = 0);
void processFirebaseData(AsyncResult &aResult);
void set_ssl_client_insecure_and_buffer(SSL_CLIENT &client);

void setup()
{
  Serial.begin(115200);
  while (!Serial);
  delay(100);
  Serial.println("\n=== ESP32 Fingerprint Access Control System ===");
  Serial.println("Initializing fingerprint sensor...");

  // Initialize hardware serial for fingerprint sensor
  mySerial.begin(57600, SERIAL_8N1, 16, 17);
  
  if (finger.verifyPassword()) {
    Serial.println("✓ Fingerprint sensor connected");
  } else {
    Serial.println("❌ Fingerprint sensor not found - SYSTEM HALTED");
    while (1) { delay(1000); }
  }

  // Initialize preferences for name storage
  preferences.begin("fingerprints", false);
  
  // Setup network and services
  setupWiFi();
  setupNTP();
  setupFirebase();
  
  Serial.println("✓ System ready - monitoring fingerprint access");
}

uint8_t readnumber(void) {
  uint8_t num = 0;

  while (num == 0) {
    while (! Serial.available());
    num = Serial.parseInt();
  }
  return num;
}

void showMenu() {
  Serial.println("\n=== ESP32 FINGERPRINT SYSTEM MENU ===");
  Serial.println("Available commands:");
  Serial.println("• 'enroll' or 'e' - Enroll a new fingerprint");
  Serial.println("• 'names' or 'n' - Set name for fingerprint ID");
  Serial.println("• 'show' or 's' - Show all stored names");
  Serial.println("• 'list' or 'l' - List all stored templates");
  Serial.println("• 'delete' or 'd' - Delete a fingerprint");
  Serial.println("• 'security' or 'sec' - Test security (3 attempts)");
  Serial.println("• 'template' or 't' - Get template for ID");
  Serial.println("• 'menu' or 'm' - Show this menu");
  Serial.println("=====================================");
  Serial.println("Place finger on sensor for authentication");
  Serial.println("Or type a command and press Enter\n");
}

void setFingerprintName() {
  Serial.println("\n=== SET FINGERPRINT NAME ===");
  Serial.println("Enter ID number (1-127) to assign name:");
  uint8_t id = readnumber();
  
  if (id < 1 || id > 127) {
    Serial.println("❌ Invalid ID! Use 1-127");
    return;
  }
  
  Serial.println("Enter name for this fingerprint (press Enter when done):");
  while (!Serial.available()) { delay(10); }
  String name = Serial.readStringUntil('\n');
  name.trim();
  
  if (name.length() > 0) {
    saveFingerprintName(id, name);
    Serial.println("✓ Name '" + name + "' assigned to ID #" + String(id));
  } else {
    Serial.println("❌ No name entered!");
  }
}

void showStoredNames() {
  Serial.println("\n=== STORED FINGERPRINT NAMES ===");
  String storedNames = getAllStoredNames();
  if (storedNames.length() > 0) {
    Serial.println(storedNames);
  } else {
    Serial.println("No names stored yet.");
  }
  Serial.println("================================");
}

void deleteFingerprint() {
  Serial.println("\n=== DELETE FINGERPRINT ===");
  Serial.println("Enter ID number (1-127) to delete:");
  uint8_t id = readnumber();
  
  if (id < 1 || id > 127) {
    Serial.println("❌ Invalid ID! Use 1-127");
    return;
  }
  
  uint8_t p = finger.deleteModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("✓ Fingerprint deleted successfully!");
    // Also delete the name if it exists
    deleteFingerprintName(id);
    Serial.println("✓ Associated name also deleted.");
  } else {
    Serial.println("❌ Failed to delete fingerprint!");
  }
}

void loop()
{
  // Maintain Firebase authentication and async tasks
  app.loop();
  
  // Process Firebase async results
  processFirebaseData(databaseResult);
  
  // Test Firebase connection once when app is ready
  if (app.ready() && !taskComplete) {
    taskComplete = true;
    testFirebaseWrite();
  }
  
  // Check for fingerprint continuously
  getFingerprintID();
  
  // Small delay to prevent overwhelming the sensor
  delay(50);
}

uint8_t getFingerprintEnroll() {
  int p = -1;
  Serial.print("Waiting for valid finger to enroll as #"); Serial.println(id);
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    switch (p) {
    case FINGERPRINT_OK:
      Serial.println("Image taken");
      break;
    case FINGERPRINT_NOFINGER:
      Serial.print(".");
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("Communication error");
      break;
    case FINGERPRINT_IMAGEFAIL:
      Serial.println("Imaging error");
      break;
    default:
      Serial.println("Unknown error");
      break;
    }
  }

  // OK success!

  p = finger.image2Tz(1);
  switch (p) {
    case FINGERPRINT_OK:
      Serial.println("Image converted");
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

  Serial.println("Remove finger");
  delay(2000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
  }
  Serial.print("ID "); Serial.println(id);
  p = -1;
  Serial.println("Place same finger again");
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    switch (p) {
    case FINGERPRINT_OK:
      Serial.println("Image taken");
      break;
    case FINGERPRINT_NOFINGER:
      Serial.print(".");
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("Communication error");
      break;
    case FINGERPRINT_IMAGEFAIL:
      Serial.println("Imaging error");
      break;
    default:
      Serial.println("Unknown error");
      break;
    }
  }

  // OK success!

  p = finger.image2Tz(2);
  switch (p) {
    case FINGERPRINT_OK:
      Serial.println("Image converted");
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
  Serial.print("Creating model for #");  Serial.println(id);

  p = finger.createModel();
  if (p == FINGERPRINT_OK) {
    Serial.println("Prints matched!");
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("Communication error");
    return p;
  } else if (p == FINGERPRINT_ENROLLMISMATCH) {
    Serial.println("Fingerprints did not match");
    return p;
  } else {
    Serial.println("Unknown error");
    return p;
  }

  Serial.print("ID "); Serial.println(id);
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("✓ Stored!");
    Serial.println("✓ Fingerprint enrollment complete!");
    Serial.println("You can now assign a name using 'names' command.");
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("Communication error");
    return p;
  } else if (p == FINGERPRINT_BADLOCATION) {
    Serial.println("Could not store in that location");
    return p;
  } else if (p == FINGERPRINT_FLASHERR) {
    Serial.println("Error writing to flash");
    return p;
  } else {
    Serial.println("Unknown error");
    return p;
  }

  return true;
}

void getTemplate() {
  Serial.println("\n=== GET TEMPLATE ===");
  Serial.println("Enter ID number (1-127) to get template:");
  uint8_t id = readnumber();
  
  if (id < 1 || id > 127) {
    Serial.println("❌ Invalid ID! Use 1-127");
    return;
  }
  
  Serial.print("Attempting to load #"); Serial.println(id);
  uint8_t p = downloadFingerprintTemplate(id);
  switch (p) {
    case FINGERPRINT_OK:
      Serial.println("✓ Template downloaded successfully!");
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("❌ Communication error");
      break;
    case FINGERPRINT_DBRANGEFAIL:
      Serial.println("❌ Invalid ID");
      break;
    default:
      Serial.println("❌ No template at this ID or unknown error");
      break;
  }
}

void listAllTemplates() {
  Serial.println("\n=== LIST ALL TEMPLATES ===");
  Serial.println("Scanning all template slots (1-127)...");
  Serial.println("This may take a moment...\n");
  
  uint16_t totalTemplates = 0;
  for (uint16_t id = 1; id <= 127; id++) {
    uint8_t p = finger.loadModel(id);
    if (p == FINGERPRINT_OK) {
      String personName = getFingerprintName(id);
      Serial.println("Found template at ID #" + String(id) + " (" + personName + ")");
      totalTemplates++;
    }
    delay(10);  // Small delay to prevent watchdog issues
  }
  
  Serial.println("\nScan complete!");
  Serial.println("Total templates found: " + String(totalTemplates) + "/" + String(finger.capacity));
  
  if (totalTemplates == 0) {
    Serial.println("\nNo fingerprints enrolled yet.");
    Serial.println("Use 'enroll' command to add some!");
  }
}

void securityBiometrics() {
  Serial.println("\n=== SECURITY TEST ===");
  Serial.println("3-attempt authentication started...\n");
  
  uint8_t attempts = 0;
  bool accessGranted = false;
  
  while (attempts < 3 && !accessGranted) {
    attempts++;
    Serial.println("Attempt " + String(attempts) + "/3: Place your finger on the sensor...");
    
    // Wait for finger detection
    uint8_t p = FINGERPRINT_NOFINGER;
    while (p != FINGERPRINT_OK) {
      p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        break;
      } else if (p == FINGERPRINT_NOFINGER) {
        // Just keep waiting
      } else {
        Serial.println("Error getting image, try again");
        break;
      }
      delay(50);
    }
    
    if (p == FINGERPRINT_OK) {
      p = finger.image2Tz();
      if (p == FINGERPRINT_OK) {
        p = finger.fingerFastSearch();
        if (p == FINGERPRINT_OK) {
          String personName = getFingerprintName(finger.fingerID);
          Serial.println("✅ ACCESS GRANTED!");
          Serial.println("Welcome " + personName + " (ID #" + String(finger.fingerID) + ")");
          Serial.println("Confidence: " + String(finger.confidence));
          accessGranted = true;
          
          // Log successful access to Firebase
          logFingerprintAccess("success", personName, attempts);
        } else {
          Serial.println("❌ Fingerprint not recognized.");
          
          // Log failed attempt to Firebase
          logFingerprintAccess("failed", "unknown", attempts);
        }
      } else {
        Serial.println("❌ Image processing failed.");
      }
    }
    
    if (!accessGranted && attempts < 3) {
      Serial.println("Try again...\n");
      delay(1000);
    }
  }
  
  if (!accessGranted) {
    Serial.println("\n🚫 ACCESS DENIED!");
    Serial.println("Maximum attempts exceeded.");
    Serial.println("Security lockout activated.");
  }
  
  Serial.println("\nReturning to main menu...");
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
    Serial.print("Found ID #"); Serial.print(finger.fingerID);
    Serial.print(" with confidence of "); Serial.println(finger.confidence);
    
    // Log successful access to Firebase
    logFingerprintAccess("success", personName);
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("Communication error");
    return p;
  } else if (p == FINGERPRINT_NOTFOUND) {
    Serial.println("❌ ACCESS DENIED - Fingerprint not recognized");
    Serial.println("Unknown finger detected");
    
    // Log failed access to Firebase
    logFingerprintAccess("failed", "unknown");
    return p;
  } else {
    Serial.println("Unknown error");
    return p;
  }

  return FINGERPRINT_OK;
}

uint8_t downloadFingerprintTemplate(uint16_t id)
{
  Serial.println("Attempting to load #" + String(id));
  uint8_t p = finger.loadModel(id);
  switch (p) {
    case FINGERPRINT_OK:
      Serial.print("Template "); Serial.print(id); Serial.println(" loaded");
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("Communication error");
      return p;
    case FINGERPRINT_DBRANGEFAIL:
      Serial.println("Invalid ID");
      return p;
    default:
      Serial.println("ID not in use or unknown error");
      return p;
  }

  // OK success!

  Serial.print("Attempting to get #"); Serial.println(id);
  p = finger.getModel();
  switch (p) {
    case FINGERPRINT_OK:
      Serial.print("Template "); Serial.print(id); Serial.println(" transferring:");
      break;
    default:
      Serial.print("Unknown error "); Serial.println(p);
      return p;
  }

  // one data packet is 267 bytes. in one data packet, 11 bytes are 'useless' :D
  uint8_t bytesReceived[534]; // 2 data packets
  memset(bytesReceived, 0xff, 534);

  uint32_t starttime = millis();
  int i = 0;
  while (i < 534 && (millis() - starttime) < 20000) {
    if (mySerial.available()) {
      bytesReceived[i++] = mySerial.read();
    }
  }
  Serial.print(i); Serial.println(" bytes read.");
  Serial.println("Decoding packet...");

  uint8_t fingerTemplate[512]; // the real template
  memset(fingerTemplate, 0xff, 512);

  // filtering only the useful bytes
  int uindx = 0, index = 0;
  memcpy(fingerTemplate + uindx, bytesReceived + index, 256);   // first 256 bytes
  uindx += 256;
  index += 267;    // skip to second packet payload
  memcpy(fingerTemplate + uindx, bytesReceived + index, 256);   // second 256 bytes

  for (int i = 0; i < 512; ++i) {
    if (i % 16 == 0) Serial.println();
    Serial.print("0x");
    Serial.print(fingerTemplate[i], HEX);
    Serial.print(", ");
  }
  Serial.println("\n\nTemplate displayed above.");

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

String getAllStoredNames() {
  String result = "";
  int nameCount = 0;
  
  preferences.begin("fingerprints", true);
  
  // Check all possible IDs (1-127) for stored names
  for (int id = 1; id <= 127; id++) {
    String key = "name_" + String(id);
    String name = preferences.getString(key.c_str(), "");
    
    if (name.length() > 0 && name != "Unknown") {
      if (nameCount > 0) {
        result += "\n";
      }
      result += "ID #" + String(id) + ": " + name;
      nameCount++;
    }
  }
  
  preferences.end();
  
  if (nameCount == 0) {
    result = "No names found in memory.\nUse 'Set Names' to assign names to enrolled fingerprints.";
  } else {
    result = "Found " + String(nameCount) + " stored name(s):\n\n" + result;
  }
  
  return result;
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
  if (client.connect("8.8.8.8", 53)) {
    client.stop();
    return true;
  }
  return false;
}

// Setup WiFi connection
void setupWiFi() {
  Serial.println("=== WiFi Setup ===");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    Serial.print(".");
    delay(500);
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("✓ Connected with IP: ");
    Serial.println(WiFi.localIP());
    
    // Test internet connectivity
    Serial.println("Testing internet connectivity...");
    if (testInternetConnectivity()) {
      Serial.println("✓ Internet connection verified");
    } else {
      Serial.println("❌ No internet access - SYSTEM HALTED");
      Serial.println("Please check your internet connection");
      Serial.println("The device cannot function without internet access for Firebase logging");
      while (true) {
        delay(1000);
      }
    }
  } else {
    Serial.println();
    Serial.println("❌ WiFi connection failed - SYSTEM HALTED");
    Serial.println("Check your WiFi credentials in secrets.h");
    Serial.println("SSID: " + String(WIFI_SSID));
    Serial.println("The device cannot function without WiFi connection");
    while (true) {
      delay(1000);
    }
  }
}

// Setup NTP for time synchronization
void setupNTP() {
  Serial.println("=== NTP Setup ===");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Wait for time to be set
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 15) {
    Serial.print(".");
    delay(1000);
    attempts++;
  }
  
  if (getLocalTime(&timeinfo)) {
    Serial.println();
    Serial.println("✓ NTP time synchronized");
    Serial.print("Current time: ");
    Serial.println(getCurrentDateTime());
  } else {
    Serial.println();
    Serial.println("❌ Failed to obtain time from NTP server - SYSTEM HALTED");
    Serial.println("Time synchronization is required for Firebase logging");
    Serial.println("Please check your internet connection and try again");
    while (true) {
      delay(1000);
    }
  }
}

// Setup Firebase
void setupFirebase() {
  Serial.println("=== Firebase Setup ===");
  Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
  
  // Configure SSL
  set_ssl_client_insecure_and_buffer(ssl_client);
  Serial.println("Configuring Firebase authentication...");
  
  // Initialize Firebase app using the UserAuth object directly
  Firebase.initializeApp(aClient, app, getAuth(user_auth));
  
  // Get database instance
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
  
  Serial.println("✓ Firebase configuration complete");
  Serial.println("✓ System ready for fingerprint access logging");
}

// Test Firebase connection with simple write (from reference)
void testFirebaseWrite() {
  Serial.println("=== TESTING FIREBASE CONNECTION ===");
  Serial.println("⚠️  IMPORTANT: Make sure your Firebase Database Rules allow writes!");
  Serial.println("⚠️  Rules should be: { \"rules\": { \".read\": true, \".write\": true } }");
  Serial.println();
  
  // Try a simple test write first
  String testPath = "/test/fingerprint_connection";
  String testValue = "Hello from ESP32 Fingerprint System";
  
  Serial.printf("Testing write to: %s\n", testPath.c_str());
  bool status = Database.set<String>(aClient, testPath, testValue);
  
  if (status) {
    Serial.println("✅ Test write successful!");
    Serial.println("✅ Firebase connection is working properly");
  } else {
    Serial.printf("❌ Test write failed: %s (code: %d)\n", 
                  aClient.lastError().message().c_str(), 
                  aClient.lastError().code());
    Serial.println("❌ Common causes:");
    Serial.println("   1. Firebase Database Rules don't allow writes");
    Serial.println("   2. Wrong Database URL");
    Serial.println("   3. Network connectivity issues");
    Serial.println("   4. Authentication not yet complete");
  }
  Serial.println("=== TEST COMPLETE ===\n");
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

// Log fingerprint access attempts to Firebase
void logFingerprintAccess(String status, String user, int attempts) {
  // Check if Firebase app is ready first
  if (!app.ready()) {
    Serial.println("❌ Firebase not ready, cannot log access");
    Serial.println("   App authenticated: " + String(app.isAuthenticated() ? "YES" : "NO"));
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
  } else {
    Serial.printf("❌ Failed to save access log\n");
    Serial.printf("❌ Error message: %s\n", aClient.lastError().message().c_str());
    Serial.printf("❌ Error code: %d\n", aClient.lastError().code());
    
    // Additional debugging
    if (aClient.lastError().code() == 401) {
      Serial.println("❌ Authentication error - check your credentials");
    } else if (aClient.lastError().code() == 403) {
      Serial.println("❌ Permission denied - check your Firebase database rules");
    } else if (aClient.lastError().code() == 404) {
      Serial.println("❌ Database not found - check your DATABASE_URL");
    }
  }
  Serial.printf("=== FIREBASE OPERATION END ===\n\n");
  
  // Update last_updated timestamp
  String lastUpdatedPath = "/devices/fingerprint_door_001/last_updated";
  Database.set<String>(aClient, lastUpdatedPath, currentDateTime);
  
  // Update failed attempts counter if it's a failed attempt
  if (status == "failed") {
    String failedAttemptsPath = "/devices/fingerprint_door_001/failed_attempts";
    
    // Get current failed attempts count (simplified approach)
    static int failedCount = 0;
    failedCount++;
    
    Database.set<int>(aClient, failedAttemptsPath, failedCount);
    
    // Reset counter on successful access
  } else if (status == "success") {
    String failedAttemptsPath = "/devices/fingerprint_door_001/failed_attempts";
    Database.set<int>(aClient, failedAttemptsPath, 0);
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

  if (aResult.isDebug()) {
    Firebase.printf("Debug task: %s, msg: %s\n", 
                   aResult.uid().c_str(), 
                   aResult.debug().c_str());
  }

  if (aResult.isError()) {
    Firebase.printf("Error task: %s, msg: %s, code: %d\n", 
                   aResult.uid().c_str(), 
                   aResult.error().message().c_str(), 
                   aResult.error().code());
  }

  if (aResult.available()) {
    Firebase.printf("Task: %s, payload: %s\n", 
                   aResult.uid().c_str(), 
                   aResult.c_str());
  }
}
