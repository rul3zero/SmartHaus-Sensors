NodeMCU <-> Arduino Mega I2C example

Files:
- examples/nodemcu_master_i2c/nodemcu_master.ino  (ESP8266/NodeMCU master)
- examples/mega_slave_i2c/mega_slave.ino          (Arduino Mega slave)

Wiring (basic):
- Connect grounds together (GND -> GND)
- NodeMCU D2 (SDA, GPIO4) -> Mega SDA (pin 20)
- NodeMCU D1 (SCL, GPIO5) -> Mega SCL (pin 21)

Notes:
- No level-shifter required for 5V Mega when using I2C in many cases because I2C is open-drain and NodeMCU's internal pull-ups are 3.3V tolerant; however, for robustness use a proper bi-directional level shifter if you have any doubt.
- Ensure both boards share a common ground.
- NodeMCU acts as master and sends the ASCII string "testing" to slave address 0x08 every 2 seconds.
- Mega prints received messages to Serial at 115200.

How to test:
1. Flash `mega_slave.ino` to the Arduino Mega. Open Serial Monitor at 115200.
2. Flash `nodemcu_master.ino` to the NodeMCU. Open its Serial Monitor at 115200.
3. You should see the NodeMCU print "Sending 'testing' to 0x08" and the Mega print "Received I2C message: 'testing'" every 2 seconds.

Troubleshooting:
- If messages are not received, check wiring and shared ground.
- If the NodeMCU reports a transmission error, try adding small delays (10-50ms) between beginTransmission and endTransmission or reduce message rate.
- For production, implement a small ACK protocol from slave to master (slave writes a one-byte ack back) and retries on no-ack.

Next steps:
- Replace the NodeMCU's periodic "testing" message with real status updates (fingerprint ID, water level).
- Add a simple ACK/retry mechanism and message framing (length prefix) for reliability.
