#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <WebSocketsServer.h> // Added WebSocketsServer library

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE
#include <FirebaseClient.h>
#include "secrets.h"

// Camera pin definitions for AI-Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// Flash LED pin
#define FLASH_LED_PIN 4

// Firebase objects
#define SSL_CLIENT WiFiClientSecure
SSL_CLIENT ssl_client;

using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);

UserAuth user_auth(API_KEY, USER_EMAIL, USER_PASSWORD, 3000);
FirebaseApp app;
RealtimeDatabase Database;
AsyncResult databaseResult;

// WebSocket server on port 81
WebSocketsServer webSocket = WebSocketsServer(81);
bool clientConnected = false;

// Frame control variables for WebSocket streaming
unsigned long wsLastFrameTime = 0;
const unsigned long wsFrameInterval = 100; // ~10 FPS - same as MJPEG stream
bool streamActive = false;

// Helper function for SSL client
void set_ssl_client_insecure_and_buffer(SSL_CLIENT &client) {
#if defined(ESP32) || defined(ESP8266)
    client.setInsecure();
#if defined(ESP8266)
    client.setBufferSizes(4096, 1024);
#endif
#endif
}

// Auth callback function
void auth_debug_print(AsyncResult &aResult) {
    if (aResult.available()) {
        Firebase.printf("Auth result: %s\n", aResult.c_str());
    }
    
    if (aResult.isError()) {
        Firebase.printf("Auth error: %s, code: %d\n", aResult.error().message().c_str(), aResult.error().code());
    }
}

// Web server
WebServer server(80);

// Connection tracking
unsigned long lastConnectionTime = 0;
unsigned long connectionTimeout = 60000; // 60 seconds
bool hasActiveConnection = false;
bool taskComplete = false;

// Process Firebase async results (from Set.ino)
void processData(AsyncResult &aResult) {
    if (!aResult.isResult()) return;

    if (aResult.isEvent()) {
        Firebase.printf("Event task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.eventLog().message().c_str(), aResult.eventLog().code());
    }

    if (aResult.isDebug()) {
        Firebase.printf("Debug task: %s, msg: %s\n", aResult.uid().c_str(), aResult.debug().c_str());
    }

    if (aResult.isError()) {
        Firebase.printf("Error task: %s, msg: %s, code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());
    }

    if (aResult.available()) {
        Firebase.printf("Task: %s, payload: %s\n", aResult.uid().c_str(), aResult.c_str());
    }
}

// WebSocket event handler
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            Serial.printf("[%u] Disconnected!\n", num);
            if (num == 0) clientConnected = false; // Simple client tracking
            break;
        
        case WStype_CONNECTED:
            {
                IPAddress ip = webSocket.remoteIP(num);
                Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
                clientConnected = true;
                lastConnectionTime = millis(); // Reset connection timer on WebSocket connect
                hasActiveConnection = true;
                
                // Send welcome message
                webSocket.sendTXT(num, "Connected to ESP32-CAM WebSocket Server");
                
                // Start streaming when client connects
                streamActive = true;
            }
            break;
        
        case WStype_TEXT:
            // Handle text commands from client (could add camera controls here)
            Serial.printf("[%u] get Text: %s\n", num, payload);
            
            // Example command: "stream_start" and "stream_stop"
            if (strcmp((char*)payload, "stream_start") == 0) {
                streamActive = true;
                webSocket.sendTXT(num, "Streaming started");
            } 
            else if (strcmp((char*)payload, "stream_stop") == 0) {
                streamActive = false;
                webSocket.sendTXT(num, "Streaming stopped");
            }
            break;
            
        case WStype_BIN:
        case WStype_ERROR:
        case WStype_FRAGMENT_TEXT_START:
        case WStype_FRAGMENT_BIN_START:
        case WStype_FRAGMENT:
        case WStype_FRAGMENT_FIN:
            // We don't expect these message types from client
            break;
    }
}

// Send camera frame via WebSocket
void sendCameraFrameWs() {
    if (!clientConnected || !streamActive) return;
    
    // Get time now - limit frame rate
    unsigned long currentTime = millis();
    if (currentTime - wsLastFrameTime < wsFrameInterval) return;
    wsLastFrameTime = currentTime;

    // Get a frame from the camera
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        return;
    }
    
    // Send the JPEG frame via WebSocket in binary mode
    webSocket.broadcastBIN(fb->buf, fb->len);
    
    // Return the frame buffer to the camera driver
    esp_camera_fb_return(fb);
}

// Test Firebase connection with simple write
void testFirebaseWrite() {
    Serial.println("=== TESTING FIREBASE CONNECTION ===");
    Serial.println("⚠️  IMPORTANT: Make sure your Firebase Database Rules allow writes!");
    Serial.println("⚠️  Rules should be: { \"rules\": { \".read\": true, \".write\": true } }");
    Serial.println();
    
    // Try a simple test write first
    String testPath = "/test/connection";
    String testValue = "Hello from ESP32-CAM";
    
    Serial.printf("Testing write to: %s\n", testPath.c_str());
    bool status = Database.set<String>(aClient, testPath, testValue);
    
    if (status) {
        Serial.println("✅ Test write successful!");
    } else {
        Serial.printf("❌ Test write failed: %s (code: %d)\n", 
                      aClient.lastError().message().c_str(), 
                      aClient.lastError().code());
        Serial.println("❌ Common causes:");
        Serial.println("   1. Firebase Database Rules don't allow writes");
        Serial.println("   2. Wrong Database URL");
        Serial.println("   3. Network connectivity issues");
    }
    Serial.println("=== TEST COMPLETE ===\n");
}

// Send IP address to Firebase (with enhanced debugging)
void sendIPToFirebase() {
    String ipAddress = WiFi.localIP().toString();
    String devicePath = "/devices/esp32cam_001/ip_address";
    
    Serial.printf("=== FIREBASE OPERATION START ===\n");
    Serial.printf("Attempting to send IP: %s\n", ipAddress.c_str());
    Serial.printf("Database URL: %s\n", DATABASE_URL);
    Serial.printf("Path: %s\n", devicePath.c_str());
    Serial.printf("App ready: %s\n", app.ready() ? "YES" : "NO");
    Serial.printf("App authenticated: %s\n", app.isAuthenticated() ? "YES" : "NO");
    
    // Use the Database.set function as documented with proper return value checking
    bool status = Database.set<String>(aClient, devicePath, ipAddress);
    
    if (status) {
        Serial.println("✅ IP address sent to Firebase successfully!");
        Serial.printf("✅ Data written: %s = %s\n", devicePath.c_str(), ipAddress.c_str());
        taskComplete = true;
    } else {
        Serial.printf("❌ Failed to send IP to Firebase\n");
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
    
    // Also store the WebSocket port information
    devicePath = "/devices/esp32cam_001/ws_port";
    status = Database.set<int>(aClient, devicePath, 81);
    
    if (status) {
        Serial.println("✅ WebSocket port sent to Firebase successfully!");
    } else {
        Serial.printf("❌ Failed to send WebSocket port to Firebase\n");
    }
}

// Camera configuration
void setupCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA; // 640x480
    config.jpeg_quality = 12;
    config.fb_count = 1;
    
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        return;
    }
    
    Serial.println("Camera initialized successfully");
}

// Handle root - serve WebSocket info page
void handleRoot() {
    String html = "<html><head><title>ESP32-CAM WebSocket Server</title>";
    html += "<style>body{font-family:Arial,sans-serif;max-width:800px;margin:0 auto;padding:20px;line-height:1.6}</style>";
    html += "</head><body>";
    html += "<h1>ESP32-CAM WebSocket Server</h1>";
    html += "<p>This ESP32-CAM is configured for WebSocket streaming.</p>";
    html += "<p><strong>WebSocket URL:</strong> ws://" + WiFi.localIP().toString() + ":81/ws</p>";
    html += "<p>Use the ESP32 Client app to view the stream.</p>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
    
    // Update connection time
    lastConnectionTime = millis();
    hasActiveConnection = true;
}

// Handle status - return system status information
void handleStatus() {
    String status = "{";
    status += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    status += "\"wsPort\":81,";
    status += "\"uptime\":" + String(millis() / 1000) + ",";
    status += "\"connected\":" + String(clientConnected ? "true" : "false");
    status += "}";
    
    server.send(200, "application/json", status);
    
    // Update connection time
    lastConnectionTime = millis();
    hasActiveConnection = true;
}

// No replacement needed as this functionality is now in handleRoot()

void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("ESP32-CAM with Firebase Starting...");
    
    // Initialize flash LED
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(FLASH_LED_PIN, LOW);
    
    // Setup camera
    setupCamera();
    
    // Setup WiFi (from Set.ino pattern)
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("Connecting to Wi-Fi");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        delay(300);
    }
    Serial.println();
    Serial.print("Connected with IP: ");
    Serial.println(WiFi.localIP());
    Serial.println();
    
    // Initialize Firebase (using simplified approach)
    Firebase.printf("Firebase Client v%s\n", FIREBASE_CLIENT_VERSION);
    set_ssl_client_insecure_and_buffer(ssl_client);
    Serial.println("Initializing Firebase authentication...");
    
    // Use the correct static function Firebase::initializeApp with proper signature
    Firebase.initializeApp(aClient, app, getAuth(user_auth));
    
    // Get database instance
    app.getApp<RealtimeDatabase>(Database);
    Database.url(DATABASE_URL);
    
    Serial.println("Firebase initialization complete");
    
    // Test Firebase connection
    delay(2000); // Give it a moment to settle
    testFirebaseWrite();
    
    // Initialize connection tracking
    lastConnectionTime = millis();
    
    // Setup web server for basic info only
    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });
    
    server.begin();
    Serial.println("HTTP server started (info only)");
    
    // Setup and start WebSocket server
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.println("WebSocket server started on port 81");
    
    Serial.printf("WebSocket URL: ws://%s:81/ws\n", WiFi.localIP().toString().c_str());
    Serial.println("ESP32-CAM ready!");
}

void loop() {
    // Maintain Firebase authentication and async tasks (from Set.ino)
    app.loop();
    
    // Process Firebase async results
    processData(databaseResult);
    
    // Send IP to Firebase once when app is ready (proper pattern from documentation)
    if (app.ready() && !taskComplete) {
        taskComplete = true;
        sendIPToFirebase();
    }
    
    // Handle web server
    server.handleClient();
    
    // Handle WebSocket clients
    webSocket.loop();
    
    // Send camera frame via WebSocket if active
    sendCameraFrameWs();
    
    // Connection timeout check - consider both HTTP and WebSocket connections
    if (!hasActiveConnection && !clientConnected) {
        unsigned long currentTime = millis();
        if (currentTime - lastConnectionTime > connectionTimeout) {
            Serial.println("No connections detected for 60 seconds. Restarting ESP32...");
            delay(1000);
            ESP.restart();
        }
    }
    
    yield(); // Allow other processes to run
}
