/******************************************************
 *  HF Amplifier Controller + WiFi / MQTT / OTA
 *  - PTT IN / PTT OUT
 *  - Band decode from 4 digital inputs (0..15 code)
 *  - Band outputs on 4 digital pins
 *  - PTT blocked while PTT active
 *  - PTT additionally blocked for N seconds
 *    after a band change (configurable)
 *  - WiFi STA + AP fallback
 *  - Web config + status page
 *  - OTA firmware update
 *  - MQTT status publishing
 *
 *  Firmware: VER
 ******************************************************/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#define MQTT_MAX_PACKET_SIZE 512
#include <PubSubClient.h>
#include <EEPROM.h>
#include <ArduinoJson.h>

// -------------------------------------------------------------------
//  Version & WiFi / MQTT globals
// -------------------------------------------------------------------
#define VER "AMP-CTRL v1.00 Nov 2025 S53M"

WiFiClient espClient;
PubSubClient client(espClient);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;

#define AP_SSID "AMP_Setup"

char ssid[32]           = "";
char password[32]       = "";
char mqtt_server[40]    = "";
int  mqtt_port          = 1883;
char amplifier_name[32] = "AMP-01";   // renamed from station_name
int  block_ptt_seconds  = 15;         // configurable "Block PTT for" (seconds)

char macAddress[18]     = "";
char topic_status[128]  = "";
char topic_debug[128]   = "";

// Debug control
bool debugEnabled            = true;
unsigned long debugDeadline  = 60000;   // 60s after boot

// -------------------------------------------------------------------
//  Amplifier hardware / logic
// -------------------------------------------------------------------

// Mask for 4-bit band code (0..15)
const byte Mask = 15;

// Current & previous band
uint8_t band    = 0xFF;
uint8_t bandOld = 0xFF;

// --- PIN DEFINITIONS -----------------------------------------------
// Adjust these to match your hardware wiring on the WiFi-capable board.

// PTT pins
const int pttInp      = 7;   // PTT IN  (active LOW)
const int pttOut      = 6;   // PTT OUT (active HIGH)
const int uglasevanje = A0;  // optional tuning input, can be reused

// 4 band input pins (form 4-bit code)
// These replace the old A0..A3 + PINC method.
// Map them to your actual 4-band-code lines.
const int bandInPins[4] = {
  2,  // bit0
  3,  // bit1
  4,  // bit2
  5   // bit3
};

// 4 band output pins (replace old PORTD bits 2..5)
const int bandOutPins[4] = {
  8,   // OUT0 (was D2)
  9,   // OUT1 (was D3)
  10,  // OUT2 (was D4)
  11   // OUT3 (was D5)
};

// PTT block timing (after band change)
unsigned long lastBandChangeTime = 0;
unsigned long bandBlockTimeMs    = 15000UL;  // updated from block_ptt_seconds

// PTT state
bool pttState          = false;  // false=RX, true=TX
bool pttBlockReported  = false;  // to avoid spamming logs

// -------------------------------------------------------------------
//  Debug helpers
// -------------------------------------------------------------------
void publishDebugMessage(const char* msg) {
  if (!debugEnabled) return;

  Serial.println(msg);
  if (client.connected() && topic_debug[0]) {
    client.publish(topic_debug, msg);
  }
}

// -------------------------------------------------------------------
//  EEPROM config
// -------------------------------------------------------------------
void loadConfigFromEEPROM() {
  EEPROM.begin(512);
  EEPROM.get(0,  ssid);
  EEPROM.get(32, password);
  EEPROM.get(64, mqtt_server);
  EEPROM.get(104, mqtt_port);
  EEPROM.get(108, amplifier_name);
  EEPROM.get(140, block_ptt_seconds);
  EEPROM.end();

  ssid[sizeof(ssid) - 1]             = '\0';
  password[sizeof(password) - 1]     = '\0';
  mqtt_server[sizeof(mqtt_server) - 1] = '\0';
  amplifier_name[sizeof(amplifier_name) - 1] = '\0';

  if (mqtt_port <= 0 || mqtt_port > 65535) mqtt_port = 1883;
  if (block_ptt_seconds <= 0 || block_ptt_seconds > 600) {
    block_ptt_seconds = 15;  // sane default
  }
  bandBlockTimeMs = (unsigned long)block_ptt_seconds * 1000UL;

  Serial.println(F("Reading configuration from EEPROM"));
  Serial.printf("  SSID: %s\n", ssid);
  Serial.printf("  MQTT Server: %s\n", mqtt_server);
  Serial.printf("  MQTT Port: %d\n", mqtt_port);
  Serial.printf("  Amplifier Name: %s\n", amplifier_name);
  Serial.printf("  Block PTT for: %d s\n", block_ptt_seconds);
}

// -------------------------------------------------------------------
//  WiFi / AP setup
// -------------------------------------------------------------------
void setupAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("AP mode started, SSID: %s, IP: %s\n", AP_SSID, ip.toString().c_str());
}

void setupWiFi() {
  delay(10);
  Serial.println();
  Serial.printf("Connecting to \"%s\" …\n", ssid);

  if (ssid[0] == '\0') {
    Serial.println(F("No SSID configured, going directly to AP mode."));
    setupAP();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.hostname(amplifier_name);
  WiFi.begin(ssid, password);

  const uint32_t CONNECT_TIMEOUT_MS = 30000;
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0 < CONNECT_TIMEOUT_MS)) {
    delay(500);
    Serial.print('.');
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi connect timeout – starting AP fallback");
    setupAP();
    return;
  }

  Serial.println();
  Serial.print("WiFi connected! IP = ");
  Serial.println(WiFi.localIP());

  // MAC address
  String macStr = WiFi.macAddress();
  macStr.toCharArray(macAddress, sizeof(macAddress));

  // MQTT topics
  snprintf(topic_status, sizeof(topic_status),
           "matrigs/0/sta/%s/amp/status", amplifier_name);
  snprintf(topic_debug, sizeof(topic_debug),
           "matrigs/0/sta/%s/amp/debug", amplifier_name);

  Serial.print("Status topic: "); Serial.println(topic_status);
  Serial.print("Debug topic:  "); Serial.println(topic_debug);
}

// -------------------------------------------------------------------
//  Web pages
// -------------------------------------------------------------------
void handleUpdate() {
  publishDebugMessage("[HTTP] OTA upload page");
  server.send(200, "text/html",
              "<!DOCTYPE html><html><head><meta charset='utf-8'>"
              "<title>OTA Update</title></head><body>"
              "<h1>Firmware Upload</h1>"
              "<form method='POST' action='/update' enctype='multipart/form-data'>"
              "<input type='file'   name='update'><br><br>"
              "<input type='submit' value='Upload & Flash'>"
              "</form>"
              "<p>The device will reboot automatically after a successful upload.</p>"
              "</body></html>");
}

void handleSave() {
  strncpy(ssid,           server.arg("ssid").c_str(),           sizeof(ssid));
  strncpy(password,       server.arg("password").c_str(),       sizeof(password));
  strncpy(mqtt_server,    server.arg("mqtt_server").c_str(),    sizeof(mqtt_server));
  mqtt_port = server.arg("mqtt_port").toInt();
  strncpy(amplifier_name, server.arg("amplifier_name").c_str(), sizeof(amplifier_name));
  block_ptt_seconds = server.arg("block_ptt_seconds").toInt();
  if (block_ptt_seconds <= 0 || block_ptt_seconds > 600) block_ptt_seconds = 15;

  bandBlockTimeMs = (unsigned long)block_ptt_seconds * 1000UL;

  EEPROM.begin(512);
  EEPROM.put(0,  ssid);
  EEPROM.put(32, password);
  EEPROM.put(64, mqtt_server);
  EEPROM.put(104, mqtt_port);
  EEPROM.put(108, amplifier_name);
  EEPROM.put(140, block_ptt_seconds);
  EEPROM.commit();
  EEPROM.end();

  server.send(200, "text/html",
              "<html><body><h2>Saved. Rebooting...</h2></body></html>");
  delay(1000);
  ESP.restart();
}

void handleRoot() {
  publishDebugMessage("[HTTP] root");

  String html;
  html.reserve(2000);

  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>Amplifier Config</title><style>"
            "body{font-family:Arial,Helvetica,sans-serif;margin:20px;color:#222;}"
            "h1{color:#006edc;} input[type=text],input[type=number]{width:260px;}"
            "input[type=submit]{padding:6px 18px;margin-top:10px;}"
            "code{background:#f2f2f2;padding:2px 4px;border-radius:3px;}"
            "</style></head><body>");

  html += F("<h1>Wi-Fi & MQTT Config (Amplifier)</h1>");
  html += F("<form action='/save' method='post'>");

  html += F("SSID:<br><input name='ssid' type='text' value='");
  html += ssid;
  html += F("'><br>Password:<br><input name='password' type='text' value='");
  html += password;
  html += F("'><br>MQTT Server IP:<br><input name='mqtt_server' type='text' value='");
  html += mqtt_server;
  html += F("'><br>MQTT Port:<br><input name='mqtt_port' type='number' value='");
  html += String(mqtt_port);
  html += F("'><br>Amplifier Name (AMP-XX):<br><input name='amplifier_name' type='text' value='");
  html += amplifier_name;
  html += F("'><br>Block PTT for (sec):<br><input name='block_ptt_seconds' type='number' value='");
  html += String(block_ptt_seconds);
  html += F("'><br><input type='submit' value='Save & Reboot'></form>");

  // Status section
  html += F("<h2>Status</h2><ul>");

  html += F("<li>Firmware: <b>");
  html += VER;
  html += F("</b></li>");

  html += F("<li>MAC Address: <code>");
  html += macAddress;
  html += F("</code></li>");

  html += F("<li>Amplifier Name: <b>");
  html += amplifier_name;
  html += F("</b></li>");

  html += F("<li>WiFi mode: ");
  if (WiFi.getMode() == WIFI_AP) html += F("AP");
  else if (WiFi.status() == WL_CONNECTED) html += F("STA (connected)");
  else html += F("STA (NOT connected)");
  html += F("</li>");

  html += F("<li>IP Address: <code>");
  if (WiFi.getMode() == WIFI_AP) html += WiFi.softAPIP().toString();
  else if (WiFi.status() == WL_CONNECTED) html += WiFi.localIP().toString();
  else html += F("n/a");
  html += F("</code></li>");

  html += F("<li>Current PTT state: <b>");
  html += (pttState ? "TX" : "RX");
  html += F("</b></li>");

  html += F("<li>Current band code: ");
  html += String(band);
  html += F("</li>");

  bool blockActive = (millis() - lastBandChangeTime <= bandBlockTimeMs);
  html += F("<li>PTT block active: ");
  html += (blockActive ? "YES" : "NO");
  if (blockActive) {
    unsigned long remaining = bandBlockTimeMs - (millis() - lastBandChangeTime);
    html += F(" (");
    html += String(remaining / 1000);
    html += F(" s left)");
  }
  html += F("</li>");

  html += F("<li>MQTT status topic: <code>");
  html += topic_status;
  html += F("</code></li>");

  html += F("<li>MQTT debug topic: <code>");
  html += topic_debug;
  html += F("</code></li>");

  html += F("</ul><h2>OTA Update</h2>"
            "<p>Open <a href='/update'>/update</a> to upload a new .bin.</p>"
            "</body></html>");

  server.send(200, "text/html", html);
}

// -------------------------------------------------------------------
//  MQTT
// -------------------------------------------------------------------
void publishAmpStatus() {
  if (!client.connected() || topic_status[0] == '\0') return;

  StaticJsonDocument<256> doc;
  doc["ptt"] = pttState ? "TX" : "RX";
  doc["band"] = band;

  bool blockActive = (millis() - lastBandChangeTime <= bandBlockTimeMs);
  doc["block"] = blockActive;
  if (blockActive) {
    doc["block_ms_left"] = bandBlockTimeMs - (millis() - lastBandChangeTime);
  } else {
    doc["block_ms_left"] = 0;
  }

  char payload[256];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  client.publish(topic_status, payload, n);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Currently no commands; just log unknown messages
  static char msg[MQTT_MAX_PACKET_SIZE];

  if (length >= sizeof(msg)) length = sizeof(msg) - 1;
  memcpy(msg, payload, length);
  msg[length] = '\0';

  char dbg[200];
  snprintf(dbg, sizeof(dbg),
           "[MQTT] message on %s: %s", topic, msg);
  publishDebugMessage(dbg);
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (client.connected()) return;

  Serial.println("Attempting MQTT connection...");
  char clientId[64];
  snprintf(clientId, sizeof(clientId), "Amp-%s", macAddress[0] ? macAddress : "Unknown");

  if (client.connect(clientId)) {
    publishDebugMessage("MQTT connected.");
    // We only publish status; no subscriptions needed yet.
    publishAmpStatus();
  } else {
    Serial.printf("MQTT connect failed, rc=%d\n", client.state());
  }
}

// -------------------------------------------------------------------
//  Band / PTT logic
// -------------------------------------------------------------------

// Read 4-bit band code from digital pins, HIGH = 1, LOW = 0
uint8_t readBandCode() {
  uint8_t value = 0;
  for (int i = 0; i < 4; i++) {
    int val = digitalRead(bandInPins[i]);
    if (val == HIGH) {
      value |= (1 << i);
    }
  }
  return (value & Mask);
}

// Apply band to 4 output pins according to original mapping
void applyBandOutputs(uint8_t b) {
  // Pattern bits: bit0 -> OUT0, bit1 -> OUT1, bit2 -> OUT2, bit3 -> OUT3
  uint8_t pattern;
  switch (b) {
    case 15:  // 1111
      pattern = 0b1111;
      break;
    case 14:  // 0000
      pattern = 0b0000;
      break;
    case 13:  // D2
      pattern = 0b0001;
      break;
    case 12:  // D3
      pattern = 0b0010;
      break;
    case 11:  // D2 + D3
      pattern = 0b0011;
      break;
    case 10:  // D4
      pattern = 0b0100;
      break;
    case 9:   // D2 + D4
      pattern = 0b0101;
      break;
    case 8:   // D3 + D4
      pattern = 0b0110;
      break;
    case 7:   // D2 + D3 + D4
      pattern = 0b0111;
      break;
    case 6:   // D5
      pattern = 0b1000;
      break;
    case 5:   // D2 + D5
      pattern = 0b1001;
      break;
    default:  // unknown -> all ON
      pattern = 0b1111;
      break;
  }

  for (int i = 0; i < 4; i++) {
    digitalWrite(bandOutPins[i], (pattern & (1 << i)) ? HIGH : LOW);
  }

  char dbg[100];
  snprintf(dbg, sizeof(dbg),
           "[BAND] code=%u -> pattern=0b%04b", b, pattern);
  publishDebugMessage(dbg);
}

void bandControl() {
  uint8_t newBand = readBandCode();

  if (newBand == bandOld) {
    // unchanged; optional debug (comment out if too chatty)
    // Serial.print("Band unchanged, code = ");
    // Serial.println(newBand);
    band = newBand;
    return;
  }

  // Band changed
  band    = newBand;
  uint8_t prev = bandOld;
  bandOld = newBand;

  char dbg[80];
  snprintf(dbg, sizeof(dbg),
           "[BAND] change detected: %u -> %u", prev, newBand);
  publishDebugMessage(dbg);

  // Start PTT block timer
  lastBandChangeTime = millis();
  snprintf(dbg, sizeof(dbg),
           "[BAND] Blocking PTT for %d s", block_ptt_seconds);
  publishDebugMessage(dbg);

  // Update outputs
  applyBandOutputs(newBand);

  // Publish status over MQTT
  publishAmpStatus();
}

// -------------------------------------------------------------------
//  Amplifier control wrapper (called from loop())
// -------------------------------------------------------------------
void handleAmplifierLogic() {
  int in_ptt = digitalRead(pttInp);
  int in_ugl = digitalRead(uglasevanje); // currently unused, but read

  unsigned long now = millis();
  bool blockActive  = (now - lastBandChangeTime <= bandBlockTimeMs);
  bool pttAllowed   = !blockActive;

  // --- handle PTT state -------------------------------------------
  bool pttPressed = (in_ptt == LOW);   // active LOW
  bool prevPtt    = pttState;

  if (pttPressed) {
    // PTT pressed
    if (pttAllowed) {
      if (!pttState) {
        publishDebugMessage("[PTT] IN active and allowed -> TX ON");
      }
      digitalWrite(pttOut, HIGH);
      pttState = true;
      pttBlockReported = false;
    } else {
      if (!pttBlockReported) {
        char dbg[80];
        snprintf(dbg, sizeof(dbg),
                 "[PTT] IN active BUT BLOCKED (%d s after band change)",
                 block_ptt_seconds);
        publishDebugMessage(dbg);
        pttBlockReported = true;
      }
      digitalWrite(pttOut, LOW);
      pttState = false;
    }
    // No bandControl() while PTT pressed
  } else {
    // PTT released
    if (pttState) {
      publishDebugMessage("[PTT] IN released -> TX OFF");
    }
    digitalWrite(pttOut, LOW);
    pttState = false;
    pttBlockReported = false;

    // Allow band changes only when PTT not active
    bandControl();
  }

  // If PTT state changed, publish status
  if (pttState != prevPtt) {
    publishAmpStatus();
  }

  // Optional: log uglasevanje
  if (in_ugl == LOW) {
    publishDebugMessage("[INFO] Uglasevanje input LOW (active)");
  }
}

// -------------------------------------------------------------------
//  Setup & loop
// -------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("Booting amplifier controller..."));
  Serial.println(VER);
  debugDeadline += millis();
  Serial.println(F("Debug enabled for 1 minute after boot"));

  // Load config
  loadConfigFromEEPROM();

  // WiFi
  setupWiFi();

  // Web + OTA
  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.on("/update", HTTP_GET, handleUpdate);
  httpUpdater.setup(&server);
  server.begin();
  Serial.println("HTTP server started");

  // MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  // Amplifier pins
  pinMode(pttOut, OUTPUT);
  pinMode(pttInp, INPUT_PULLUP);
  pinMode(uglasevanje, INPUT_PULLUP);

  for (int i = 0; i < 4; i++) {
    pinMode(bandInPins[i], INPUT_PULLUP);
    pinMode(bandOutPins[i], OUTPUT);
    digitalWrite(bandOutPins[i], LOW);
  }

  // Initial band read (no block)
  bandOld = readBandCode();
  band    = bandOld;
  applyBandOutputs(band);

  Serial.print("Initial band code: ");
  Serial.println(band);
  Serial.print("Initial PTT block time: ");
  Serial.print(block_ptt_seconds);
  Serial.println(" s");
}

void loop() {
  // Handle amplifier logic
  handleAmplifierLogic();

  // Auto-disable debug after first minute
  if (debugEnabled && debugDeadline && millis() >= debugDeadline) {
    debugEnabled  = false;
    debugDeadline = 0;
    Serial.println(F("Debug messages DISABLED (1-minute timeout)"));
  }

  // Web + MQTT
  server.handleClient();
  client.loop();

  // WiFi / MQTT health & reconnect
  static unsigned long lastMQTTCheck = 0;
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED && WiFi.getMode() != WIFI_AP) {
    // lost STA, but not already in AP
    publishDebugMessage("WiFi disconnected.");
  }

  if (!client.connected() && (now - lastMQTTCheck > 5000)) {
    lastMQTTCheck = now;
    reconnectMQTT();
  }

  delay(5);  // small relief
}
