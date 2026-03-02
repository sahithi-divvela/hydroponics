#include <WiFi.h>
#include <WebServer.h>              // ESP32 uses WebServer (not ESP8266WebServer)
#include <DHT.h>

// ────────────────────────────────────────────────
// PINS (ESP32 DevKit / NodeMCU-32S style)
// ────────────────────────────────────────────────
#define DHTPIN       4             // GPIO4 — good for DHT22 (avoid strapping pins)
#define DHTTYPE      DHT22
#define PH_SENSOR    35            // GPIO35 (VP / ADC1_CH7) — analog pH
#define SOIL_SENSOR  35            // GPIO35 — shared analog soil moisture (read one at a time)
#define RS485_RX     16            // GPIO16 — Serial2 RX (default for many boards)
#define RS485_TX     17            // GPIO17 — Serial2 TX
#define RS485_DE_RE  5             // GPIO5  — DE & RE tied together (HIGH = transmit)

// ────────────────────────────────────────────────
// Objects
// ────────────────────────────────────────────────
DHT dht(DHTPIN, DHTTYPE);
WebServer server(80);              // ESP32 WebServer

// ────────────────────────────────────────────────
// WiFi Credentials — CHANGE THESE!
// ────────────────────────────────────────────────
const char* ssid     = "Dulaj's i13";
const char* password = "dulaj2913";

// ────────────────────────────────────────────────
// RS485 Modbus control
// ────────────────────────────────────────────────
void preTransmission() {
  digitalWrite(RS485_DE_RE, HIGH);
}

void postTransmission() {
  digitalWrite(RS485_DE_RE, LOW);
}

// ────────────────────────────────────────────────
// Read one 16-bit value via raw Modbus RTU (common for cheap NPK/RS485 sensors)
// Returns -1 on error/timeout
// ────────────────────────────────────────────────
int readModbusValue(const byte* request, uint8_t len) {
  preTransmission();
  Serial2.write(request, len);
  Serial2.flush();
  postTransmission();

  unsigned long timeout = millis() + 800;
  byte buffer[12];
  uint8_t idx = 0;

  while (millis() < timeout && idx < 10) {
    if (Serial2.available()) {
      buffer[idx++] = Serial2.read();
    }
  }

  // Expected: 01 03 02 HH LL CRC_L CRC_H → 7 bytes
  if (idx >= 7 && buffer[0] == 0x01 && buffer[1] == 0x03 && buffer[2] == 0x02) {
    return (buffer[3] << 8) | buffer[4];
  }
  return -1;
}

// Common commands — slave ID 0x01 — registers very often used by cheap 7-in-1 NPK sensors
const byte nitro_cmd[] = {0x01, 0x03, 0x00, 0x1E, 0x00, 0x01, 0xE4, 0x0C}; // N 0x001E
const byte phos_cmd[]  = {0x01, 0x03, 0x00, 0x1F, 0x00, 0x01, 0xB5, 0xCC}; // P 0x001F
const byte pota_cmd[]  = {0x01, 0x03, 0x00, 0x20, 0x00, 0x01, 0x85, 0xC0}; // K 0x0020

// ────────────────────────────────────────────────
// Read NPK — returns true if all three succeeded
// ────────────────────────────────────────────────
bool readNPK(int& n, int& p, int& k) {
  n = readModbusValue(nitro_cmd, 8);
  delay(80);
  p = readModbusValue(phos_cmd, 8);
  delay(80);
  k = readModbusValue(pota_cmd, 8);

  return (n >= 0 && p >= 0 && k >= 0);
}

// ────────────────────────────────────────────────
// Soil moisture 0–100% (calibrate dry/wet values yourself!)
// ────────────────────────────────────────────────
int readSoilMoisture() {
  int raw = analogRead(SOIL_SENSOR);
  // Example mapping — dry air ≈ 4095, fully wet ≈ 1500–2000 → adjust after testing
  int percentage = map(raw, 4095, 1800, 0, 100);
  percentage = constrain(percentage, 0, 100);
  return percentage;
}

// ────────────────────────────────────────────────
// Connect to WiFi with timeout & restart
// ────────────────────────────────────────────────
void connectToWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 25) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nFailed → restarting in 5s...");
    delay(5000);
    ESP.restart();
  }
}

// ────────────────────────────────────────────────
// /sensor endpoint — JSON response
// ────────────────────────────────────────────────
void handleSensorRequest() {
  float humidity    = dht.readHumidity();
  float temperature = dht.readTemperature();

  if (isnan(humidity) || isnan(temperature)) {
    humidity = -999.0;
    temperature = -999.0;
  }

  // pH — approximate formula — CALIBRATE with buffer solutions!
  int raw_ph = analogRead(PH_SENSOR);
  float voltage = raw_ph * (3.3 / 4095.0);           // ESP32 ADC is 12-bit → 4095 max
  float ph = 7.0 + ((2.5 - voltage) / 0.18);         // ← placeholder — must calibrate!

  int soil_moisture = readSoilMoisture();

  int nitrogen = -1, phosphorus = -1, potassium = -1;
  bool npk_ok = readNPK(nitrogen, phosphorus, potassium);

  String json = "{";
  json += "\"humidity\":"      + String(humidity, 1)    + ",";
  json += "\"temperature\":"   + String(temperature, 1) + ",";
  json += "\"ph\":"            + String(ph, 2)          + ",";
  json += "\"soil_moisture\":" + String(soil_moisture)  + ",";
  json += "\"nitrogen\":"      + String(nitrogen)       + ",";
  json += "\"phosphorus\":"    + String(phosphorus)     + ",";
  json += "\"potassium\":"     + String(potassium);
  if (!npk_ok) {
    json += ",\"npk_error\":true";
  }
  json += "}";

  server.send(200, "application/json", json);
}

// ────────────────────────────────────────────────
// SETUP
// ────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\nESP32 Soil & NPK Monitor starting...");

  pinMode(RS485_DE_RE, OUTPUT);
  digitalWrite(RS485_DE_RE, LOW);       // default: receive mode

  Serial2.begin(9600, SERIAL_8N1, RS485_RX, RS485_TX);   // Hardware UART2

  connectToWiFi();
  dht.begin();

  server.on("/sensor", HTTP_GET, handleSensorRequest);
  server.begin();
  Serial.println("Web server started → http://<IP>/sensor");
}

// ────────────────────────────────────────────────
// LOOP
// ────────────────────────────────────────────────
void loop() {
  server.handleClient();
  // yield();  // usually not needed on ESP32
}
