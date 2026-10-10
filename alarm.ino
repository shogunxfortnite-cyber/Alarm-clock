#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------- PINS ----------
#define TFT_CS    5
#define TFT_DC    6
#define TFT_RST   7
#define TFT_MOSI  10
#define TFT_SCLK  8

#define SW_DISMISS 2   // D0
#define SW_MENU    4   // D2
#define SW_UP      21  // D6
#define SW_DOWN    20  // D7
#define BUZZER     3   // D1

TFT_eSPI tft = TFT_eSPI();
WebServer server(80);
Preferences prefs;

// ---------- WIFI ----------
const char* ssid     = "YOUR_WIFI";
const char* password = "YOUR_PASS";

// ---------- LOGIN ----------
const char* www_username = "admin";
const char* www_password = "blare2024";

// ---------- WEATHER ----------
const String API_KEY = "YOUR_OPENWEATHER_KEY";
const String CITY    = "Mumbai,IN";

// ---------- TIME ----------
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;

// ---------- STATE ----------
bool alarmEnabled = false;
int alarmHour = 7;
int alarmMinute = 30;
bool alarmRinging = false;
int menuState = 0;              // 0=clock, 1=hour, 2=minute, 3=toggle
unsigned long lastWeatherFetch = 0;
const unsigned long WEATHER_INTERVAL = 600000;

String currentTemp = "--";
String currentCondition = "Loading...";
String alertText = "";

// ---------- HTML ----------
String getHTMLPage() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>BLARE</title><style>";
  html += "body{background:#0a0f1e;color:#e0f0ff;font-family:system-ui,sans-serif;display:flex;justify-content:center;padding-top:40px;margin:0}";
  html += ".card{background:rgba(5,10,25,0.9);border-radius:24px;padding:30px;max-width:400px;width:90%;box-shadow:0 0 40px rgba(0,180,255,0.3);border:1px solid rgba(180,220,255,0.3)}";
  html += "h1{text-align:center;color:#00aaff;letter-spacing:4px}";
  html += ".row{display:flex;justify-content:space-between;align-items:center;margin:15px 0}";
  html += "label{color:#8ab8ff}";
  html += "input[type=time]{background:#0c1a2b;border:1px solid #3f7fa8;border-radius:20px;padding:8px 12px;color:#e0f0ff;font-size:1.1rem;width:120px;text-align:center}";
  html += "input[type=submit]{background:#0a2a44;border:1px solid #2f8bc0;color:#d0ecff;padding:10px 30px;border-radius:20px;cursor:pointer;font-weight:600;width:100%;margin-top:15px;font-size:1rem}";
  html += "input[type=submit]:hover{background:#144a6b;box-shadow:0 0 25px #0088ff}";
  html += ".status{text-align:center;margin:15px 0;padding:10px;border-radius:12px;background:#0a1a2a;border:1px solid #2a5f8a;font-size:0.9rem}";
  html += ".on{color:#00ff88}.off{color:#ff6666}";
  html += "</style></head><body><div class='card'>";
  html += "<h1>BLARE</h1>";
  html += "<div class='status'>Alarm: <span class='" + String(alarmEnabled ? "on" : "off") + "'>" + String(alarmEnabled ? "ON" : "OFF") + "</span></div>";
  html += "<form action='/set' method='GET'>";
  html += "<div class='row'><label>Alarm Time</label><input type='time' name='time' value='" + String(alarmHour < 10 ? "0" : "") + String(alarmHour) + ":" + String(alarmMinute < 10 ? "0" : "") + String(alarmMinute) + "'></div>";
  html += "<input type='submit' value='SET ALARM'></form>";
  html += "<form action='/toggle' method='GET'>";
  html += "<input type='submit' value='" + String(alarmEnabled ? "DISABLE" : "ENABLE") + "'></form>";
  html += "</div></body></html>";
  return html;
}

bool isAuthenticated() {
  if (!server.authenticate(www_username, www_password)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleRoot() {
  if (!isAuthenticated()) return;
  server.send(200, "text/html", getHTMLPage());
}

void saveAlarm();
void loadAlarm();

void handleSet() {
  if (!isAuthenticated()) return;
  String timeVal = server.arg("time");
  int colonIdx = timeVal.indexOf(':');
  if (colonIdx > 0) {
    alarmHour = timeVal.substring(0, colonIdx).toInt();
    alarmMinute = timeVal.substring(colonIdx + 1).toInt();
    if (alarmHour >= 0 && alarmHour < 24 && alarmMinute >= 0 && alarmMinute < 60) {
      saveAlarm();
    }
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleToggle() {
  if (!isAuthenticated()) return;
  alarmEnabled = !alarmEnabled;
  saveAlarm();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void loadAlarm() {
  prefs.begin("blare", true);
  alarmHour = prefs.getInt("hour", 7);
  alarmMinute = prefs.getInt("min", 30);
  alarmEnabled = prefs.getBool("enabled", false);
  prefs.end();
}

void saveAlarm() {
  prefs.begin("blare", false);
  prefs.putInt("hour", alarmHour);
  prefs.putInt("min", alarmMinute);
  prefs.putBool("enabled", alarmEnabled);
  prefs.end();
}

void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?q=" + CITY + "&appid=" + API_KEY + "&units=metric";
  http.begin(url);
  if (http.GET() == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, payload);
    currentTemp = String(doc["main"]["temp"].as<float>(), 1);
    currentCondition = doc["weather"][0]["description"].as<String>();
    String mainWeather = doc["weather"][0]["main"].as<String>();
    alertText = (mainWeather == "Thunderstorm" || mainWeather == "Tornado" || mainWeather == "Squall") ? mainWeather : "";
  }
  http.end();
}

void drawScreen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  tft.fillScreen(TFT_BLACK);

  char timeStr[10];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.setCursor(10, 10);
  tft.println(timeStr);

  tft.setTextSize(1);
  tft.setCursor(10, 55);
  char dateStr[20];
  strftime(dateStr, sizeof(dateStr), "%a, %d %b %Y", &timeinfo);
  tft.println(dateStr);

  tft.setCursor(10, 80);
  tft.setTextColor(TFT_CYAN);
  tft.print("Temp: "); tft.print(currentTemp); tft.println(" C");

  tft.setCursor(10, 100);
  tft.setTextColor(TFT_YELLOW);
  tft.println(currentCondition);

  if (alertText.length() > 0) {
    tft.setTextColor(TFT_RED);
    tft.setCursor(10, 125);
    tft.println("! " + alertText);
  }

  // Alarm status line — highlights the field being edited
  tft.setCursor(10, 150);
  tft.setTextColor(alarmEnabled ? TFT_GREEN : TFT_DARKGREY);
  tft.print("Alarm ");

  if (menuState == 1) tft.setTextColor(TFT_YELLOW);
  else tft.setTextColor(alarmEnabled ? TFT_GREEN : TFT_DARKGREY);
  tft.print(alarmHour);

  tft.print(":");

  if (menuState == 2) tft.setTextColor(TFT_YELLOW);
  else tft.setTextColor(alarmEnabled ? TFT_GREEN : TFT_DARKGREY);
  if (alarmMinute < 10) tft.print("0");
  tft.print(alarmMinute);

  if (menuState == 3) {
    tft.setTextColor(TFT_YELLOW);
    tft.print(alarmEnabled ? "  [ON]" : "  [OFF]");
  }

  // Menu hint
  tft.setTextColor(TFT_ORANGE);
  tft.setCursor(10, 170);
  switch (menuState) {
    case 0: tft.print("SW2=MENU"); break;
    case 1: tft.print("SET HOUR: SW3+ SW4-"); break;
    case 2: tft.print("SET MIN:  SW3+ SW4-"); break;
    case 3: tft.print("SW3/SW4=TOGGLE"); break;
  }

  tft.setTextColor(TFT_BLUE);
  tft.setCursor(10, 190);
  tft.print("IP: "); tft.print(WiFi.localIP());
}

void checkAlarm() {
  if (!alarmEnabled || alarmRinging) return;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  if (timeinfo.tm_hour == alarmHour && timeinfo.tm_min == alarmMinute && timeinfo.tm_sec == 0) {
    alarmRinging = true;
  }
}

void ringAlarm() {
  tft.fillScreen(TFT_RED);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.setCursor(20, 60);
  tft.println("WAKE UP");
  tft.setTextSize(1);
  tft.setCursor(20, 120);
  tft.println("Press SW1 to dismiss");

  tone(BUZZER, 880);  delay(150);
  noTone(BUZZER);     delay(100);
  tone(BUZZER, 1100); delay(150);
  noTone(BUZZER);     delay(100);
}

// --- Button helper: returns true only on a fresh press ---
bool pressed(int pin, unsigned long &lastTime) {
  if (digitalRead(pin) == LOW && millis() - lastTime > 250) {
    lastTime = millis();
    return true;
  }
  return false;
}

void handleButtons() {
  static unsigned long tDismiss = 0, tMenu = 0, tUp = 0, tDown = 0;

  // SW1 — dismiss alarm or exit menu
  if (pressed(SW_DISMISS, tDismiss)) {
    if (alarmRinging) {
      alarmRinging = false;
      noTone(BUZZER);
    } else if (menuState != 0) {
      menuState = 0;
      saveAlarm();
    }
  }

  // SW2 — cycle menu
  if (pressed(SW_MENU, tMenu) && !alarmRinging) {
    menuState = (menuState + 1) % 4;
    if (menuState == 0) saveAlarm();
  }

  // SW3 — increment
  if (pressed(SW_UP, tUp) && !alarmRinging) {
    if (menuState == 1) {
      alarmHour = (alarmHour + 1) % 24;
    } else if (menuState == 2) {
      alarmMinute = (alarmMinute + 1) % 60;
    } else if (menuState == 3) {
      alarmEnabled = !alarmEnabled;
      saveAlarm();
    }
  }

  // SW4 — decrement
  if (pressed(SW_DOWN, tDown) && !alarmRinging) {
    if (menuState == 1) {
      alarmHour = (alarmHour + 23) % 24;
    } else if (menuState == 2) {
      alarmMinute = (alarmMinute + 59) % 60;
    } else if (menuState == 3) {
      alarmEnabled = !alarmEnabled;
      saveAlarm();
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(SW_DISMISS, INPUT_PULLUP);
  pinMode(SW_MENU, INPUT_PULLUP);
  pinMode(SW_UP, INPUT_PULLUP);
  pinMode(SW_DOWN, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("BLARE");

  loadAlarm();

  WiFi.begin(ssid, password);
  tft.setTextSize(1);
  tft.setCursor(10, 50);
  tft.println("Connecting WiFi...");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  tft.fillScreen(TFT_BLACK);
  tft.setCursor(10, 10);
  tft.setTextSize(2);
  tft.println("WiFi OK");
  tft.setTextSize(1);
  tft.setCursor(10, 40);
  tft.print("IP: "); tft.println(WiFi.localIP());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  delay(2000);

  server.on("/", handleRoot);
  server.on("/set", handleSet);
  server.on("/toggle", handleToggle);
  server.begin();

  fetchWeather();
  lastWeatherFetch = millis();
}

void loop() {
  server.handleClient();
  handleButtons();
  checkAlarm();

  if (alarmRinging) {
    ringAlarm();
    return;
  }

  if (millis() - lastWeatherFetch > WEATHER_INTERVAL) {
    fetchWeather();
    lastWeatherFetch = millis();
  }

  drawScreen();
  delay(100);
}