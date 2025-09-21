#ifndef EXAMPLE_FUNCTIONS_H
#define EXAMPLE_FUNCTIONS_H

#include <FirebaseClient.h>

// WiFi library used in the examples
#if defined(ESP32) || defined(ARDUINO_RASPBERRY_PI_PICO_W) || defined(ARDUINO_GIGA) || defined(ARDUINO_OPTA)
#include <WiFi.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#elif __has_include(<WiFiNINA.h>) || defined(ARDUINO_NANO_RP2040_CONNECT)
#include <WiFiNINA.h>
#elif __has_include(<WiFi101.h>)
#include <WiFi101.h>
#elif __has_include(<WiFiS3.h>) || defined(ARDUINO_UNOWIFIR4)
#include <WiFiS3.h>
#elif __has_include(<WiFiC3.h>) || defined(ARDUINO_PORTENTA_C33)
#include <WiFiC3.h>
#elif __has_include(<WiFi.h>)
#include <WiFi.h>
#endif

// SSL Client used in the examples
#if defined(ESP32) || defined(ESP8266) || defined(ARDUINO_RASPBERRY_PI_PICO_W)
#include <WiFiClientSecure.h>
#define SSL_CLIENT WiFiClientSecure
#elif defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_UNOWIFIR4) || defined(ARDUINO_GIGA) || defined(ARDUINO_OPTA) || defined(ARDUINO_PORTENTA_C33) || defined(ARDUINO_NANO_RP2040_CONNECT)
#include <WiFiSSLClient.h>
#define SSL_CLIENT WiFiSSLClient
#else
#define SSL_CLIENT ESP_SSLClient
#endif

// Set some SSL client for skipping server certificate verification.
void set_ssl_client_insecure_and_buffer(SSL_CLIENT &client)
{
#if defined(ESP32) || defined(ESP8266) || defined(PICO_RP2040)
    client.setInsecure();
#if defined(ESP8266)
    client.setBufferSizes(4096, 1024);
#endif
#else
    (void)client;
#endif
}

// Helper function to initialize Firebase app  
void initializeApp(AsyncClientClass &client, FirebaseApp &app, UserAuth auth, AsyncResultCallback cb, const String &uid)
{
    app.initializeApp(client, auth, cb, uid.c_str());
}

#endif // EXAMPLE_FUNCTIONS_H
