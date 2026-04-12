/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║   SmartPark IoT — ESP32 Final Version                        ║
 * ║   Keypad PIN + I2C LCD + HC-SR04 + Servo + 4 LEDs            ║
 * ╠══════════════════════════════════════════════════════════════╣
 * ║  LED ROLES (CORRECTED):                                      ║
 * ║   GATE GREEN (GPIO 25) — lights when gate is OPEN / PIN OK   ║
 * ║   GATE RED   (GPIO 26) — lights when gate is LOCKED          ║
 * ║   SLOT GREEN (GPIO 32) — lights when parking slot is VACANT  ║
 * ║   SLOT RED   (GPIO 33) — lights when car IS in the slot      ║
 * ║                                                              ║
 * ║  SERVO (GPIO 13) — main entry gate arm                       ║
 * ║  HC-SR04 (TRIG 14 / ECHO 12) — slot occupancy sensor         ║
 * ║  KEYPAD ROWS  : GPIO 4, 5, 15, 16                            ║
 * ║  KEYPAD COLS  : GPIO 17, 18, 19, 23                          ║
 * ║  LCD I2C      : SDA=GPIO21  SCL=GPIO22                       ║
 * ║                                                              ║
 * ║  PIN SYSTEM:                                                 ║
 * ║   Each booking on the website generates a unique 4-digit PIN ║
 * ║   ESP32 fetches the active PIN from your server via HTTP     ║
 * ║   On correct PIN + # → gate opens 4 sec → gate closes        ║
 * ║   After customer leaves → server sets new PIN for next user  ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * REQUIRED LIBRARIES (Arduino Library Manager):
 *   ESP32Servo    — by Kevin Harrington
 *   Keypad        — by Mark Stanley & Alexander Brevig
 *   LiquidCrystal I2C — by Frank de Brabander
 *   ArduinoJson   — by Benoit Blanchon
 *   WiFi.h        — built-in with ESP32 board package
 *   HTTPClient.h  — built-in with ESP32 board package
 */

#include <ESP32Servo.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ══════════════════════════════════════════════════════
//  CONFIGURATION — CHANGE THESE
// ══════════════════════════════════════════════════════
const char* WIFI_SSID     = "Wokwi-GUEST";       // Your Wi-Fi SSID
const char* WIFI_PASS     = "";   // Your Wi-Fi password

// Your backend server URL (see connection guide below)
// Example with local server:  "http://192.168.1.100:3000"
// Example with Firebase:      "https://your-project.firebaseio.com"
const char* SERVER_URL    = "https://smartpark-server-yczi.onrender.com";

// This device's unique ID — register this in the website when adding your lot
const char* DEVICE_ID     = "SP-0001";

// Fallback PIN (used when Wi-Fi/server is unavailable for testing)
const char* FALLBACK_PIN  = "1234";

// Car detection threshold
const int   CAR_DIST_CM   = 15;   // < 15cm = car present
const int   GATE_OPEN_SEC = 4;    // Gate open duration in seconds
const int   PIN_FETCH_MS  = 5000; // How often to fetch PIN from server (ms)
const int   STATUS_PUSH_MS= 3000; // How often to push slot status (ms)
const int   LCD_I2C_ADDR  = 0x27;

// ══════════════════════════════════════════════════════
//  PIN NUMBERS
// ══════════════════════════════════════════════════════
#define TRIG_PIN      14
#define ECHO_PIN      12

#define LED_GATE_GREEN 25   // Gate: green = OPEN / authorized
#define LED_GATE_RED   2   // Gate: red   = LOCKED / closed
#define LED_SLOT_GREEN 32   // Slot: green = VACANT
#define LED_SLOT_RED   33   // Slot: red   = OCCUPIED

#define SERVO_PIN      13

// 4x4 Keypad
const byte ROWS = 4, COLS = 4;
char keyLayout[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {4, 5, 15, 16};
byte colPins[COLS] = {17, 18, 19, 23};

// ══════════════════════════════════════════════════════
//  OBJECTS
// ══════════════════════════════════════════════════════
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);
Keypad   keypad  = Keypad(makeKeymap(keyLayout), rowPins, colPins, ROWS, COLS);
Servo    gateServo;
HTTPClient http;

// ══════════════════════════════════════════════════════
//  STATE
// ══════════════════════════════════════════════════════
String  enteredPIN    = "";
String  currentPIN    = FALLBACK_PIN; // active PIN from server
bool    slotOccupied  = false;
bool    gateIsOpen    = false;
bool    wifiConnected = false;
unsigned long lastPINFetch   = 0;
unsigned long lastStatusPush = 0;

// ══════════════════════════════════════════════════════
//  HELPER: Measure distance in cm
// ══════════════════════════════════════════════════════
long measureCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long dur = pulseIn(ECHO_PIN, HIGH, 25000);
  if (dur == 0) return 999;
  return (dur * 34L) / 2000;
}

// ══════════════════════════════════════════════════════
//  HELPER: Set GATE LEDs
// ══════════════════════════════════════════════════════
void setGateLED(bool isOpen) {
  digitalWrite(LED_GATE_GREEN, isOpen ? HIGH : LOW);  // green = open
  digitalWrite(LED_GATE_RED,   isOpen ? LOW  : HIGH); // red   = locked
}

// ══════════════════════════════════════════════════════
//  HELPER: Set SLOT LEDs
// ══════════════════════════════════════════════════════
void setSlotLED(bool isCar) {
  digitalWrite(LED_SLOT_GREEN, isCar ? LOW  : HIGH); // green = vacant
  digitalWrite(LED_SLOT_RED,   isCar ? HIGH : LOW);  // red   = occupied
}

// ══════════════════════════════════════════════════════
//  HELPER: Open gate, wait, close
// ══════════════════════════════════════════════════════
void openGate() {
  gateIsOpen = true;
  gateServo.write(90);
  setGateLED(true);  // gate LED → green

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  ACCESS OK!    ");

  Serial.println("[GATE] Open — 90 degrees");

  for (int s = GATE_OPEN_SEC; s > 0; s--) {
    lcd.setCursor(0, 1);
    lcd.print("  Closing in ");
    lcd.print(s); lcd.print("s  ");
    delay(1000);
  }

  gateServo.write(0);
  gateIsOpen = false;
  digitalWrite(LED_GATE_GREEN, LOW);  // right GREEN off after close
  digitalWrite(LED_GATE_RED, LOW);    // right RED stays OFF

  Serial.println("[GATE] Closed — 0 degrees");

  // After gate closes, tell server this booking is done
  // so a NEW PIN is needed for the next customer
  pushGateEvent("closed");

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  SmartPark     ");
  lcd.setCursor(0, 1); lcd.print("  Enter PIN + # ");
}

// ══════════════════════════════════════════════════════
//  HELPER: Update PIN display row
// ══════════════════════════════════════════════════════
void refreshPINRow() {
  lcd.setCursor(0, 1);
  lcd.print("PIN:");
  for (int i = 0; i < (int)enteredPIN.length(); i++) lcd.print("*");
  for (int i = enteredPIN.length(); i < 4; i++) lcd.print("_");
  lcd.print("       "); // Clear trailing chars
}

// ══════════════════════════════════════════════════════
//  NETWORK: Fetch active PIN from server
//  GET /api/pin?device=SP-0001
//  Server returns: {"pin":"7284","booking_id":"BK001"}
// ══════════════════════════════════════════════════════
void fetchPIN() {
  if (!wifiConnected) return;
  String url = String(SERVER_URL) + "/api/pin?device=" + DEVICE_ID;
  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    StaticJsonDocument<128> doc;
    if (!deserializeJson(doc, body)) {
      String newPin = doc["pin"].as<String>();
      if (newPin.length() == 4) {
        if (newPin != currentPIN) {
          currentPIN = newPin;
          Serial.println("[PIN] Updated from server: ****");
        }
      }
    }
  } else {
    Serial.print("[PIN] Fetch failed, code: "); Serial.println(code);
  }
  http.end();
}

// ══════════════════════════════════════════════════════
//  NETWORK: Push slot status to server
//  POST /api/status
//  Body: {"device":"SP-0001","occupied":true,"dist":8}
// ══════════════════════════════════════════════════════
void pushStatus(bool occupied, long dist) {
  if (!wifiConnected) return;
  String url = String(SERVER_URL) + "/api/status";
  String body = "{\"device\":\"" + String(DEVICE_ID) +
                "\",\"occupied\":" + (occupied ? "true" : "false") +
                ",\"dist\":" + dist + "}";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  Serial.print("[STATUS] Pushed — code: "); Serial.println(code);
  http.end();
}

// ══════════════════════════════════════════════════════
//  NETWORK: Tell server gate opened/closed
//  POST /api/gate-event
// ══════════════════════════════════════════════════════
void pushGateEvent(String event) {
  if (!wifiConnected) return;
  String url = String(SERVER_URL) + "/api/gate-event";
  String body = "{\"device\":\"" + String(DEVICE_ID) +
                "\",\"event\":\"" + event + "\"}";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.POST(body);
  http.end();
}

// ══════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println(F("\n╔══════════════════════════════════╗"));
  Serial.println(F("║   SmartPark ESP32 — Starting...  ║"));
  Serial.println(F("╚══════════════════════════════════╝"));

  // I/O Setup
  pinMode(TRIG_PIN,      OUTPUT);
  pinMode(ECHO_PIN,      INPUT);
  pinMode(LED_GATE_GREEN,OUTPUT);
  pinMode(LED_GATE_RED,  OUTPUT);
  pinMode(LED_SLOT_GREEN,OUTPUT);
  pinMode(LED_SLOT_RED,  OUTPUT);

  // Servo
  gateServo.attach(SERVO_PIN);
  gateServo.write(0); // Gate starts CLOSED

  // Initial LED state: both right LEDs OFF, slot VACANT (green)
  digitalWrite(LED_GATE_GREEN, LOW);
  digitalWrite(LED_GATE_RED, LOW);
  setSlotLED(false);

  // LCD
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("  SmartPark     ");
  lcd.setCursor(0, 1); lcd.print("  Booting...    ");

  // Flash all LEDs 3× as boot animation
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_GATE_GREEN, HIGH); digitalWrite(LED_GATE_RED,  HIGH);
    digitalWrite(LED_SLOT_GREEN, HIGH); digitalWrite(LED_SLOT_RED,  HIGH);
    delay(180);
    digitalWrite(LED_GATE_GREEN, LOW);  digitalWrite(LED_GATE_RED,  LOW);
    digitalWrite(LED_SLOT_GREEN, LOW);  digitalWrite(LED_SLOT_RED,  LOW);
    delay(180);
  }
  // Restore correct state
  digitalWrite(LED_GATE_GREEN, LOW);
  digitalWrite(LED_GATE_RED, LOW);
  setSlotLED(false);

  // Wi-Fi
  lcd.setCursor(0, 1); lcd.print("  WiFi...       ");
  Serial.print("[WiFi] Connecting to "); Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500); Serial.print("."); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println(F("\n[WiFi] Connected!"));
    Serial.print(F("[WiFi] IP: ")); Serial.println(WiFi.localIP());
    fetchPIN();
  } else {
    Serial.println(F("\n[WiFi] Failed — using fallback PIN"));
    currentPIN = FALLBACK_PIN;
  }

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("  SmartPark     ");
  lcd.setCursor(0, 1); lcd.print("  Enter PIN + # ");

  Serial.println(F("[SYS] Ready!"));
  Serial.println(F("  Type PIN on keypad, press # to open gate"));
  Serial.println(F("  Press * to clear PIN"));
  Serial.println(F("══════════════════════════════════"));
}

// ══════════════════════════════════════════════════════
//  MAIN LOOP
// ══════════════════════════════════════════════════════
void loop() {

  // ── 1. Sensor reading ──────────────────────────────
  long dist = measureCm();
  bool car  = (dist < CAR_DIST_CM);

  if (car != slotOccupied) {
    slotOccupied = car;
    setSlotLED(car);

    // Update LCD top row
    lcd.setCursor(0, 0);
    lcd.print(car ? "Slot: OCCUPIED  " : "Slot: VACANT    ");

    Serial.print(F("[SLOT] "));
    Serial.print(car ? "CAR IN " : "EMPTY  ");
    Serial.print(F("("));
    Serial.print(dist);
    Serial.println(F(" cm)"));
  }

  // ── 2. Periodic network tasks ───────────────────────
  unsigned long now = millis();

  if (now - lastPINFetch > PIN_FETCH_MS) {
    lastPINFetch = now;
    fetchPIN();
  }

  if (now - lastStatusPush > STATUS_PUSH_MS) {
    lastStatusPush = now;
    pushStatus(slotOccupied, dist);
  }

  // ── 3. Keypad input ─────────────────────────────────
  char key = keypad.getKey();
  if (!key) { delay(60); return; }

  Serial.print(F("[KEY] ")); Serial.println(key);

  if (key == '*') {
    // ── CLEAR ─────────────────────────────────────────
    enteredPIN = "";
    refreshPINRow();
    Serial.println(F("[PIN] Cleared"));

  } else if (key == '#') {
    // ── CHECK PIN ─────────────────────────────────────
    if (enteredPIN == currentPIN) {

      if (!slotOccupied) {
        // ✅ Correct PIN + slot is free
        Serial.println(F("[AUTH] CORRECT — opening gate!"));
        pushGateEvent("opened");
        openGate();
        enteredPIN = "";
        // Fetch fresh PIN (server may have issued new one)
        fetchPIN();
      } else {
        // ✅ Correct PIN but slot is full
        Serial.println(F("[AUTH] Correct PIN but slot OCCUPIED!"));
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print(" Slot OCCUPIED! ");
        lcd.setCursor(0, 1); lcd.print(" Wait for space ");
        delay(2500);
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("  SmartPark     ");
        enteredPIN = "";
        refreshPINRow();
      }

    } else {
      // ❌ Wrong PIN
      Serial.println(F("[AUTH] WRONG PIN!"));
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print("  WRONG PIN!    ");
      lcd.setCursor(0, 1); lcd.print("  Try again...  ");

      // Flash gate RED LED twice as warning
      // Right RED glows solid during wrong PIN
      digitalWrite(LED_GATE_RED, HIGH);   // right RED ON
      delay(2000);                        // solid for 2 seconds
      digitalWrite(LED_GATE_RED, LOW);    // right RED OFF — done
      lcd.clear();
      lcd.setCursor(0, 0); lcd.print(slotOccupied ? "Slot: OCCUPIED  " : "Slot: VACANT    ");
      enteredPIN = "";
      refreshPINRow();
    }

  } else if (key >= '0' && key <= '9') {
    // ── DIGIT ─────────────────────────────────────────
    if ((int)enteredPIN.length() < 4) {
      enteredPIN += key;
      // Show slot status on top row, PIN on bottom row
      lcd.setCursor(0, 0);
      lcd.print(slotOccupied ? "Slot: OCCUPIED  " : "Slot: VACANT    ");
      refreshPINRow();
      Serial.print(F("[PIN] Digits entered: "));
      Serial.println(enteredPIN.length());
    } else {
      lcd.setCursor(0, 1); lcd.print("Max 4 digits!   ");
      delay(600); refreshPINRow();
    }

  }
  // A/B/C/D keys are ignored silently

  delay(60);
}

/*
 * ══════════════════════════════════════════════════════
 *  MINIMAL NODE.JS SERVER (backend/server.js)
 *  Run with: node server.js
 *  Install:  npm install express
 * ══════════════════════════════════════════════════════
 *
 * const express = require('express');
 * const app = express();
 * app.use(express.json());
 *
 * // In-memory store (use a real DB like MongoDB/Firebase in production)
 * const devices = {
 *   'SP-0001': { pin: '1234', booking: null, occupied: false },
 * };
 *
 * // GET /api/pin?device=SP-0001
 * // ESP32 calls this every 5 seconds to get the current valid PIN
 * app.get('/api/pin', (req, res) => {
 *   const d = devices[req.query.device];
 *   if (!d) return res.status(404).json({ error: 'Device not found' });
 *   res.json({ pin: d.pin, booking_id: d.booking });
 * });
 *
 * // POST /api/status  (body: {device, occupied, dist})
 * // ESP32 pushes slot sensor status every 3 seconds
 * app.post('/api/status', (req, res) => {
 *   const { device, occupied } = req.body;
 *   if (devices[device]) devices[device].occupied = occupied;
 *   res.json({ ok: true });
 *   // TODO: emit to website via Socket.io for live map updates
 * });
 *
 * // POST /api/gate-event  (body: {device, event: "opened"|"closed"})
 * app.post('/api/gate-event', (req, res) => {
 *   const { device, event } = req.body;
 *   if (event === 'closed' && devices[device]) {
 *     // When gate closes: generate new PIN for next customer
 *     devices[device].pin = String(Math.floor(1000 + Math.random() * 9000));
 *     devices[device].booking = null;
 *     console.log(`[${device}] New PIN issued: ****`);
 *   }
 *   res.json({ ok: true });
 * });
 *
 * // POST /api/booking  (called by website when user books)
 * // Body: {device, pin, booking_id}
 * app.post('/api/booking', (req, res) => {
 *   const { device, pin, booking_id } = req.body;
 *   if (!devices[device]) return res.status(404).json({ error: 'Device not found' });
 *   devices[device].pin = pin;
 *   devices[device].booking = booking_id;
 *   res.json({ ok: true });
 * });
 *
 * app.listen(3000, () => console.log('SmartPark server running on :3000'));
 */