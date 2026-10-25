// Nursery air monitor: BME680 gas-resistance drop detector with web UI.
// Board: SparkFun Thing Plus C (ESP32). Sensor on the Qwiic port (SDA=23, SCL=22).

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>
#include "secrets.h"
#include "web_ui.h"

namespace cfg
{
  constexpr uint8_t kSdaPin = 23;
  constexpr uint8_t kSclPin = 22;
  constexpr uint8_t kBmeAddr = 0x77; // SparkFun default; 0x76 if the jumper is bridged

  constexpr uint32_t kSamplePeriodMs = 10000;
  constexpr float kFastAlpha = 0.15f;   // ~1 min time constant
  constexpr float kBaseAlpha = 0.0005f; // ~5.5 h time constant

  constexpr uint32_t kHistEvery = 6; // store one history point per minute
  constexpr size_t kHistSize = 720;  // 12 hours
  constexpr size_t kEventSize = 32;
} // namespace cfg

// ---------- Settings (persisted in NVS) ----------
struct Settings
{
  float trigger = 0.60f; // alert when fast/baseline falls below this
  float clear = 0.80f;   // re-arm when it recovers above this
  uint8_t count = 6;     // consecutive samples below trigger
  uint16_t cooldownMin = 30;
  uint16_t warmupMin = 60;
  bool notifyEnabled = true;
  char ntfyUrl[128] = NTFY_URL;
};

static Settings settings;
static Preferences prefs;

static void loadSettings()
{
  prefs.begin("nursery", false);
  settings.trigger = prefs.getFloat("trig", settings.trigger);
  settings.clear = prefs.getFloat("clr", settings.clear);
  settings.count = prefs.getUChar("cnt", settings.count);
  settings.cooldownMin = prefs.getUShort("cool", settings.cooldownMin);
  settings.warmupMin = prefs.getUShort("warm", settings.warmupMin);
  settings.notifyEnabled = prefs.getBool("notif", settings.notifyEnabled);
  const String url = prefs.getString("ntfy", settings.ntfyUrl);
  strlcpy(settings.ntfyUrl, url.c_str(), sizeof settings.ntfyUrl);
  prefs.end();
}

static void saveSettings()
{
  prefs.begin("nursery", false);
  prefs.putFloat("trig", settings.trigger);
  prefs.putFloat("clr", settings.clear);
  prefs.putUChar("cnt", settings.count);
  prefs.putUShort("cool", settings.cooldownMin);
  prefs.putUShort("warm", settings.warmupMin);
  prefs.putBool("notif", settings.notifyEnabled);
  prefs.putString("ntfy", settings.ntfyUrl);
  prefs.end();
}

// ---------- Runtime state ----------
struct DetectorState
{
  float fast = 0.0f;
  float baseline = 0.0f;
  uint32_t samples = 0;
  uint8_t belowCount = 0;
  bool alerting = false;
  uint32_t lastAlertMs = 0;
  bool everAlerted = false;
};

struct Reading
{
  float temp = 0, hum = 0, pres = 0, gas = 0;
};

struct Point
{
  float gas, base, temp, hum;
};

struct Event
{
  uint32_t t;   // uptime seconds
  uint8_t type; // 0 = alert, 1 = manual diaper change
};

static Adafruit_BME680 bme(&Wire);
static WebServer server(80);
static DetectorState st;
static Reading last;
static uint32_t lastSampleMs = 0;

static Point hist[cfg::kHistSize];
static size_t histHead = 0, histCount = 0;
static Event events[cfg::kEventSize];
static size_t evHead = 0, evCount = 0;

static uint32_t uptimeSec()
{
  return static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
}

static void addEvent(uint8_t type)
{
  events[evHead] = {uptimeSec(), type};
  evHead = (evHead + 1) % cfg::kEventSize;
  if (evCount < cfg::kEventSize)
    evCount++;
}

static void pushHistory()
{
  hist[histHead] = {st.fast, st.baseline, last.temp, last.hum};
  histHead = (histHead + 1) % cfg::kHistSize;
  if (histCount < cfg::kHistSize)
    histCount++;
}

// ---------- Networking ----------
static void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    // 201 = AP not found, 202/15/204 = auth or handshake failure, 2 = auth expired
    Serial.printf("WiFi disconnected, reason %d\n", info.wifi_sta_disconnected.reason);
  }
  else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
  {
    Serial.println("WiFi got IP");
  }
}

static void printHex(const char *s)
{
  for (const char *p = s; *p; ++p)
    Serial.printf("%02X ", static_cast<uint8_t>(*p));
  Serial.println();
}

// Prints what the ESP32 actually sees, to compare with the configured SSID.
static void wifiDiagnostics()
{
  Serial.println("---- WiFi diagnostics ----");
  Serial.printf("Configured SSID: [%s] (%u bytes)\n", WIFI_SSID,
                static_cast<unsigned>(strlen(WIFI_SSID)));
  Serial.print("Configured hex:  ");
  printHex(WIFI_SSID);
  for (const char *p = WIFI_SSID; *p; ++p)
  {
    if (static_cast<uint8_t>(*p) >= 0x80)
    {
      Serial.println("WARNING: SSID has non-ASCII bytes (curly apostrophe?). "
                     "Compare the hex with the scan below.");
      break;
    }
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(200);
  const int n = WiFi.scanNetworks();
  Serial.printf("Scan found %d networks\n", n);
  for (int i = 0; i < n; i++)
  {
    const String ssid = WiFi.SSID(i);
    const bool exact = ssid == WIFI_SSID;
    Serial.printf("%s [%s] ch%d %ddBm enc=%d\n", exact ? "MATCH" : "     ", ssid.c_str(),
                  WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i));
    if (!exact && (ssid.indexOf("Stink") >= 0 || ssid.indexOf("Fart") >= 0))
    {
      Serial.print("  near-match hex: ");
      printHex(ssid.c_str());
    }
  }
  WiFi.scanDelete();
  Serial.println("--------------------------");
}

static void connectWifi(uint32_t timeoutMs = 15000)
{
  if (WiFi.status() == WL_CONNECTED)
    return;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs)
    delay(250);
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("WiFi connected, IP ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.printf("WiFi unavailable (status %d)\n", WiFi.status());
  }
}

static bool notify(const String &title, const String &body)
{
  connectWifi();
  if (WiFi.status() != WL_CONNECTED)
    return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, settings.ntfyUrl))
    return false;
  http.addHeader("Title", title);
  const int code = http.POST(body);
  Serial.printf("notify -> HTTP %d\n", code);
  http.end();
  return code >= 200 && code < 300;
}

// ---------- Sensor + detector ----------
static bool bmeBegin(uint8_t addr)
{
  if (!bme.begin(addr))
    return false;
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);
  return true;
}

// Scans the bus on one SDA/SCL pair and reports each device's chip ID (BME680 = 0x61).
static int i2cScan(int sda, int scl)
{
  Wire.end();
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  Serial.printf("I2C scan on SDA=%d SCL=%d\n", sda, scl);
  int found = 0;
  for (uint8_t a = 1; a < 127; a++)
  {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0)
      continue;
    found++;
    int id = -1;
    Wire.beginTransmission(a);
    Wire.write(0xD0);
    if (Wire.endTransmission(false) == 0 && Wire.requestFrom(a, static_cast<uint8_t>(1)) == 1)
    {
      id = Wire.read();
    }
    Serial.printf("  device at 0x%02X, reg 0xD0 = 0x%02X\n", a, id & 0xFF);
  }
  if (!found)
    Serial.println("  no devices responded");
  return found;
}

static bool initSensor()
{
  Wire.begin(cfg::kSdaPin, cfg::kSclPin);
  if (bmeBegin(cfg::kBmeAddr))
    return true;

  // Failed: the library may have reset the I2C pins, so re-init and look around.
  Serial.println("BME680 init failed, running I2C diagnostics");
  const int pins[][2] = {{cfg::kSdaPin, cfg::kSclPin}, {21, 22}, {22, 23}};
  for (const auto &pr : pins)
  {
    if (i2cScan(pr[0], pr[1]) == 0)
      continue;
    Wire.end();
    Wire.begin(pr[0], pr[1]);
    for (uint8_t addr : {uint8_t{0x77}, uint8_t{0x76}})
    {
      if (bmeBegin(addr))
      {
        Serial.printf("BME680 found at 0x%02X on SDA=%d SCL=%d\n", addr, pr[0], pr[1]);
        return true;
      }
    }
  }
  Wire.end();
  Wire.begin(cfg::kSdaPin, cfg::kSclPin);
  return false;
}

static uint32_t warmupSamples()
{
  return static_cast<uint32_t>(settings.warmupMin) * 60000UL / cfg::kSamplePeriodMs;
}

static float currentRatio()
{
  return st.baseline > 0.0f ? st.fast / st.baseline : 1.0f;
}

// Returns true when a new odor alert should fire.
static bool updateDetector(float gasOhms)
{
  if (st.samples == 0)
  {
    st.fast = gasOhms;
    st.baseline = gasOhms;
  }
  st.samples++;
  st.fast += cfg::kFastAlpha * (gasOhms - st.fast);

  // Freeze the baseline during an event so it doesn't "learn" the smell.
  if (!st.alerting && st.belowCount == 0)
  {
    st.baseline += cfg::kBaseAlpha * (gasOhms - st.baseline);
  }

  if (st.samples < warmupSamples())
    return false;

  const float ratio = currentRatio();
  if (ratio < settings.trigger)
  {
    if (st.belowCount < 255)
      st.belowCount++;
  }
  else
  {
    st.belowCount = 0;
  }

  if (st.alerting)
  {
    if (ratio > settings.clear)
      st.alerting = false; // re-arm
    return false;
  }

  const uint32_t cooldownMs = static_cast<uint32_t>(settings.cooldownMin) * 60000UL;
  const bool cooled = !st.everAlerted || (millis() - st.lastAlertMs > cooldownMs);
  if (st.belowCount >= settings.count && cooled)
  {
    st.alerting = true;
    st.everAlerted = true;
    st.lastAlertMs = millis();
    return true;
  }
  return false;
}

// ---------- Web handlers ----------
static bool authed()
{
  if (server.authenticate(WEB_USER, WEB_PASS))
    return true;
  server.requestAuthentication();
  return false;
}

static void sendError(int code, const char *msg)
{
  char buf[96];
  snprintf(buf, sizeof buf, "{\"error\":\"%s\"}", msg);
  server.send(code, "application/json", buf);
}

static void handleRoot()
{
  if (!authed())
    return;
  server.send(200, "text/html", INDEX_HTML);
}

static void handleStatus()
{
  if (!authed())
    return;
  const uint32_t ws = warmupSamples();
  const uint32_t warmLeft =
      st.samples < ws ? (ws - st.samples) * cfg::kSamplePeriodMs / 1000UL : 0;
  char buf[320];
  snprintf(buf, sizeof buf,
           "{\"up\":%lu,\"temp\":%.2f,\"hum\":%.1f,\"pres\":%.1f,\"gas\":%.0f,"
           "\"fast\":%.0f,\"base\":%.0f,\"ratio\":%.3f,\"alert\":%s,\"warm\":%lu,"
           "\"rssi\":%d,\"heap\":%u}",
           static_cast<unsigned long>(uptimeSec()), last.temp, last.hum, last.pres,
           last.gas, st.fast, st.baseline, currentRatio(), st.alerting ? "true" : "false",
           static_cast<unsigned long>(warmLeft), WiFi.RSSI(),
           static_cast<unsigned>(ESP.getFreeHeap()));
  server.send(200, "application/json", buf);
}

static void flushIfLarge(String &out, bool force = false)
{
  if (force || out.length() > 1000)
  {
    server.sendContent(out);
    out = "";
  }
}

static void appendSeries(String &out, const char *key, float Point::*field, const char *fmt)
{
  out += ",\"";
  out += key;
  out += "\":[";
  char buf[24];
  const size_t oldest = (histHead + cfg::kHistSize - histCount) % cfg::kHistSize;
  for (size_t i = 0; i < histCount; i++)
  {
    const Point &p = hist[(oldest + i) % cfg::kHistSize];
    snprintf(buf, sizeof buf, fmt, p.*field);
    if (i)
      out += ',';
    out += buf;
    flushIfLarge(out);
  }
  out += ']';
}

static void handleHistory()
{
  if (!authed())
    return;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");

  String out;
  out.reserve(1200);
  char buf[64];
  snprintf(buf, sizeof buf, "{\"period\":%u,\"n\":%u",
           static_cast<unsigned>(cfg::kSamplePeriodMs / 1000 * cfg::kHistEvery),
           static_cast<unsigned>(histCount));
  out += buf;
  appendSeries(out, "gas", &Point::gas, "%.0f");
  appendSeries(out, "base", &Point::base, "%.0f");
  appendSeries(out, "temp", &Point::temp, "%.1f");
  appendSeries(out, "hum", &Point::hum, "%.0f");

  out += ",\"events\":[";
  const uint32_t now = uptimeSec();
  const size_t oldest = (evHead + cfg::kEventSize - evCount) % cfg::kEventSize;
  for (size_t i = 0; i < evCount; i++)
  {
    const Event &e = events[(oldest + i) % cfg::kEventSize];
    snprintf(buf, sizeof buf, "%s[%lu,%u]", i ? "," : "",
             static_cast<unsigned long>(now - e.t), e.type);
    out += buf;
  }
  out += "]}";
  flushIfLarge(out, true);
  server.sendContent("");
}

static void handleConfigGet()
{
  if (!authed())
    return;
  char buf[320];
  snprintf(buf, sizeof buf,
           "{\"trigger\":%.2f,\"clear\":%.2f,\"count\":%u,\"cooldown\":%u,"
           "\"warmup\":%u,\"notify\":%s,\"ntfy\":\"%s\"}",
           settings.trigger, settings.clear, settings.count, settings.cooldownMin,
           settings.warmupMin, settings.notifyEnabled ? "true" : "false", settings.ntfyUrl);
  server.send(200, "application/json", buf);
}

static bool safeUrl(const String &url)
{
  if (url.length() == 0 || url.length() >= sizeof settings.ntfyUrl)
    return false;
  if (!url.startsWith("http://"))
    return false; // HTTPS would need a TLS client
  for (size_t i = 0; i < url.length(); i++)
  {
    const char c = url[i];
    if (c < 0x20 || c == '"' || c == '\\')
      return false;
  }
  return true;
}

static void handleConfigPost()
{
  if (!authed())
    return;
  Settings n = settings;
  if (server.hasArg("trigger"))
    n.trigger = server.arg("trigger").toFloat();
  if (server.hasArg("clear"))
    n.clear = server.arg("clear").toFloat();
  if (server.hasArg("count"))
    n.count = constrain(server.arg("count").toInt(), 0, 255);
  if (server.hasArg("cooldown"))
    n.cooldownMin = constrain(server.arg("cooldown").toInt(), 0, 65535);
  if (server.hasArg("warmup"))
    n.warmupMin = constrain(server.arg("warmup").toInt(), 0, 65535);
  if (server.hasArg("notify"))
    n.notifyEnabled = server.arg("notify") == "1";

  if (n.trigger < 0.2f || n.trigger > 0.95f)
    return sendError(400, "Threshold must be 0.20-0.95");
  if (n.clear <= n.trigger || n.clear > 1.0f)
    return sendError(400, "Re-arm must be above threshold and at most 1.0");
  if (n.count < 1 || n.count > 60)
    return sendError(400, "Sample count must be 1-60");
  if (n.cooldownMin < 1 || n.cooldownMin > 1440)
    return sendError(400, "Cooldown must be 1-1440 min");
  if (n.warmupMin < 5 || n.warmupMin > 1440)
    return sendError(400, "Warm-up must be 5-1440 min");

  if (server.hasArg("ntfy"))
  {
    const String url = server.arg("ntfy");
    if (!safeUrl(url))
      return sendError(400, "ntfy URL must be a plain http:// URL");
    strlcpy(n.ntfyUrl, url.c_str(), sizeof n.ntfyUrl);
  }

  settings = n;
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleMark()
{
  if (!authed())
    return;
  addEvent(1);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleTest()
{
  if (!authed())
    return;
  if (notify("Nursery air monitor", "Test notification"))
  {
    server.send(200, "application/json", "{\"ok\":true}");
  }
  else
  {
    sendError(502, "Notification failed (check WiFi and URL)");
  }
}

static void handleReset()
{
  if (!authed())
    return;
  st.baseline = st.fast;
  st.belowCount = 0;
  st.alerting = false;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void startWebServer()
{
  if (MDNS.begin("nursery-air"))
    Serial.println("mDNS: http://nursery-air.local");
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/mark", HTTP_POST, handleMark);
  server.on("/api/test", HTTP_POST, handleTest);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.begin();
}

// ---------- Arduino entry points ----------
void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\nNursery air monitor starting");

  loadSettings();
  if (!initSensor())
  {
    Serial.println("BME680 not found. Check Qwiic cable and I2C address.");
    while (true)
      delay(1000);
  }
  WiFi.onEvent(onWifiEvent);
  connectWifi();
  if (WiFi.status() != WL_CONNECTED)
    wifiDiagnostics();
  startWebServer();
  Serial.println("ms,temp_C,humidity_pct,pressure_hPa,gas_ohm,fast_ohm,baseline_ohm,ratio,alert");
}

void loop()
{
  server.handleClient();

  // Keep trying to get back on WiFi if it drops or never connected.
  static uint32_t lastWifiTry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 30000)
  {
    lastWifiTry = millis();
    connectWifi(8000);
  }

  const uint32_t now = millis();
  if (now - lastSampleMs < cfg::kSamplePeriodMs)
  {
    delay(2);
    return;
  }
  lastSampleMs = now;

  if (!bme.performReading())
  {
    Serial.println("Sensor read failed");
    return;
  }

  last = {bme.temperature, bme.humidity, bme.pressure / 100.0f,
          static_cast<float>(bme.gas_resistance)};

  bool fire = false;
  if (last.gas > 0.0f)
  {
    fire = updateDetector(last.gas);
    if (st.samples % cfg::kHistEvery == 0)
      pushHistory();
  }

  Serial.printf("%lu,%.2f,%.2f,%.2f,%.0f,%.0f,%.0f,%.3f,%d\n",
                static_cast<unsigned long>(now), last.temp, last.hum, last.pres, last.gas,
                st.fast, st.baseline, currentRatio(), st.alerting ? 1 : 0);

  if (fire)
  {
    addEvent(0);
    if (settings.notifyEnabled)
    {
      notify("Nursery odor detected",
             String("Air quality dropped; maybe a diaper change. Temp ") +
                 String(last.temp, 1) + " C, humidity " + String(last.hum, 0) + "%");
    }
  }
}
