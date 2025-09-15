#include <Arduino.h>
#include <SoftwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <Preferences.h>

// SoftwareSerial for fingerprint sensor (D5=RX, D6=TX)
SoftwareSerial mySerial(14, 12); // D5, D6
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// Preferences for storing fingerprint names
Preferences preferences;

// Function declarations
void fingerprintManagementMenu();
void createFingerprintEntry();
void setNamesToFingerprints();
void showAllActiveFingerprints();
void deleteFingerprint();
int getNextAvailableID();
bool getFingerprintEnroll(int id);
int readNumberInput();
String readStringInput(unsigned long timeoutMs);

void setup() {
  // Wait for board to stabilize
  delay(1000);
  
  // Initialize serial communication at ESP8266 native baud rate
  Serial.begin(74880);
  Serial.println();
  Serial.println("=== NodeMCU Fingerprint Management System ===");
  Serial.println("Initializing fingerprint sensor...");
  
  // Initialize fingerprint sensor
  mySerial.begin(57600);
  
  if (finger.verifyPassword()) {
    Serial.println("✓ Fingerprint sensor found and ready!");
    Serial.print("Sensor contains "); Serial.print(finger.templateCount); Serial.println(" templates");
    
    // Initialize preferences
    preferences.begin("fingerprints", false);
    
    Serial.println();
    Serial.println("=== FINGERPRINT MANAGEMENT MENU ===");
    Serial.println("Press 'M' to open the management menu");
    Serial.println("====================================");
  } else {
    Serial.println("✗ Did not find fingerprint sensor :(");
    Serial.println("Check wiring and connections");
    while (1) { delay(1); }
  }
}

void loop() {
  // Check for menu activation
  if (Serial.available()) {
    String input = Serial.readString();
    input.trim();
    input.toUpperCase();
    
    if (input == "M") {
      fingerprintManagementMenu();
    }
  }
  
  delay(50);
}

void fingerprintManagementMenu() {
  while (true) {
    Serial.println();
    Serial.println("====================================");
    Serial.println("    FINGERPRINT MANAGEMENT MENU    ");
    Serial.println("====================================");
    Serial.println("1. Create Fingerprint Entry");
    Serial.println("2. Set Names To Fingerprints");  
    Serial.println("3. Show All Active Fingerprints");
    Serial.println("4. Delete Fingerprint");
    Serial.println("0. Exit Menu");
    Serial.println("====================================");
    Serial.print("Enter your choice (1-4, 0 to exit): ");
    
    // Wait for input with timeout
    unsigned long timeout = millis() + 30000; // 30 second timeout
    String choice = "";
    
    while (millis() < timeout) {
      if (Serial.available()) {
        choice = Serial.readString();
        choice.trim();
        break;
      }
      delay(100);
    }
    
    if (choice == "") {
      Serial.println("\nTimeout - Returning to main loop");
      return;
    }
    
    Serial.println(choice);
    
    if (choice == "1") {
      createFingerprintEntry();
    } else if (choice == "2") {
      setNamesToFingerprints();
    } else if (choice == "3") {
      showAllActiveFingerprints();
    } else if (choice == "4") {
      deleteFingerprint();
    } else if (choice == "0") {
      Serial.println("Exiting menu...");
      return;
    } else {
      Serial.println("Invalid choice. Please try again.");
      delay(1000);
    }
  }
}

void createFingerprintEntry() {
  Serial.println();
  Serial.println("=== CREATE FINGERPRINT ENTRY ===");
  Serial.println();
  
  Serial.println("Choose fingerprint ID option:");
  Serial.println("1. Auto-assign next available ID");
  Serial.println("2. Choose specific ID (1-162)");
  Serial.print("Enter choice (1 or 2): ");
  
  String choice = readStringInput(10000);
  choice.trim();
  
  int id = -1;
  
  if (choice == "1") {
    // Auto-assign next available ID
    id = getNextAvailableID();
    if (id == -1) {
      Serial.println("Error: Sensor is full (162 fingerprints maximum)");
      delay(2000);
      return;
    }
    Serial.print("Auto-assigned ID #");
    Serial.println(id);
  } else if (choice == "2") {
    // Let user choose specific ID
    Serial.print("Enter desired ID (1-162): ");
    id = readNumberInput();
    
    if (id < 1 || id > 162) {
      Serial.println("Error: ID must be between 1 and 162");
      delay(2000);
      return;
    }
    
    // Check if ID is already in use
    if (finger.loadModel(id) == FINGERPRINT_OK) {
      Serial.print("Warning: ID #");
      Serial.print(id);
      Serial.print(" is already in use. ");
      
      // Show existing name if available
      String key = "fp_" + String(id);
      String existingName = preferences.getString(key.c_str(), "Unnamed");
      Serial.print("Current name: ");
      Serial.println(existingName);
      
      Serial.print("Overwrite? (y/N): ");
      String confirm = readStringInput(10000);
      confirm.toLowerCase();
      
      if (confirm != "y" && confirm != "yes") {
        Serial.println("Enrollment cancelled");
        delay(2000);
        return;
      }
    }
    
    Serial.print("Using ID #");
    Serial.println(id);
  } else {
    Serial.println("Invalid choice. Returning to menu.");
    delay(2000);
    return;
  }
  
  Serial.println();
  Serial.println("📋 Fingerprint Enrollment Instructions:");
  Serial.println("• Make sure your finger is clean and dry");
  Serial.println("• Cover the entire sensor surface");
  Serial.println("• Press firmly but don't slide your finger");
  Serial.println("• You'll need to scan the same finger twice");
  Serial.println();
  Serial.println("⏳ Get ready... Starting in 3 seconds...");
  delay(1000);
  Serial.print("3... ");
  delay(1000);
  Serial.print("2... ");
  delay(1000);
  Serial.print("1... ");
  delay(1000);
  Serial.println("GO!");
  Serial.println();
  
  if (getFingerprintEnroll(id)) {
    Serial.println("✓ Fingerprint enrolled successfully!");
    
    // Ask for name
    Serial.print("Enter a name for this fingerprint: ");
    String name = readStringInput(30000); // 30 second timeout
    
    if (name.length() > 0) {
      String key = "fp_" + String(id);
      preferences.putString(key.c_str(), name);
      Serial.println("✓ Name saved successfully!");
    }
  } else {
    Serial.println("✗ Failed to enroll fingerprint");
  }
  
  delay(2000);
}

void setNamesToFingerprints() {
  Serial.println();
  Serial.println("=== SET NAMES TO FINGERPRINTS ===");
  
  // Show existing fingerprints
  showAllActiveFingerprints();
  
  Serial.print("Enter fingerprint ID to name: ");
  int id = readNumberInput();
  
  if (id < 1 || id > 162) {
    Serial.println("Invalid ID. Must be between 1-162");
    delay(2000);
    return;
  }
  
  // Check if fingerprint exists
  if (finger.loadModel(id) != FINGERPRINT_OK) {
    Serial.println("No fingerprint found at that ID");
    delay(2000);
    return;
  }
  
  Serial.print("Enter name for fingerprint ID ");
  Serial.print(id);
  Serial.print(": ");
  
  String name = readStringInput(30000);
  if (name.length() > 0) {
    String key = "fp_" + String(id);
    preferences.putString(key.c_str(), name);
    Serial.println("✓ Name updated successfully!");
  } else {
    Serial.println("No name entered");
  }
  
  delay(2000);
}

void showAllActiveFingerprints() {
  Serial.println();
  Serial.println("=== ACTIVE FINGERPRINTS ===");
  
  int count = 0;
  for (int i = 1; i <= 162; i++) {
    if (finger.loadModel(i) == FINGERPRINT_OK) {
      count++;
      String key = "fp_" + String(i);
      String name = preferences.getString(key.c_str(), "Unnamed");
      
      Serial.print("ID: ");
      if (i < 10) Serial.print("  ");
      else if (i < 100) Serial.print(" ");
      Serial.print(i);
      Serial.print(" | Name: ");
      Serial.println(name);
    }
  }
  
  if (count == 0) {
    Serial.println("No fingerprints found");
  } else {
    Serial.print("Total: ");
    Serial.print(count);
    Serial.println(" fingerprints");
  }
  
  Serial.println("========================");
  delay(3000);
}

void deleteFingerprint() {
  Serial.println();
  Serial.println("=== DELETE FINGERPRINT ===");
  
  showAllActiveFingerprints();
  
  Serial.print("Enter fingerprint ID to delete: ");
  int id = readNumberInput();
  
  if (id < 1 || id > 162) {
    Serial.println("Invalid ID. Must be between 1-162");
    delay(2000);
    return;
  }
  
  // Check if fingerprint exists
  if (finger.loadModel(id) != FINGERPRINT_OK) {
    Serial.println("No fingerprint found at that ID");
    delay(2000);
    return;
  }
  
  // Get name for confirmation
  String key = "fp_" + String(id);
  String name = preferences.getString(key.c_str(), "Unnamed");
  
  Serial.print("Delete fingerprint ID ");
  Serial.print(id);
  Serial.print(" (");
  Serial.print(name);
  Serial.print(")? (y/N): ");
  
  String confirm = readStringInput(10000);
  confirm.toLowerCase();
  
  if (confirm == "y" || confirm == "yes") {
    if (finger.deleteModel(id) == FINGERPRINT_OK) {
      preferences.remove(key.c_str());
      Serial.println("✓ Fingerprint deleted successfully!");
    } else {
      Serial.println("✗ Failed to delete fingerprint");
    }
  } else {
    Serial.println("Delete cancelled");
  }
  
  delay(2000);
}

int getNextAvailableID() {
  for (int i = 1; i <= 162; i++) {
    if (finger.loadModel(i) != FINGERPRINT_OK) {
      return i;
    }
  }
  return -1; // No available slots
}

bool getFingerprintEnroll(int id) {
  int p = -1;
  
  Serial.println("========================================");
  Serial.print("ENROLLING FINGERPRINT - ID #"); 
  Serial.println(id);
  Serial.println("========================================");
  Serial.println("🖐️  STEP 1: First Finger Scan");
  Serial.println("Place your finger firmly on the sensor");
  Serial.println("Make sure your finger covers the entire sensor");
  Serial.println("Take your time - waiting for finger...");
  Serial.println();
  
  // Wait for finger placement with extended timeout
  unsigned long startTime = millis();
  int dotCount = 0;
  
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    
    // Timeout after 60 seconds (doubled)
    if (millis() - startTime > 60000) {
      Serial.println();
      Serial.println("⏰ Timeout waiting for finger placement");
      Serial.println("Please try again when ready");
      return false;
    }
    
    switch (p) {
    case FINGERPRINT_OK:
      Serial.println();
      Serial.println("✓ First image captured successfully!");
      break;
    case FINGERPRINT_NOFINGER:
      // Show progress every 2 seconds instead of rapid dots
      if (millis() - startTime > dotCount * 2000) {
        Serial.print(".");
        dotCount++;
        if (dotCount % 15 == 0) { // New line every 30 seconds
          Serial.println(" (still waiting...)");
        }
      }
      delay(500); // Much slower polling - 500ms instead of 200ms
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println();
      Serial.println("✗ Communication error");
      return false;
    case FINGERPRINT_IMAGEFAIL:
      Serial.println();
      Serial.println("✗ Imaging error - please clean sensor and try again");
      delay(2000);
      break;
    default:
      Serial.println();
      Serial.println("✗ Unknown error");
      return false;
    }
  }

  // Convert first image with longer delay
  Serial.println("Processing first image...");
  delay(1000); // Longer processing delay
  p = finger.image2Tz(1);
  switch (p) {
    case FINGERPRINT_OK:
      Serial.println("✓ First image processed successfully");
      break;
    case FINGERPRINT_IMAGEMESS:
      Serial.println("✗ Image too messy - please clean finger and try again");
      return false;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("✗ Communication error");
      return false;
    case FINGERPRINT_FEATUREFAIL:
      Serial.println("✗ Could not extract fingerprint features");
      Serial.println("💡 Tips: Clean your finger, press more firmly, or try a different angle");
      return false;
    case FINGERPRINT_INVALIDIMAGE:
      Serial.println("✗ Invalid image - please place finger properly");
      return false;
    default:
      Serial.println("✗ Unknown error processing image");
      return false;
  }

  Serial.println();
  Serial.println("✓ First scan complete!");
  Serial.println("📤 Please REMOVE your finger from the sensor completely");
  Serial.println("Waiting for finger removal...");
  
  delay(3000); // Give more time to read instructions
  p = 0;
  
  // Wait for finger removal with visual feedback
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      break;
    }
    Serial.print(".");
    delay(500);
  }
  
  Serial.println();
  Serial.println("✓ Finger removed successfully");
  Serial.println();
  Serial.println("🖐️  STEP 2: Second Finger Scan");
  Serial.println("📥 Please place the SAME finger again");
  Serial.println("Press firmly and hold steady...");
  Serial.println("Take your time - waiting for finger...");
  Serial.println();
  
  delay(2000); // Give user time to read instructions
  p = -1;
  startTime = millis();
  dotCount = 0;
  
  // Wait for second finger placement
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    
    // Timeout after 60 seconds
    if (millis() - startTime > 60000) {
      Serial.println();
      Serial.println("⏰ Timeout waiting for second finger placement");
      return false;
    }
    
    switch (p) {
    case FINGERPRINT_OK:
      Serial.println();
      Serial.println("✓ Second image captured successfully!");
      break;
    case FINGERPRINT_NOFINGER:
      // Show progress every 2 seconds
      if (millis() - startTime > dotCount * 2000) {
        Serial.print(".");
        dotCount++;
        if (dotCount % 15 == 0) {
          Serial.println(" (still waiting...)");
        }
      }
      delay(500);
      break;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println();
      Serial.println("✗ Communication error");
      return false;
    case FINGERPRINT_IMAGEFAIL:
      Serial.println();
      Serial.println("✗ Imaging error - please try again");
      delay(2000);
      break;
    default:
      Serial.println();
      Serial.println("✗ Unknown error");
      return false;
    }
  }

  // Convert second image
  Serial.println("Processing second image...");
  delay(1000);
  p = finger.image2Tz(2);
  switch (p) {
    case FINGERPRINT_OK:
      Serial.println("✓ Second image processed successfully");
      break;
    case FINGERPRINT_IMAGEMESS:
      Serial.println("✗ Second image too messy - please try again");
      return false;
    case FINGERPRINT_PACKETRECIEVEERR:
      Serial.println("✗ Communication error");
      return false;
    case FINGERPRINT_FEATUREFAIL:
      Serial.println("✗ Could not extract features from second image");
      return false;
    case FINGERPRINT_INVALIDIMAGE:
      Serial.println("✗ Invalid second image");
      return false;
    default:
      Serial.println("✗ Unknown error processing second image");
      return false;
  }

  // Create fingerprint model
  Serial.println();
  Serial.println("🔍 Comparing fingerprint images...");
  delay(1000);
  
  p = finger.createModel();
  if (p == FINGERPRINT_OK) {
    Serial.println("✓ Fingerprints matched successfully!");
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("✗ Communication error during matching");
    return false;
  } else if (p == FINGERPRINT_ENROLLMISMATCH) {
    Serial.println("✗ Fingerprints did not match - please try again");
    Serial.println("💡 Make sure to use the same finger and position");
    return false;
  } else {
    Serial.println("✗ Unknown error during matching");
    return false;
  }

  // Store the fingerprint
  Serial.println();
  Serial.print("💾 Storing fingerprint model at ID #");
  Serial.println(id);
  delay(1000);
  
  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    Serial.println("🎉 Fingerprint enrolled successfully!");
    Serial.println("========================================");
    return true;
  } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
    Serial.println("✗ Communication error during storage");
    return false;
  } else if (p == FINGERPRINT_BADLOCATION) {
    Serial.println("✗ Could not store at that location");
    return false;
  } else if (p == FINGERPRINT_FLASHERR) {
    Serial.println("✗ Error writing to flash memory");
    return false;
  } else {
    Serial.println("✗ Unknown storage error");
    return false;
  }
}

int readNumberInput() {
  String input = readStringInput(10000);
  return input.toInt();
}

String readStringInput(unsigned long timeoutMs) {
  unsigned long startTime = millis();
  String input = "";
  
  while (millis() - startTime < timeoutMs) {
    if (Serial.available()) {
      input = Serial.readString();
      input.trim();
      break;
    }
    delay(100);
  }
  
  return input;
}
