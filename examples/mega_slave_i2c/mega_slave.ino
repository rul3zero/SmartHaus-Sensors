  /*
    Arduino Mega I2C Slave - listens on address 0x08 and prints received messages
    SDA -> 20, SCL -> 21 on Mega (hardware I2C pins)
    Make sure both boards share GND.
  */
  #include <Arduino.h>
  #include <Wire.h>
  #include <SoftwareSerial.h>

  const uint8_t SLAVE_ADDR = 0x08;
  
  // SIM800L module setup
  #define SIM800L_TX 18  // Mega TX1 -> SIM800L RX
  #define SIM800L_RX 19  // Mega RX1 -> SIM800L TX
  #define SIM800L_RST 7  // Reset pin (optional)
  // Set your phone number for SMS alerts (e.g., "+1234567890")
  const String PHONE_NUMBER = "+YOUR_PHONE_NUMBER_HERE"; // <-- Fill in your number
  
  // Use Hardware Serial1 for SIM800L (pins 18,19)
  #define sim800l Serial1
  
  // SMS spam prevention
  bool alertSMSSent = false;
  bool waterEmptySMSSent = false;
  
  // SMS State Machine
  enum SMSState {
    SMS_IDLE,
    SMS_SENDING_COMMAND,
    SMS_WAITING_PROMPT,
    SMS_SENDING_MESSAGE,
    SMS_WAITING_RESPONSE
  };
  
  SMSState smsState = SMS_IDLE;
  String pendingSMSMessage = "";
  unsigned long smsTimestamp = 0;
  const unsigned long SMS_TIMEOUT = 10000; // 10 second timeout
  
  // receive buffer
  char recvBuf[128];
  size_t recvLen = 0;

  // Relay mapping and state cache
  const int RELAY_BASE_PIN = 22; // ID 1 -> pin 22, ID 2 -> 23, ...
  const int MAX_RELAYS = 16; // configurable upper bound
  bool relayInitialized[MAX_RELAYS + 1] = {false};
  bool relayState[MAX_RELAYS + 1] = {false};
  // Set true if your relay board activates on LOW (common for some modules)
  const bool RELAY_ACTIVE_LOW = true; // relays are active-low in your setup


  // Dedicated water relay
  const int WATER_RELAY_PIN = 52;
  bool waterRelayInitialized = false;
  bool waterRelayState = false; // false = OFF, true = ON
  // Water sensor input: when dry, water relay must remain OFF regardless of commands.
  // Choose a free digital pin on Mega; change as needed for your wiring.
  // Note: using INPUT (no internal pullup). Sensor should drive pin HIGH when WET, LOW when DRY.
  const int WATER_SENSOR_PIN = 48; // wired to float/wet sensor; uses INPUT (HIGH = wet)
  bool waterSensorInitialized = false;
  bool waterSensorWet = false; // true = wet, false = dry
  bool waterRequestedState = false; // last requested desired state from commands
  unsigned long lastWaterSensorCheck = 0;
  const unsigned long WATER_SENSOR_CHECK_INTERVAL = 1000; // ms

  // Forward declaration for receiveEvent function
  void receiveEvent(int howMany);

  // SIM800L Functions
  void readAndPrintResponse() {
    String response = "";
    unsigned long timeout = millis() + 2000;
    while (millis() < timeout) {
      if (sim800l.available()) {
        response += (char)sim800l.read();
      }
    }
    if (response.length() > 0) {
      Serial.print(F("SIM800L Response: "));
      Serial.println(response);
    }
  }
  
  void initSIM800L() {
    Serial.println(F("Initializing SIM800L..."));
    
    // Initialize reset pin
    pinMode(SIM800L_RST, OUTPUT);
    digitalWrite(SIM800L_RST, HIGH);
    delay(100);
    
    // Hardware reset
    Serial.println(F("Resetting SIM800L..."));
    digitalWrite(SIM800L_RST, LOW);
    delay(100);
    digitalWrite(SIM800L_RST, HIGH);
    delay(5000); // Wait longer for module to boot
    
    // Use 115200 baud rate (confirmed working)
    Serial.println(F("Starting SIM800L at 115200 baud..."));
    sim800l.begin(115200);
    delay(1000);
    
    // Clear any existing data
    while (sim800l.available()) {
      sim800l.read();
    }
    
    // Test AT command
    sim800l.println("AT");
    delay(1000);
    
    String response = "";
    unsigned long timeout = millis() + 3000;
    while (millis() < timeout) {
      if (sim800l.available()) {
        response += (char)sim800l.read();
      }
    }
    
    Serial.print(F("AT Response: "));
    Serial.println(response);
    
    if (response.indexOf("OK") < 0) {
      Serial.println(F("❌ Failed to connect to SIM800L at 115200"));
      return;
    }
    
    Serial.println(F("✅ SIM800L connected at 115200 baud"));
    
    // Check network registration
    Serial.println(F("Checking network..."));
    sim800l.println("AT+CREG?");
    delay(1000);
    readAndPrintResponse();
    
    // Check signal quality
    Serial.println(F("Checking signal..."));
    sim800l.println("AT+CSQ");
    delay(1000);
    readAndPrintResponse();
    
    // Set text mode for SMS
    Serial.println(F("Setting SMS text mode..."));
    sim800l.println("AT+CMGF=1");
    delay(1000);
    readAndPrintResponse();
    
    Serial.println(F("✅ SIM800L initialized successfully"));
  }
  
  void sendSMS(String message) {
    // Only start SMS if not already in progress
    if (smsState == SMS_IDLE) {
      Serial.print(F("📱 Queuing SMS: ")); Serial.println(message);
      pendingSMSMessage = message;
      smsState = SMS_SENDING_COMMAND;
      smsTimestamp = millis();
      
      // Send AT+CMGS command
      sim800l.print("AT+CMGS=\"");
      sim800l.print(PHONE_NUMBER);
      sim800l.println("\"");
    } else {
      Serial.println(F("⚠️ SMS already in progress, skipping"));
    }
  }
  
  void processSMSStateMachine() {
    switch (smsState) {
      case SMS_IDLE:
        // Nothing to do
        break;
        
      case SMS_SENDING_COMMAND:
        // Wait 2 seconds after sending command, then look for prompt
        if (millis() - smsTimestamp > 2000) {
          smsState = SMS_WAITING_PROMPT;
          Serial.println(F("Looking for > prompt..."));
        }
        break;
        
      case SMS_WAITING_PROMPT:
        // Check for > prompt in available data
        if (sim800l.available()) {
          char c = sim800l.read();
          if (c == '>') {
            Serial.println(F("Got > prompt, sending message"));
            sim800l.print(pendingSMSMessage);
            sim800l.write(26); // Ctrl+Z
            smsState = SMS_WAITING_RESPONSE;
            smsTimestamp = millis();
          }
        }
        // Timeout check
        if (millis() - smsTimestamp > 5000) {
          Serial.println(F("❌ Timeout waiting for prompt"));
          smsState = SMS_IDLE;
        }
        break;
        
      case SMS_WAITING_RESPONSE:
        // Wait for completion, then reset to idle
        if (millis() - smsTimestamp > 5000) {
          Serial.println(F("✅ SMS completed"));
          smsState = SMS_IDLE;
        }
        break;
    }
    
    // Global timeout
    if (smsState != SMS_IDLE && millis() - smsTimestamp > SMS_TIMEOUT) {
      Serial.println(F("❌ SMS timeout, resetting"));
      smsState = SMS_IDLE;
    }
  }
  
  void sendStartupSMS(String message) {
    // Simple SMS for startup (no I2C to disable)
    Serial.print(F("📱 Startup SMS: ")); Serial.println(message);
    
    sim800l.print("AT+CMGS=\"");
    sim800l.print(PHONE_NUMBER);
    sim800l.println("\"");
    delay(2000);
    sim800l.print(message);
    delay(100);
    sim800l.write(26);
    delay(3000);
    
    Serial.println(F("✅ Startup SMS sent"));
  }
  
  void readSIM800LResponse() {
    // Only read responses when not actively sending SMS
    if (smsState != SMS_IDLE) return;
    
    if (sim800l.available()) {
      String response = sim800l.readString();
      Serial.print(F("SIM800L: "));
      Serial.println(response);
    }
  }

  void applyWaterRelay(bool on) {
    // Remember requested state
    waterRequestedState = on;

    // Lazy init water relay
    if (!waterRelayInitialized) {
      pinMode(WATER_RELAY_PIN, OUTPUT);
      digitalWrite(WATER_RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
      waterRelayInitialized = true;
      waterRelayState = false;
      Serial.print(F("Initialized water relay pin: ")); Serial.println(WATER_RELAY_PIN);
    }

    // Lazy init water sensor (if not initialized in setup)
    if (!waterSensorInitialized) {
      pinMode(WATER_SENSOR_PIN, INPUT);
      waterSensorInitialized = true;
      // With external wiring/high-active sensor, HIGH == wet
      waterSensorWet = (digitalRead(WATER_SENSOR_PIN) == HIGH); // HIGH = wet
      Serial.print(F("Initialized water sensor pin: ")); Serial.print(WATER_SENSOR_PIN);
      Serial.print(F(" (wet=")); Serial.print(waterSensorWet ? "true" : "false"); Serial.println(")");
    }

    // Actual hardware state only allowed when sensor reports wet
    bool actualOn = on && waterSensorWet;

    if (waterRelayState == actualOn) {
      // If the requested change couldn't be applied due to dry sensor, log once
      if (on && !waterSensorWet) {
        Serial.println(F("⚠️ Water sensor dry - cannot turn water relay ON; keeping OFF"));
      }
      return;
    }

    waterRelayState = actualOn;
    digitalWrite(WATER_RELAY_PIN, (actualOn ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
    Serial.print(F("Water relay (pin ")); Serial.print(WATER_RELAY_PIN); Serial.print(F(") set to ")); Serial.println(actualOn ? "ON" : "OFF");
  }

  // Poll water sensor and re-evaluate water relay when sensor changes
  void updateWaterSensorIfNeeded() {
    if (!waterSensorInitialized) return;
    if (millis() - lastWaterSensorCheck < WATER_SENSOR_CHECK_INTERVAL) return;
    lastWaterSensorCheck = millis();

  // HIGH == wet, LOW == dry
  bool nowWet = (digitalRead(WATER_SENSOR_PIN) == HIGH);
    if (nowWet == waterSensorWet) return; // no change

    waterSensorWet = nowWet;
    Serial.print(F("Water sensor changed: ")); Serial.println(waterSensorWet ? "WET" : "DRY");

    // Re-evaluate requested state: if previously requested ON and sensor now wet, this will turn it ON
    // If sensor becomes dry, this will ensure relay is turned OFF regardless of request
    applyWaterRelay(waterRequestedState);
  }

  // Unlock relay (pin 53) - default ON, payload "unlock" turns it OFF
  const int UNLOCK_RELAY_PIN = 53;
  bool unlockRelayInitialized = false;
  bool unlockRelayState = true; // default ON

  void applyUnlockRelay(bool on) {
    if (!unlockRelayInitialized) {
      pinMode(UNLOCK_RELAY_PIN, OUTPUT);
      // Default ON at startup
      digitalWrite(UNLOCK_RELAY_PIN, (true ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
      unlockRelayInitialized = true;
      unlockRelayState = true;
      Serial.print(F("Initialized unlock relay pin: ")); Serial.println(UNLOCK_RELAY_PIN);
    }
    if (unlockRelayState == on) return;
    unlockRelayState = on;
    digitalWrite(UNLOCK_RELAY_PIN, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
    Serial.print(F("Unlock relay (pin ")); Serial.print(UNLOCK_RELAY_PIN); Serial.print(F(") set to ")); Serial.println(on ? "ON" : "OFF");
  }

  // Apply a relay command to hardware: id -> pin (RELAY_BASE_PIN + id - 1)
  void applyRelayCommand(uint16_t id, bool on) {
    if (id < 1 || id > MAX_RELAYS) return;
    int pin = RELAY_BASE_PIN + (id - 1);
    // Initialize pin mode on first use
    if (!relayInitialized[id]) {
      pinMode(pin, OUTPUT);
    // initialize to OFF with correct polarity
    digitalWrite(pin, RELAY_ACTIVE_LOW ? HIGH : LOW);
      relayInitialized[id] = true;
      relayState[id] = false;
      Serial.print(F("Initialized relay pin: ")); Serial.print(pin); Serial.print(F(" for id=")); Serial.println(id);
    }

    if (relayState[id] == on) return; // no change
    relayState[id] = on;
    digitalWrite(pin, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
    Serial.print(F("Relay id=")); Serial.print(id); Serial.print(F(" -> pin ")); Serial.print(pin);
    Serial.print(F(" set to ")); Serial.println(on ? "ON" : "OFF");
  }

  // Simple packet processing - just handle "id:state" format
  void processPacket(const char *packet, size_t len) {
    String p = String(packet);
    p.trim();
    if (p.length() == 0) return;

    Serial.print(F("packet raw: '")); Serial.print(p); Serial.println('\'');

    // Handle special commands first
    if (p.equalsIgnoreCase("lock")) {
      applyUnlockRelay(true);
      return;
    }
    if (p.equalsIgnoreCase("unlock")) {
      applyUnlockRelay(false);
      // Reset alert SMS flag when system is unlocked (valid access)
      if (alertSMSSent) {
        Serial.println(F("🔓 System unlocked - resetting alert SMS flag"));
        alertSMSSent = false;
      }
      return;
    }
    if (p.equalsIgnoreCase("waterempty")) {
      applyWaterRelay(true);
      if (!waterEmptySMSSent) {
        Serial.println(F("💧 WATER EMPTY - Sending alert SMS"));
        sendSMS("Alert! Water is Empty");
        Serial.println(F("✅ Water empty SMS queued"));
        waterEmptySMSSent = true;
      } else {
        Serial.println(F("💧 Water empty SMS already sent - not spamming"));
      }
      return;
    }
    if (p.equalsIgnoreCase("waterpresent")) {
      applyWaterRelay(false);
      // Reset water empty SMS flag when water is present again
      if (waterEmptySMSSent) {
        Serial.println(F("💧 Water is present again - resetting SMS flag"));
        waterEmptySMSSent = false;
      }
      return;
    }
    if (p.equalsIgnoreCase("alert")) {
      if (!alertSMSSent) {
        Serial.println(F("🚨 ALERT RECEIVED - Sending intruder SMS"));
        sendSMS("Intruder Alert, 3 Maximum attempt is reached");
        Serial.println(F("✅ Alert SMS queued"));
        alertSMSSent = true;
      } else {
        Serial.println(F("🚨 Alert SMS already sent - not spamming"));
      }
      return;
    }

    // Parse simple "id:state" format (e.g., "1:1", "2:0")
    int sep = p.indexOf(':');
    if (sep != -1) {
      String idStr = p.substring(0, sep);
      String stateStr = p.substring(sep + 1);
      idStr.trim(); 
      stateStr.trim();
      
      uint16_t id = (uint16_t)idStr.toInt();
      
      Serial.print("Relay command id="); Serial.print(id);
      Serial.print(" state='"); Serial.print(stateStr); Serial.println("'");
      
      if (id >= 1 && id <= MAX_RELAYS) {
        bool on = (stateStr == "1");
        applyRelayCommand(id, on);
      } else {
        Serial.print(F("⚠️ Relay id out of range: ")); Serial.println(id);
      }
      return;
    }

    // If we reach here the packet couldn't be parsed
    Serial.print(F("Malformed packet: '")); Serial.print(p); Serial.println('\'');
  }

  void receiveEvent(int howMany) {
    Serial.print(F("onReceive howMany=")); Serial.println(howMany);
    while (Wire.available()) {
      int b = Wire.read();
      char c = (char)b;
      Serial.print(F(" recv byte: 0x")); Serial.print(b, HEX); Serial.print(F(" '")); Serial.print(c); Serial.println('\'');
      // store until newline or buffer full
      if (recvLen < sizeof(recvBuf) - 1) {
        recvBuf[recvLen++] = c;
      }
      // if newline received, process packet
      if (c == '\n') {
        recvBuf[recvLen] = '\0';
        processPacket(recvBuf, recvLen);
        recvLen = 0;
      }
    }
    // If the master sent a transmission without a trailing newline, treat the
    // available bytes as a complete packet (common when sender uses Wire.write without '\n').
    if (recvLen > 0) {
      recvBuf[recvLen] = '\0';
      processPacket(recvBuf, recvLen);
      recvLen = 0;
    }
  }
  void setup() {
    Serial.begin(57600);
    while (!Serial) ;
    Serial.println("Mega I2C Slave starting...");

    // Initialize SIM800L first
    initSIM800L();
    
    // Try to send startup SMS (don't block if it fails)
    sendStartupSMS("done initialize");
    Serial.println(F("⚠️ Startup SMS sent - continuing with normal operation"));
    
    Wire.begin(SLAVE_ADDR); // join I2C bus as slave
    Wire.onReceive(receiveEvent);

    Serial.print("Listening on I2C address 0x");
    Serial.print(SLAVE_ADDR, HEX);
    Serial.println(" as slave.");

    // Ensure unlock relay is ON by default
    applyUnlockRelay(true);
  // Initialize water sensor (optional pin init). Uses INPUT; sensor should drive HIGH when wet.
  pinMode(WATER_SENSOR_PIN, INPUT);
  waterSensorInitialized = true;
  waterSensorWet = (digitalRead(WATER_SENSOR_PIN) == HIGH);
  Serial.print(F("Water sensor initial (pin " )); Serial.print(WATER_SENSOR_PIN); Serial.print(F(") wet=")); Serial.println(waterSensorWet ? "true" : "false");
  
  Serial.println("=== MEGA SLAVE READY ===");
  }

  void loop() {
    // Poll water sensor so that relay respects wet/dry status
    updateWaterSensorIfNeeded();
    
    // Non-blocking SMS state machine processing
    processSMSStateMachine();
    
    // Monitor SIM800L responses only when not actively sending SMS
    if (smsState == SMS_IDLE) {
      readSIM800LResponse();
    }
    
    delay(10); // Reduced delay for more responsive SMS state machine
  }
