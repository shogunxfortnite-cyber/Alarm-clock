#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ================= PINS =================
#define TFT_CS     20  // D7
#define TFT_DC      5  // D3
#define TFT_RST     6  // D4
#define TFT_MOSI   10  // D10
#define TFT_SCLK    8  // D8

#define BUZZER      3  // D1
#define SW_DISMISS  2  // D0
#define SW_MENU     4  // D2
#define SW_UP       7  // D5
#define SW_DOWN    21  // D6
#define BATTERY_PIN 9  // D9 (ADC1_CH9)

TFT_eSPI tft = TFT_eSPI();
WebServer server(80);
Preferences prefs;

// ================= WIFI =================
const char* ssid     = "YOUR_WIFI";
const char* password = "YOUR_PASS";

// ================= LOGIN =================
const char* www_username = "admin";
const char* www_password = "blare2024";

// ================= WEATHER =================
const String API_KEY = "YOUR_OPENWEATHER_KEY";
const String CITY    = "Mumbai,IN";

// ================= TIME =================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0;

// ================= STATE =================
bool alarmEnabled = false;
int alarmHour = 7;
int alarmMinute = 30;
bool alarmRinging = false;
int menuState = 0;
unsigned long lastWeatherFetch = 0;
unsigned long lastBatteryCheck = 0;
const unsigned long WEATHER_INTERVAL = 600000;
const unsigned long BATTERY_INTERVAL = 5000;

String currentTemp = "--";
String currentCondition = "Loading...";
String alertText = "";
float batteryVoltage = 0.0;
int batteryPercent = 0;

// Space animation state
int starX[40], starY[40], starSpeed[40], starBright[40];
unsigned long lastStarFrame = 0;
int frameCounter = 0;

// ================= HTML =================
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
  html += "<div class='status'>Battery: " + String(batteryPercent) + "% (" + String(batteryVoltage, 2) + "V)</div>";
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

// ================= BATTERY =================
void readBattery() {
  int raw = analogRead(BATTERY_PIN);
  // Divider: two 100k resistors → /2 voltage
  batteryVoltage = (raw / 4095.0) * 3.3 * 2.0;
  // LiPo range: 3.2V empty → 4.2V full
  batteryPercent = constrain(map((int)(batteryVoltage * 100), 320, 420, 0, 100), 0, 100);
}

// ================= WEATHER =================
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

// ================= SPACE ANIMATION =================
void initStars() {
  randomSeed(analogRead(0));
  for (int i = 0; i < 40; i++) {
    starX[i] = random(0, 240);
    starY[i] = random(0, 320);
    starSpeed[i] = random(1, 4);
    starBright[i] = random(80, 255);
  }
}

void drawStars() {
  // Fade old stars (draw dark pixels over them)
  for (int i = 0; i < 40; i++) {
    tft.drawPixel(starX[i], starY[i], TFT_BLACK);
    starY[i] += starSpeed[i];
    if (starY[i] > 320) {
      starY[i] = 0;
      starX[i] = random(0, 240);
      starSpeed[i] = random(1, 4);
    }
    uint16_t color = tft.color565(starBright[i], starBright[i], 255);
    tft.drawPixel(starX[i], starY[i], color);
  }
}

void drawIdleScreen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  // Black background only on first frame
  if (frameCounter == 0) {
    tft.fillScreen(TFT_BLACK);
  }

  drawStars();

  // Time on the right side, big
  char timeStr[10];
  strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(4);
  tft.setCursor(10, 130);
  tft.println(timeStr);

  // Seconds small
  char secStr[4];
  strftime(secStr, sizeof(secStr), "%S", &timeinfo);
  tft.setTextSize(2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(200, 145);
  tft.print(":");
  tft.print(secStr);

  // Date
  tft.setTextSize(1);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.setCursor(10, 180);
  char dateStr[20];
  strftime(dateStr, sizeof(dateStr), "%a, %d %b %Y", &timeinfo);
  tft.print(dateStr);

  // Weather + battery bottom
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(10, 200);
  tft.print(currentTemp); tft.print("C  ");
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.print(currentCondition);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(10, 220);
  tft.print("BAT ");
  tft.print(batteryPercent);
  tft.print("%  ");

  tft.setTextColor(alarmEnabled ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  tft.print("ALM ");
  if (alarmEnabled) {
    if (alarmHour < 10) tft.print("0");
    tft.print(alarmHour);
    tft.print(":");
    if (alarmMinute < 10) tft.print("0");
    tft.print(alarmMinute);
  } else {
    tft.print("OFF");
  }

  if (alertText.length() > 0) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setCursor(10, 240);
    tft.print("! "); tft.print(alertText);
  }

  frameCounter++;
}

// ================= MENU SCREEN =================
void drawMenuScreen() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("SET ALARM");

  char timeStr[10];
  strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
  tft.setTextSize(3);
  tft.setCursor(10, 50);
  tft.setTextColor(TFT_CYAN);
  tft.println(timeStr);

  // Alarm display with highlight
  tft.setTextSize(4);
  tft.setCursor(20, 120);

  if (menuState == 1) tft.setTextColor(TFT_YELLOW);
  else tft.setTextColor(alarmEnabled ? TFT_GREEN : TFT_DARKGREY);
  if (alarmHour < 10) tft.print("0");
  tft.print(alarmHour);

  tft.setTextColor(TFT_WHITE);
  tft.print(":");

  if (menuState == 2) tft.setTextColor(TFT_YELLOW);
  else tft.setTextColor(alarmEnabled ? TFT_GREEN : TFT_DARKGREY);
  if (alarmMinute < 10) tft.print("0");
  tft.print(alarmMinute);

  if (menuState == 3) {
    tft.setTextColor(TFT_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(20, 190);
    tft.print(alarmEnabled ? "ALARM: ON" : "ALARM: OFF");
  }

  tft.setTextSize(1);
  tft.setTextColor(TFT_ORANGE);
  tft.setCursor(10, 230);
  switch (menuState) {
    case 1: tft.print("SW3+ / SW4-   SW2 next"); break;
    case 2: tft.print("SW3+ / SW4-   SW2 next"); break;
    case 3: tft.print("SW3/SW4 toggle  SW2 done"); break;
  }

  tft.setTextColor(TFT_BLUE);
  tft.setCursor(10, 250);
  tft.print("SW1 exit");

  tft.setTextColor(TFT_DARKGREY);
  tft.setCursor(10, 300);
  tft.print("BAT "); tft.print(batteryPercent); tft.print("%  ");
  tft.print("IP "); tft.print(WiFi.localIP());
}

// ================= ALARM =================
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

// ================= BUTTONS =================
bool pressed(int pin, unsigned long &lastTime) {
  if (digitalRead(pin) == LOW && millis() - lastTime > 250) {
    lastTime = millis();
    return true;
  }
  return false;
}

void handleButtons() {
  static unsigned long tDismiss = 0, tMenu = 0, tUp = 0, tDown = 0;

  if (pressed(SW_DISMISS, tDismiss)) {
    if (alarmRinging) {
      alarmRinging = false;
      noTone(BUZZER);
    } else if (menuState != 0) {
      menuState = 0;
      saveAlarm();
      frameCounter = 0;
    }
  }

  if (pressed(SW_MENU, tMenu) && !alarmRinging) {
    menuState = (menuState + 1) % 4;
    if (menuState == 0) saveAlarm();
    frameCounter = 0;
  }

  if (pressed(SW_UP, tUp) && !alarmRinging) {
    if (menuState == 1) alarmHour = (alarmHour + 1) % 24;
    else if (menuState == 2) alarmMinute = (alarmMinute + 1) % 60;
    else if (menuState == 3) { alarmEnabled = !alarmEnabled; saveAlarm(); }
  }

  if (pressed(SW_DOWN, tDown) && !alarmRinging) {
    if (menuState == 1) alarmHour = (alarmHour + 23) % 24;
    else if (menuState == 2) alarmMinute = (alarmMinute + 59) % 60;
    else if (menuState == 3) { alarmEnabled = !alarmEnabled; saveAlarm(); }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(SW_DISMISS, INPUT_PULLUP);
  pinMode(SW_MENU, INPUT_PULLUP);
  pinMode(SW_UP, INPUT_PULLUP);
  pinMode(SW_DOWN, INPUT_PULLUP);
  pinMode(BUZZER, OUTPUT);
  pinMode(BATTERY_PIN, INPUT);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  initStars();
  loadAlarm();

  // Boot splash
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("BLARE");

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

  readBattery();
  fetchWeather();
  lastWeatherFetch = millis();
  lastBatteryCheck = millis();
}

// ================= LOOP =================
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

  if (millis() - lastBatteryCheck > BATTERY_INTERVAL) {
    readBattery();
    lastBatteryCheck = millis();
  }

  if (menuState == 0) {
    if (millis() - lastStarFrame > 80) {
      drawIdleScreen();
      lastStarFrame = millis();
    }
  } else {
    drawMenuScreen();
    delay(150);
    frameCounter = 0;
  }
}
