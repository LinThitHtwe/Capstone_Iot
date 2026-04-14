#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>

//Insert Wifi library
#include <WiFi.h>
#include <HTTPClient.h>
#include <stdio.h>
#include <string.h>

// Replace with your network credentials (STATION)
#define ssid "Kakak 4 Bocil"
#define password "kakaknasmah"

// Django REST API: use this PC's Wi-Fi IPv4 (ipconfig). Server must listen on all interfaces:
//   manage.py runserver 0.0.0.0:8001
// (127.0.0.1-only runserver is unreachable from the ESP32 on the LAN.)
// If the board uses Host-Only / VM network, try 192.168.56.1 instead.
#define API_HOST "10.205.251.117"
#define API_PORT 8001
// Full path pattern: http://API_HOST:API_PORT/api/iot/table-status/<table_number>/

// Django Table.table_number (not DB row id). Must match the first four entries from
// ``reload_frontend_demo`` / ``capstone-frontend/lib/data/admin-tables-mock.ts``:
// floor 1 scatter coords (40,45)→1, (200,38)→2, (380,62)→3, (560,44)→4.
#define TABLE_NUM_ST1 1
#define TABLE_NUM_ST2 2
#define TABLE_NUM_RT1 3
#define TABLE_NUM_GT1 4

#define ST1_DEBOUNCE_MS 600
#define REMOTE_POLL_MS 2000
#define ST1_RESV_POLL_MS 400
#define HTTP_TIMEOUT_MS 4000
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

static String makeTableReservationUrl(int tableNumber) {
  return String("http://") + API_HOST + ":" + String(API_PORT) +
         "/api/iot/table-reservation/" + String(tableNumber) + "/";
}

/** Table 1 booking flags from GET /api/iot/table-reservation/1/ (demo-friendly). */
static bool st1ActiveReservation = false;
static bool st1UnfinishedBooking = false;
static char st1ReservationEndLocal[16] = "--:--:--";

/** Read JSON boolean after "key": (key must include leading "). */
static bool jsonBoolAfterKey(const String& body, const char* keyWithQuote) {
  int k = body.indexOf(keyWithQuote);
  if (k < 0) return false;
  int c = body.indexOf(':', k + (int)strlen(keyWithQuote));
  if (c < 0) return false;
  int i = c + 1;
  while (i < (int)body.length() && (body.charAt(i) == ' ' || body.charAt(i) == '\t')) i++;
  return body.substring(i).startsWith("true");
}

/** Copy quoted string value for "reservation_end_local" (handles : null). */
static void jsonCopyReservationEndLocal(const String& body, char* out, size_t outSz) {
  if (outSz == 0) return;
  out[0] = '\0';
  const char* tag = "\"reservation_end_local\"";
  int k = body.indexOf(tag);
  if (k < 0) return;
  int colon = body.indexOf(':', k);
  if (colon < 0) return;
  int i = colon + 1;
  while (i < (int)body.length() && (body.charAt(i) == ' ' || body.charAt(i) == '\t')) i++;
  if (i >= (int)body.length()) return;
  if (body.charAt(i) != '"') return;
  i++;
  int start = i;
  int end = body.indexOf('"', start);
  if (end <= start || (size_t)(end - start) >= outSz) return;
  body.substring(start, end).toCharArray(out, outSz);
}

static void refreshSt1ReservationFromApi() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = makeTableReservationUrl(TABLE_NUM_ST1);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) {
    Serial.println("resv: http.begin failed");
    return;
  }
  int code = http.GET();
  if (code != 200) {
    Serial.printf("GET resv %s code=%d\n", url.c_str(), code);
    http.end();
    return;
  }
  String body = http.getString();
  http.end();

  /* Only replace state on success so a failed GET does not wipe a good demo read. */
  st1ActiveReservation = jsonBoolAfterKey(body, "\"has_active_reservation\"");
  st1UnfinishedBooking = jsonBoolAfterKey(body, "\"has_unfinished_reservation\"");
  jsonCopyReservationEndLocal(body, st1ReservationEndLocal, sizeof(st1ReservationEndLocal));
  if (st1ReservationEndLocal[0] == '\0') {
    strncpy(st1ReservationEndLocal, "--:--:--", sizeof(st1ReservationEndLocal));
    st1ReservationEndLocal[sizeof(st1ReservationEndLocal) - 1] = '\0';
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
static int fetchStatusFromApi(int tableNumber, int defaultIot) {
  if (WiFi.status() != WL_CONNECTED) return defaultIot;
  HTTPClient http;
  String url = makeTableStatusUrl(tableNumber);
  http.setTimeout(HTTP_TIMEOUT_MS);
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

static bool postStatusToApi(int tableNumber, int iotState) {
  if (WiFi.status() != WL_CONNECTED) return false;
  int apiVal = apiStatusFromIot(iotState);
  String body = String("{\"status\":") + String(apiVal) + "}");
  String url = makeTableStatusUrl(tableNumber);
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(100);
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
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

/** INPUT_PULLUP: pressed = LOW. One press = one fire; must release before another. */
struct TableButtonDebouncer {
  uint8_t phase; /* 0 = armed (released), 1 = latched until full release */
  unsigned long mark;
};

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
const int LOADCELL_DOUT_PIN = 12;
const int LOADCELL_SCK_PIN = 13;
HX711 scale;
float calibration_factor = -1365; // for me this vlaue works just perfect 419640


//Variable will not change for
const int ST2buttonPin = 26;  // the number of the pushbutton pin
const int RT1buttonPin = 27;
const int GT1buttonPin = 14;

const int ST2ledPinR =  25;    // the number of the LED pin
const int ST2ledPinG =  33;    // led R: Unavailable, Led G: Available
const int ST2ledPinB =  32;    // Led B: Reserved
const int RT1ledPinR =  19;
const int RT1ledPinG =  18;
const int RT1ledPinB =  5;
const int GT1ledPinR =  4;
const int GT1ledPinG =  2;
const int GT1ledPinB =  15;

int ST1state; // 0: unavailable, 1: available, 2: reserved
int ST2state;
int RT1state;
int GT1state;

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

  // Start I2C communication with the Multiplexer for OLED
  Wire.begin();

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
  ST1state = fetchStatusFromApi(TABLE_NUM_ST1, 1);
  ST2state = fetchStatusFromApi(TABLE_NUM_ST2, 1);
  RT1state = fetchStatusFromApi(TABLE_NUM_RT1, 1);
  GT1state = fetchStatusFromApi(TABLE_NUM_GT1, 1);
  setRgbForState(ST2ledPinR, ST2ledPinG, ST2ledPinB, ST2state);
  setRgbForState(RT1ledPinR, RT1ledPinG, RT1ledPinB, RT1state);
  setRgbForState(GT1ledPinR, GT1ledPinG, GT1ledPinB, GT1state);

  refreshSt1ReservationFromApi();

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
  static TableButtonDebouncer st2Btn = {0, 0};
  static TableButtonDebouncer rt1Btn = {0, 0};
  static TableButtonDebouncer gt1Btn = {0, 0};
  static unsigned long suppressPollUntil = 0;

  // Buttons first — before slow scale/OLED/HTTP. Stable-low + release-rearm ⇒ one step per click.
  if (consumeStableTablePress(ST2buttonPin, st2Btn, now)) {
    Serial.println("ST2 pressed — cycle status");
    ST2state = (ST2state + 1) % 3;
    setRgbForState(ST2ledPinR, ST2ledPinG, ST2ledPinB, ST2state);
    postStatusToApi(TABLE_NUM_ST2, ST2state);
    suppressPollUntil = millis() + BTN_POLL_SUPPRESS_MS;
  }
  if (consumeStableTablePress(RT1buttonPin, rt1Btn, now)) {
    Serial.println("RT1 pressed — cycle status");
    RT1state = (RT1state + 1) % 3;
    setRgbForState(RT1ledPinR, RT1ledPinG, RT1ledPinB, RT1state);
    postStatusToApi(TABLE_NUM_RT1, RT1state);
    suppressPollUntil = millis() + BTN_POLL_SUPPRESS_MS;
  }
  if (consumeStableTablePress(GT1buttonPin, gt1Btn, now)) {
    Serial.println("GT1 pressed — cycle status");
    GT1state = (GT1state + 1) % 3;
    setRgbForState(GT1ledPinR, GT1ledPinG, GT1ledPinB, GT1state);
    postStatusToApi(TABLE_NUM_GT1, GT1state);
    suppressPollUntil = millis() + BTN_POLL_SUPPRESS_MS;
  }

  unsigned int ADC = scale.get_units();
  float weight = float(ADC) / 2100.00 * 5.00;
  Serial.print("ADC = ");
  Serial.print(ADC);
  Serial.print(", weight = ");
  Serial.println(weight);

  // ST1: poll reservation JSON often (demo) so end time / reserved flags update quickly.
  static unsigned long lastSt1ResvPoll = 0;
  if (now - lastSt1ResvPoll >= ST1_RESV_POLL_MS) {
    lastSt1ResvPoll = now;
    refreshSt1ReservationFromApi();
  }

  // ST1: weight + any unfinished booking -> IoT 0=unavail, 1=avail, 2=reserved; POST maps to API.
  static int st1Cand = -1;
  static unsigned long st1CandSince = 0;
  const int measWeight = (weight >= 2.00f) ? 0 : 1;
  const bool st1BookedForDemo = st1ActiveReservation || st1UnfinishedBooking;
  int st1Desired;
  if (st1BookedForDemo && measWeight == 1) {
    st1Desired = 2;
  } else if (measWeight == 0) {
    st1Desired = 0;
  } else {
    st1Desired = 1;
  }
  if (st1Cand < 0) {
    st1Cand = st1Desired;
    st1CandSince = now;
  }
  if (st1Desired != st1Cand) {
    st1Cand = st1Desired;
    st1CandSince = now;
  }
  if ((now - st1CandSince) >= ST1_DEBOUNCE_MS && ST1state != st1Cand) {
    ST1state = st1Cand;
    postStatusToApi(TABLE_NUM_ST1, ST1state);
  }

  // OLED bus 2 — ST1 line reflects debounced ST1state + reservation end time when occupied
  TCA9548A(2);
  display.clearDisplay();
  display.setTextSize(2);
  display.getTextBounds("ST 1", 0, 0, &x, &y, &w, &h);
  length = (SCREEN_WIDTH - w) / 2;
  display.setCursor(length, 0);
  display.print("ST 1");
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
    display.getTextBounds(st1ReservationEndLocal, 0, 0, &x, &y, &w, &h);
    length = (SCREEN_WIDTH - w) / 2;
    display.setCursor(length, 50);
    display.print(st1ReservationEndLocal);
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
  display.display();

  if (now - lastPoll >= REMOTE_POLL_MS && now >= suppressPollUntil) {
    lastPoll = now;
    if (WiFi.status() == WL_CONNECTED) {
      // Tables 2–4: buttons + remote (ST1 reservation polled above every ST1_RESV_POLL_MS)
      ST2state = fetchStatusFromApi(TABLE_NUM_ST2, ST2state);
      RT1state = fetchStatusFromApi(TABLE_NUM_RT1, RT1state);
      GT1state = fetchStatusFromApi(TABLE_NUM_GT1, GT1state);
      setRgbForState(ST2ledPinR, ST2ledPinG, ST2ledPinB, ST2state);
      setRgbForState(RT1ledPinR, RT1ledPinG, RT1ledPinB, RT1state);
      setRgbForState(GT1ledPinR, GT1ledPinG, GT1ledPinB, GT1state);
    }
  }

  refreshDisplay2();

  delay(50);
}