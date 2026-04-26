#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>

//Insert Wifi library
#include <WiFi.h>
#include <HTTPClient.h>
#include <stdio.h>
#include <string.h>
#include <Keypad.h>

/** Declared before any function so Arduino's auto-prototypes do not reference an unknown type. */
struct TableButtonDebouncer {
  uint8_t phase; /* 0 = armed (released), 1 = latched until full release */
  unsigned long mark;
};

// Replace with your network credentials (STATION)
#define ssid "Lin-2.4GHz"
#define password "Thet3470"

// Django REST API: use this PC's Wi-Fi IPv4 (ipconfig). Server must listen on all interfaces:
//   manage.py runserver 0.0.0.0:8001
// (127.0.0.1-only runserver is unreachable from the ESP32 on the LAN.)
// If the board uses Host-Only / VM network, try 192.168.56.1 instead.
#define API_HOST "192.168.0.96"
#define API_PORT 8001
// Full path pattern: http://API_HOST:API_PORT/api/iot/table-status/<table_number>/

// Django Table.table_number (not DB row id). Must match the first four entries from
// ``reload_frontend_demo`` / ``capstone-frontend/lib/data/admin-tables-mock.ts``:
// floor 1 scatter coords (40,45)→1, (200,38)→2, (380,62)→3, (560,44)→4.
#define TABLE_NUM_ST1 1
#define TABLE_NUM_ST2 2
#define TABLE_NUM_RT1 3
#define TABLE_NUM_GT1 4

/** Keypad OTP entry length (must match server reservation OTP). */
#define KEYPAD_PIN_LIMIT 6

#define ST1_DEBOUNCE_MS 600
#define REMOTE_POLL_MS 2000
/** GET on boot (first sync): generous for flaky Wi-Fi. */
#define HTTP_GET_BOOT_TIMEOUT_MS 3500
/** GET during periodic poll: short so one request does not stall the sketch for seconds. */
#define HTTP_GET_POLL_TIMEOUT_MS 1200
/** POST from deferred queue (after LED already updated): one quick try per loop visit. */
#define HTTP_POST_DEFERRED_TIMEOUT_MS 900
#define HTTP_POST_DEFERRED_ATTEMPTS 1
/**
 * HX711 + dual OLED + HTTP poll are slow; run them on this interval so the loop still
 * spins fast enough for button debouncing (stable LOW/HIGH ms) to feel responsive.
 */
#define SLOW_LOOP_PERIOD_MS 40
/** LOW must be stable this long (ms) before we count one press (filters chatter). */
#define BTN_STABLE_LOW_MS 45
/** HIGH (released) must be stable this long before the next press can register. */
#define BTN_STABLE_HIGH_MS 50
/** After POST returns, block GET poll this long so DB is not read back too early. */
#define BTN_POLL_SUPPRESS_MS 1600

/** IoT: 0=unavailable, 1=available, 2=reserved. API/DB: 1=free, 2=occupied, 3=reserved. */
static int apiStatusFromIot(int iot) {
  switch (iot) {
    case 0: return 2;
    case 1: return 1;
    case 2: return 3;
    default: return 1;
  }
}

static int iotStatusFromApi(int api) {
  if (api == 2) return 0;
  if (api == 3) return 2;
  if (api == 1) return 1;
  return 1;
}

static String makeTableStatusUrl(int tableNumber) {
  return String("http://") + API_HOST + ":" + String(API_PORT) +
         "/api/iot/table-status/" + String(tableNumber) + "/";
}

/** ST1 OLED: live reservation end (HH:MM:SS) from Django; default until first GET. */
static char st1BookingEndsLocal[16] = "--:--:--";
/** Library-local HH:MM for the booking window shown on OLED (from weight-availability). */
static char st1BookingStartsLocal[8] = "";
static char st1BookingWindowEndLocal[8] = "";
static bool st1OtpVerified = false;
static bool g_st1LastInSeat = false;
#define ST1_OTP_FEEDBACK_MS 2200
static unsigned long st1FeedbackUntil = 0;
static uint8_t st1FeedbackKind = 0; /* 0 none, 1 ok, 2 bad */
static volatile bool g_st1OtpSubmitPending = false;
static String keypadInputBuffer;

static String makeSt1WeightAvailabilityUrl() {
  return String("http://") + API_HOST + ":" + String(API_PORT) +
         "/api/public/tables/1/weight-availability/";
}

static String makeSt1VerifyOtpUrl() {
  return String("http://") + API_HOST + ":" + String(API_PORT) +
         "/api/iot/tables/" + String(TABLE_NUM_ST1) + "/verify-reservation-otp/";
}

/** After ``"key":`` — copy quoted string into ``out``, or clear on JSON null. */
static void parseJsonStringField(const String& body, const char* key, char* out, size_t outSz) {
  String needle = String("\"") + key + "\":";
  int i = body.indexOf(needle);
  if (i < 0) {
    if (outSz > 0) out[0] = '\0';
    return;
  }
  i += needle.length();
  while (i < (int)body.length() && (body.charAt(i) == ' ' || body.charAt(i) == '\n')) i++;
  if (i < (int)body.length() && body.charAt(i) == 'n') {
    if (outSz > 0) out[0] = '\0';
    return;
  }
  if (i >= (int)body.length() || body.charAt(i) != '"') {
    if (outSz > 0) out[0] = '\0';
    return;
  }
  i++;
  int j = 0;
  while (i < (int)body.length() && body.charAt(i) != '"' && j < (int)outSz - 1) {
    out[j++] = (char)body.charAt(i++);
  }
  out[j] = '\0';
}

static bool parseJsonBoolTrue(const String& body, const char* key) {
  String needle = String("\"") + key + "\":";
  int i = body.indexOf(needle);
  if (i < 0) return false;
  i += needle.length();
  while (i < (int)body.length() && body.charAt(i) == ' ') i++;
  return i < (int)body.length() && body.charAt(i) == 't';
}

static bool st1HasBookingWindowForUi() {
  return st1BookingStartsLocal[0] != '\0' && st1BookingWindowEndLocal[0] != '\0';
}

static bool st1ShouldSuppressFreeStatusPost() {
  if (!st1HasBookingWindowForUi()) return false;
  if (st1OtpVerified) return false;
  return true;
}

static bool postVerifyOtpToApi() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (keypadInputBuffer.length() != KEYPAD_PIN_LIMIT) return false;
  HTTPClient http;
  String url = makeSt1VerifyOtpUrl();
  http.setTimeout(HTTP_GET_BOOT_TIMEOUT_MS);
  if (!http.begin(url)) {
    Serial.println("st1 verify: http.begin failed");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"otp\":\"") + keypadInputBuffer + "\"}";
  int code = http.POST(body);
  http.end();
  Serial.printf("st1 verify POST code=%d\n", code);
  return code >= 200 && code < 300;
}

static void fetchSt1WeightAvailabilityFromApi() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = makeSt1WeightAvailabilityUrl();
  http.setTimeout(HTTP_GET_POLL_TIMEOUT_MS);
  if (!http.begin(url)) {
    Serial.println("st1 avail: http.begin failed");
    return;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("st1 avail GET code=%d\n", code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  bool hasSensor = true;
  if (body.indexOf("\"has_weight_sensor\":") >= 0) {
    hasSensor = parseJsonBoolTrue(body, "has_weight_sensor");
  }
  if (!hasSensor) {
    st1BookingStartsLocal[0] = '\0';
    st1BookingWindowEndLocal[0] = '\0';
    st1OtpVerified = false;
    strncpy(st1BookingEndsLocal, "--:--:--", sizeof(st1BookingEndsLocal));
    st1BookingEndsLocal[sizeof(st1BookingEndsLocal) - 1] = '\0';
    return;
  }

  parseJsonStringField(body, "current_booking_starts_local", st1BookingStartsLocal,
                       sizeof(st1BookingStartsLocal));
  parseJsonStringField(body, "current_booking_window_end_local", st1BookingWindowEndLocal,
                       sizeof(st1BookingWindowEndLocal));
  st1OtpVerified = parseJsonBoolTrue(body, "otp_verified");

  char endsBuf[sizeof(st1BookingEndsLocal)];
  parseJsonStringField(body, "current_booking_ends_local", endsBuf, sizeof(endsBuf));
  if (endsBuf[0] == '\0') {
    strncpy(st1BookingEndsLocal, "--:--:--", sizeof(st1BookingEndsLocal));
    st1BookingEndsLocal[sizeof(st1BookingEndsLocal) - 1] = '\0';
  } else {
    strncpy(st1BookingEndsLocal, endsBuf, sizeof(st1BookingEndsLocal));
    st1BookingEndsLocal[sizeof(st1BookingEndsLocal) - 1] = '\0';
  }
}

/** Parse JSON body like {"status":2}. Returns -1 on failure. */
static int parseStatusJson(const String& body) {
  int i = body.indexOf("status");
  if (i < 0) return -1;
  i = body.indexOf(':', i);
  if (i < 0) return -1;
  i++;
  while (i < (int)body.length() && (body.charAt(i) == ' ' || body.charAt(i) == '"')) i++;
  if (i >= (int)body.length()) return -1;
  long v = 0;
  bool any = false;
  while (i < (int)body.length() && body.charAt(i) >= '0' && body.charAt(i) <= '9') {
    any = true;
    v = v * 10 + (body.charAt(i) - '0');
    i++;
  }
  if (!any) return -1;
  return (int)v;
}

/** GET -> IoT state 0..2; on error returns defaultIot. */
static int fetchStatusFromApi(int tableNumber, int defaultIot, uint32_t getTimeoutMs) {
  if (WiFi.status() != WL_CONNECTED) return defaultIot;
  HTTPClient http;
  String url = makeTableStatusUrl(tableNumber);
  http.setTimeout(getTimeoutMs);
  if (!http.begin(url)) {
    Serial.println("http.begin failed");
    return defaultIot;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("GET %s code=%d\n", url.c_str(), code);
    http.end();
    return defaultIot;
  }
  String body = http.getString();
  http.end();
  int raw = parseStatusJson(body);
  if (raw < 0) {
    Serial.println("parseStatusJson failed: " + body);
    return defaultIot;
  }
  return iotStatusFromApi(raw);
}

static bool postStatusToApiWithConfig(
    int tableNumber, int iotState, uint32_t timeoutMs, int maxAttempts) {
  if (WiFi.status() != WL_CONNECTED) return false;
  int apiVal = apiStatusFromIot(iotState);
  char jsonBuf[48];
  snprintf(jsonBuf, sizeof(jsonBuf), "{\"status\":%d}", apiVal);
  String body(jsonBuf);
  String url = makeTableStatusUrl(tableNumber);
  for (int attempt = 0; attempt < maxAttempts; attempt++) {
    if (attempt > 0) delay(80);
    HTTPClient http;
    http.setTimeout(timeoutMs);
    if (!http.begin(url)) {
      Serial.println("post: http.begin failed");
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body);
    http.end();
    if (code >= 200 && code < 300) {
      Serial.printf("POST ok table=%d iot=%d api=%d\n", tableNumber, iotState, apiVal);
      return true;
    }
    Serial.printf("POST try %d %s code=%d\n", attempt + 1, url.c_str(), code);
  }
  return false;
}

/** Deferred POST (single short attempt) — used so buttons never block on HTTP. */
static bool postStatusToApiDeferred(int tableNumber, int iotState) {
  return postStatusToApiWithConfig(
      tableNumber, iotState, HTTP_POST_DEFERRED_TIMEOUT_MS, HTTP_POST_DEFERRED_ATTEMPTS);
}

/** Latest table status the firmware wants Django to reflect (0 = none pending). */
static int g_postPendingTable = 0;
static int g_postPendingIot = 0;

static void scheduleStatusPost(int tableNumber, int iotState) {
  g_postPendingTable = tableNumber;
  g_postPendingIot = iotState;
}

/** Try one deferred POST per slow-path visit (LED/state already updated). */
static void drainPendingStatusPost() {
  if (g_postPendingTable == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (postStatusToApiDeferred(g_postPendingTable, g_postPendingIot)) {
    g_postPendingTable = 0;
  }
}

static const char* iotStatusLabel(int s) {
  if (s == 0) return "Unavailable";
  if (s == 2) return "Reserved";
  return "Available";
}

static void setRgbForState(int pinR, int pinG, int pinB, int iotState) {
  digitalWrite(pinR, iotState == 0 ? HIGH : LOW);
  digitalWrite(pinG, iotState == 1 ? HIGH : LOW);
  digitalWrite(pinB, iotState == 2 ? HIGH : LOW);
}

/** Returns true once when LOW has been stable long enough; ignores bounce until HIGH release. */
static bool consumeStableTablePress(int pin, TableButtonDebouncer& d, unsigned long now) {
  const bool low = digitalRead(pin) == LOW;
  if (d.phase == 0) {
    if (low) {
      if (d.mark == 0) d.mark = now;
      if (now - d.mark >= (unsigned long)BTN_STABLE_LOW_MS) {
        d.phase = 1;
        d.mark = 0;
        return true;
      }
    } else {
      d.mark = 0;
    }
  } else {
    if (!low) {
      if (d.mark == 0) d.mark = now;
      if (now - d.mark >= (unsigned long)BTN_STABLE_HIGH_MS) {
        d.phase = 0;
        d.mark = 0;
      }
    } else {
      d.mark = 0;
    }
  }
  return false;
}

void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  // WiFi.begin("Wokwi-GUEST", "");
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println(WiFi.status());
    Serial.print('.');
    delay(1000);
  }
  Serial.println("Connected");
  Serial.println(WiFi.status());
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("RRSI: ");
  Serial.println(WiFi.RSSI());
}

//insert OLED screen library
#include <Adafruit_SSD1306.h>

//Initialize OLED
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels

#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Select I2C BUS
void TCA9548A(uint8_t bus){
  Wire.beginTransmission(0x70);  // TCA9548A address, because pin A0 A1 A2 are 0 (go to ground)
  Wire.write(1 << bus);          // send byte to select bus
  Wire.endTransmission();
  Serial.print(bus);
}

//variables for centering OLED display
byte length, width;
int16_t  x, y;
uint16_t w, h;

//insert HX711 library
#include "HX711.h"

//initialize HX711 object
// HX711 circuit wiring
const int LOADCELL_DOUT_PIN = 10;  // ST1 DT: IO10
const int LOADCELL_SCK_PIN = 11;  // ST1 SCK: IO11
HX711 scale;
float calibration_factor = -1365; // for me this vlaue works just perfect 419640


//Variable will not change for
const int ST2buttonPin = 38;  // ST2 button: IO38
const int RT1buttonPin = 19;   // RT1 button: IO7
const int GT1buttonPin = 18;  // GT1 button: IO18

const int ST2ledPinR =  14;   // ST2 R: IO14
const int ST2ledPinG =  13;   // ST2 G: IO13
const int ST2ledPinB =  12;   // ST2 B: IO12
const int RT1ledPinR =  4;    // RT1 R: IO4
const int RT1ledPinG =  5;    // RT1 G: IO5
const int RT1ledPinB =  6;    // RT1 B: IO6
const int GT1ledPinR =  15;   // GT1 R: IO15
const int GT1ledPinG =  16;   // GT1 G: IO16
const int GT1ledPinB =  17;   // GT1 B: IO17

int ST1state; // 0: unavailable, 1: available, 2: reserved
int ST2state;
int RT1state;
int GT1state;

// --- Keypad (4x3): * clear, # submit; rows IO1,2,42,40 cols IO39,47,21 ---
static const byte KEYPAD_ROWS = 4;
static const byte KEYPAD_COLS = 3;
static char keypadKeys[KEYPAD_ROWS][KEYPAD_COLS] = {
    {'1', '2', '3'},
    {'4', '5', '6'},
    {'7', '8', '9'},
    {'*', '0', '#'}};
static byte keypadRowPins[KEYPAD_ROWS] = {1, 2, 42, 40};
static byte keypadColPins[KEYPAD_COLS] = {39, 47, 21};
static Keypad keypad =
    Keypad(makeKeymap(keypadKeys), keypadRowPins, keypadColPins, KEYPAD_ROWS, KEYPAD_COLS);

static void printCenteredAtY(int py, const char* text) {
  display.getTextBounds(text, 0, 0, &x, &y, &w, &h);
  length = (SCREEN_WIDTH - w) / 2;
  display.setCursor(length, py);
  display.print(text);
}

static void refreshDisplay2() {
  TCA9548A(3);
  display.clearDisplay();
  display.setTextSize(1);
  char buf[24];
  snprintf(buf, sizeof(buf), "ST 1: %s", iotStatusLabel(ST1state));
  printCenteredAtY(0, buf);
  snprintf(buf, sizeof(buf), "ST 2: %s", iotStatusLabel(ST2state));
  printCenteredAtY(15, buf);
  snprintf(buf, sizeof(buf), "RT 1: %s", iotStatusLabel(RT1state));
  printCenteredAtY(30, buf);
  snprintf(buf, sizeof(buf), "GT 1: %s", iotStatusLabel(GT1state));
  printCenteredAtY(45, buf);
  display.display();
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.println("Welcome to Library Reservation System ");
  Serial.println("Keypad: 6-digit reservation OTP, * clear, # submit (after item on table at ST1).");

  // Start I2C: SDA IO8, SCL IO9 (ST1 wiring)
  Wire.begin(8, 9);

    // Init OLED display on bus number 2
  TCA9548A(2);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  } 
  // Clear the buffer
  display.clearDisplay();

  // Init OLED display on bus number 3
  TCA9548A(3);
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;);
  } 
  // Clear the buffer
  display.clearDisplay();

  //init Wifi
  initWiFi();

  // init LED
  pinMode(ST2buttonPin, INPUT_PULLUP); // set ESP32 pin to input pull-up mode
  pinMode(RT1buttonPin, INPUT_PULLUP);
  pinMode(GT1buttonPin, INPUT_PULLUP);
  
  pinMode(ST2ledPinR, OUTPUT);  // set ESP32 pin to output mode
  pinMode(ST2ledPinG, OUTPUT);
  pinMode(ST2ledPinB, OUTPUT);
  pinMode(RT1ledPinR, OUTPUT);
  pinMode(RT1ledPinG, OUTPUT);
  pinMode(RT1ledPinB, OUTPUT);
  pinMode(GT1ledPinR, OUTPUT);
  pinMode(GT1ledPinG, OUTPUT);
  pinMode(GT1ledPinB, OUTPUT);

  // All four slots match demo tables 1–4; sync persisted ``Table.status`` from API on boot
  // (demo seeds 1=free, 2=occupied, 3=reserved, 4=free — see reload_frontend_demo status_cycle).
  ST1state = fetchStatusFromApi(TABLE_NUM_ST1, 1, HTTP_GET_BOOT_TIMEOUT_MS);
  ST2state = fetchStatusFromApi(TABLE_NUM_ST2, 1, HTTP_GET_BOOT_TIMEOUT_MS);
  RT1state = fetchStatusFromApi(TABLE_NUM_RT1, 1, HTTP_GET_BOOT_TIMEOUT_MS);
  GT1state = fetchStatusFromApi(TABLE_NUM_GT1, 1, HTTP_GET_BOOT_TIMEOUT_MS);
  fetchSt1WeightAvailabilityFromApi();
  setRgbForState(ST2ledPinR, ST2ledPinG, ST2ledPinB, ST2state);
  setRgbForState(RT1ledPinR, RT1ledPinG, RT1ledPinB, RT1state);
  setRgbForState(GT1ledPinR, GT1ledPinG, GT1ledPinB, GT1state);

/*
   //init display OLED
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE); // Draw white text
*/


  //INIT HX711
  Serial.println("init HX711");
  TCA9548A(2);
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE); // Draw white text
  display.getTextBounds("WELCOME D1", 0, 0, &x, &y, &w, &h); //untuk mendapatkan jumlah piksel dalam width dan tinggi untuk tulisan WELCOME
  length = (SCREEN_WIDTH - w) / 2; // Untuk menentukan penempatan piksel pertama
  width = (SCREEN_HEIGHT - h) / 2;
  display.setCursor(length, width);     // kursor diletakkan pada rata tengah kolom 0
  display.print("WELCOME D1");
  display.display();

  TCA9548A(3);
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE); // Draw white text
  display.getTextBounds("WELCOME D2", 0, 0, &x, &y, &w, &h); //untuk mendapatkan jumlah piksel dalam width dan tinggi untuk tulisan WELCOME
  length = (SCREEN_WIDTH - w) / 2; // Untuk menentukan penempatan piksel pertama
  width = (SCREEN_HEIGHT - h) / 2;
  display.setCursor(length, width);     // kursor diletakkan pada rata tengah kolom 0
  display.print("WELCOME D2");
  display.display();


  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  scale.set_scale();
  scale.tare(); //Reset the scale to 0
  long zero_factor = scale.read_average(); //Get a baseline reading
  delay(1000);

  //Write to OLED on bus number 2 ready to go
  TCA9548A(2);  
  display.clearDisplay();
  display.getTextBounds("Display 1 Ready!!!", 0, 0, &x, &y, &w, &h); //untuk mendapatkan jumlah piksel dalam width dan tinggi untuk tulisan WELCOME
  length = (SCREEN_WIDTH - w) / 2; // Untuk menentukan penempatan piksel pertama
  width = (SCREEN_HEIGHT - h) / 2;
  display.setCursor(length, width);     // kursor diletakkan pada rata tengah kolom 0
  display.print("Disp 1 Ready!!!");
  display.display();
  Serial.println("Disp 1 Ready To Go");
  delay(1000);

  //display 1
  display.clearDisplay();
  display.getTextBounds("ST 1", 0, 0, &x, &y, &w, &h); //untuk mendapatkan jumlah piksel dalam width dan tinggi untuk tulisan WELCOME
  length = (SCREEN_WIDTH - w) / 2; // Untuk menentukan penempatan piksel pertama
  display.setCursor(length, 0);     // kursor diletakkan pada rata tengah kolom 0
  display.print("ST 1");

  display.getTextBounds("AVAILABLE", 0, 0, &x, &y, &w, &h); //untuk mendapatkan jumlah piksel dalam width dan tinggi untuk tulisan WELCOME
  length = (SCREEN_WIDTH - w) / 2; // Untuk menentukan penempatan piksel pertama
  display.setCursor(length, 25);     // kursor diletakkan pada rata tengah kolom 0
  display.print("AVAILABLE");
  display.display();

  //Write to OLED on bus number 3 ready to go
  TCA9548A(3);  
  display.clearDisplay();
  display.getTextBounds(" Display 2 Ready!!!", 0, 0, &x, &y, &w, &h); //untuk mendapatkan jumlah piksel dalam width dan tinggi untuk tulisan WELCOME
  length = (SCREEN_WIDTH - w) / 2; // Untuk menentukan penempatan piksel pertama
  width = (SCREEN_HEIGHT - h) / 2;
  display.setCursor(length, width);     // kursor diletakkan pada rata tengah kolom 0
  display.print("Disp 2 Ready!!!");
  display.display();
  Serial.println("Disp 2 Ready To Go");
  delay(1000);

  refreshDisplay2();
}

void loop() {
  unsigned long now = millis();
  static unsigned long lastPoll = 0;
  static unsigned long lastSlowWork = 0;
  static uint8_t pollStep = 0;
  static bool pollCycleActive = false;
  static TableButtonDebouncer st2Btn = {0, 0};
  static TableButtonDebouncer rt1Btn = {0, 0};
  static TableButtonDebouncer gt1Btn = {0, 0};
  static unsigned long suppressPollUntil = 0;

  // Buttons first — before slow scale/OLED/HTTP. Stable-low + release-rearm ⇒ one step per click.
  if (consumeStableTablePress(ST2buttonPin, st2Btn, now)) {
    Serial.println("ST2 pressed — cycle status");
    ST2state = (ST2state + 1) % 3;
    setRgbForState(ST2ledPinR, ST2ledPinG, ST2ledPinB, ST2state);
    scheduleStatusPost(TABLE_NUM_ST2, ST2state);
    suppressPollUntil = millis() + BTN_POLL_SUPPRESS_MS;
  }
  if (consumeStableTablePress(RT1buttonPin, rt1Btn, now)) {
    Serial.println("RT1 pressed — cycle status");
    RT1state = (RT1state + 1) % 3;
    setRgbForState(RT1ledPinR, RT1ledPinG, RT1ledPinB, RT1state);
    scheduleStatusPost(TABLE_NUM_RT1, RT1state);
    suppressPollUntil = millis() + BTN_POLL_SUPPRESS_MS;
  }
  if (consumeStableTablePress(GT1buttonPin, gt1Btn, now)) {
    Serial.println("GT1 pressed — cycle status");
    GT1state = (GT1state + 1) % 3;
    setRgbForState(GT1ledPinR, GT1ledPinG, GT1ledPinB, GT1state);
    scheduleStatusPost(TABLE_NUM_GT1, GT1state);
    suppressPollUntil = millis() + BTN_POLL_SUPPRESS_MS;
  }

  char key = keypad.getKey();
  if (key) {
    if (key == '#') {
      if (keypadInputBuffer.length() == KEYPAD_PIN_LIMIT) {
        g_st1OtpSubmitPending = true;
      } else {
        Serial.println("\nEnter 6 digits then #.");
      }
    } else if (key == '*') {
      keypadInputBuffer = "";
      Serial.println("\nCleared.");
    } else {
      if (keypadInputBuffer.length() < KEYPAD_PIN_LIMIT) {
        keypadInputBuffer += key;
        Serial.print(key);
      } else {
        Serial.println("\n[Limit reached! Press # to submit]");
      }
    }
  }

  // Let debouncers advance in real time: HX711 + OLED + poll are deferred to a throttled block.
  if ((unsigned long)(now - lastSlowWork) < (unsigned long)SLOW_LOOP_PERIOD_MS) {
    delay(1);
    return;
  }
  lastSlowWork = now;
  now = millis();

  // HTTP after LEDs: one short POST attempt so the fast path never sits in Wi-Fi.
  drainPendingStatusPost();

  if (g_st1OtpSubmitPending) {
    g_st1OtpSubmitPending = false;
    if (keypadInputBuffer.length() == KEYPAD_PIN_LIMIT) {
      if (postVerifyOtpToApi()) {
        st1FeedbackKind = 1;
        st1FeedbackUntil = now + ST1_OTP_FEEDBACK_MS;
        keypadInputBuffer = "";
        fetchSt1WeightAvailabilityFromApi();
      } else {
        st1FeedbackKind = 2;
        st1FeedbackUntil = now + ST1_OTP_FEEDBACK_MS;
        keypadInputBuffer = "";
      }
    }
  }

  unsigned int ADC = scale.get_units();
  float weight = float(ADC) / 2100.00 * 5.00;
  Serial.print("ADC = ");
  Serial.print(ADC);
  Serial.print(", weight = ");
  Serial.println(weight);

  g_st1LastInSeat = (weight >= 2.00f);

  // ST1: HX711 -> debounced IoT state 0/1 -> POST (2=reserved not used from weight)
  static int st1Cand = -1;
  static unsigned long st1CandSince = 0;
  int meas = (weight >= 2.00f) ? 0 : 1;
  if (st1Cand < 0) {
    st1Cand = meas;
    st1CandSince = now;
  }
  if (meas != st1Cand) {
    st1Cand = meas;
    st1CandSince = now;
  }
  if ((now - st1CandSince) >= ST1_DEBOUNCE_MS && ST1state != st1Cand) {
    ST1state = st1Cand;
    const bool suppressPost = (ST1state == 1) && st1ShouldSuppressFreeStatusPost();
    if (!suppressPost) {
      scheduleStatusPost(TABLE_NUM_ST1, ST1state);
    }
  }

  // OLED bus 2 — ST1: reservation + OTP flow when API reports a booking window
  TCA9548A(2);
  display.clearDisplay();
  display.setTextSize(2);
  display.getTextBounds("ST 1", 0, 0, &x, &y, &w, &h);
  length = (SCREEN_WIDTH - w) / 2;
  display.setCursor(length, 0);
  display.print("ST 1");
  if (st1FeedbackKind != 0 && (unsigned long)now < st1FeedbackUntil) {
    display.setTextSize(1);
    if (st1FeedbackKind == 1) {
      printCenteredAtY(22, "RESERVED OK");
      printCenteredAtY(38, "OTP ACCEPTED");
    } else {
      printCenteredAtY(30, "WRONG OTP");
    }
  } else if (st1HasBookingWindowForUi()) {
    st1FeedbackKind = 0;
    display.setTextSize(1);
    if (st1OtpVerified) {
      printCenteredAtY(14, "RESERVED");
      printCenteredAtY(26, "CONFIRMED");
      char bufAvail[40];
      snprintf(bufAvail, sizeof(bufAvail), "Avail until %s", st1BookingWindowEndLocal);
      printCenteredAtY(44, bufAvail);
    } else if (g_st1LastInSeat) {
      printCenteredAtY(16, "PLEASE ENTER");
      printCenteredAtY(30, "OTP");
      printCenteredAtY(48, "* clr # send");
    } else {
      printCenteredAtY(10, "RESERVED");
      char win[24];
      snprintf(win, sizeof(win), "%s - %s", st1BookingStartsLocal, st1BookingWindowEndLocal);
      printCenteredAtY(24, win);
      printCenteredAtY(40, "Place item on table");
      printCenteredAtY(52, "Enter OTP on keypad");
    }
  } else {
    st1FeedbackKind = 0;
    if (ST1state == 0) {
      display.setTextSize(1);
      display.getTextBounds("UNAVAILABLE", 0, 0, &x, &y, &w, &h);
      length = (SCREEN_WIDTH - w) / 2;
      display.setCursor(length, 20);
      display.print("UNAVAILABLE");
      display.setTextSize(1);
      display.getTextBounds("WILL AVAILABLE AT", 0, 0, &x, &y, &w, &h);
      length = (SCREEN_WIDTH - w) / 2;
      display.setCursor(length, 30);
      display.print("WILL AVAILABLE AT");
      display.setTextSize(2);
      display.getTextBounds("00:00:00", 0, 0, &x, &y, &w, &h);
      length = (SCREEN_WIDTH - w) / 2;
      display.setCursor(length, 50);
      display.print(st1BookingEndsLocal);
    } else if (ST1state == 2) {
      display.setTextSize(1);
      display.getTextBounds("RESERVED", 0, 0, &x, &y, &w, &h);
      length = (SCREEN_WIDTH - w) / 2;
      display.setCursor(length, 25);
      display.print("RESERVED");
    } else {
      display.setTextSize(1);
      display.getTextBounds("AVAILABLE", 0, 0, &x, &y, &w, &h);
      length = (SCREEN_WIDTH - w) / 2;
      display.setCursor(length, 25);
      display.print("AVAILABLE");
    }
  }
  display.display();

  // Spread remote reads across slow-path visits (one HTTP per loop) so we never
  // block ~10s+ in a single iteration; LEDs already match local state from presses.
  if (now >= suppressPollUntil) {
    if (!pollCycleActive && (unsigned long)(now - lastPoll) >= REMOTE_POLL_MS) {
      pollCycleActive = true;
      pollStep = 0;
    }
    if (pollCycleActive && WiFi.status() == WL_CONNECTED) {
      if (pollStep == 0) {
        fetchSt1WeightAvailabilityFromApi();
        pollStep = 1;
      } else if (pollStep == 1) {
        ST2state = fetchStatusFromApi(TABLE_NUM_ST2, ST2state, HTTP_GET_POLL_TIMEOUT_MS);
        setRgbForState(ST2ledPinR, ST2ledPinG, ST2ledPinB, ST2state);
        pollStep = 2;
      } else if (pollStep == 2) {
        RT1state = fetchStatusFromApi(TABLE_NUM_RT1, RT1state, HTTP_GET_POLL_TIMEOUT_MS);
        setRgbForState(RT1ledPinR, RT1ledPinG, RT1ledPinB, RT1state);
        pollStep = 3;
      } else {
        GT1state = fetchStatusFromApi(TABLE_NUM_GT1, GT1state, HTTP_GET_POLL_TIMEOUT_MS);
        setRgbForState(GT1ledPinR, GT1ledPinG, GT1ledPinB, GT1state);
        pollStep = 0;
        pollCycleActive = false;
        lastPoll = now;
      }
    } else if (pollCycleActive && WiFi.status() != WL_CONNECTED) {
      pollCycleActive = false;
      pollStep = 0;
      lastPoll = now;
    }
  }

  refreshDisplay2();

  delay(1);
}