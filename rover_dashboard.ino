/*************************************************************************
 *  LinkX Smart Rover — Obstacle-Avoidance WebSocket Dashboard
 *  Board   : Gbro STEM AI Robotics ESP32-S3 (LinkX Dev Board)
 *  Author  : Generated project (edit SSID/PASS below before flashing)
 *
 *  FEATURES
 *   - AP mode dashboard @ 192.168.4.1 (STA fallback optional, see WIFI_STA_*)
 *   - WebSocket live updates (distance, obstacle state, events) @ 5 Hz
 *   - Dual motor driver control (manual + auto obstacle-avoid mode)
 *   - HC-SR04 ultrasonic obstacle sensor with runtime-adjustable threshold
 *   - RGB LED status indication + buzzer alert patterns
 *   - Event logging to LittleFS (/log.csv) with CSV download from dashboard
 *   - OTA firmware updates via Arduino IDE / PlatformIO (ArduinoOTA)
 *   - Dead-man safety timeout (auto-stop if no command for 800ms while moving)
 *
 *  LIBRARIES REQUIRED (Library Manager or PlatformIO):
 *   - ESPAsyncWebServer   (esphome/ESPAsyncWebServer or me-no-dev fork)
 *   - AsyncTCP            (me-no-dev/AsyncTCP, ESP32 core)
 *   - ArduinoJson (v6+)
 *   - LittleFS (bundled with ESP32 board package)
 *   - ArduinoOTA  (bundled with ESP32 board package)
 *
 *  BEFORE FLASHING:
 *   1. Tools > ESP32S3 Dev Module (or your board's exact variant)
 *   2. Tools > Partition Scheme > "Default 4MB with spiffs" (or larger)
 *   3. Upload the /data folder to LittleFS:
 *        Arduino IDE: use "ESP32 Sketch Data Upload" tool (installs via
 *        github.com/lorol/arduino-esp32fs-plugin), OR
 *        PlatformIO: `pio run --target uploadfs`
 *   4. Flash this .ino normally.
 *************************************************************************/

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>

// ================= WIFI CONFIG =================
const char* AP_SSID     = "LinkX-Rover";
const char* AP_PASSWORD = "12345678";      // min 8 chars

// Optional: set these to also join a home WiFi (STA) at the same time.
// Leave STA_SSID empty ("") to stay in AP-only mode.
const char* STA_SSID     = "";
const char* STA_PASSWORD = "";

// ================= MOTOR PINS =================
#define IN1 11
#define IN2 10
#define PWMA 12
#define IN3 46
#define IN4 13
#define PWMB 3

// ================= RGB + BUZZER =================
#define RED_PIN   45
#define GREEN_PIN 48
#define BLUE_PIN  47
#define BUZZER    14

// ================= ULTRASONIC (obstacle sensor) =================
#define TRIG_PIN 35
#define ECHO_PIN 36
#define MAX_DISTANCE_CM 400
#define ECHO_TIMEOUT_US 25000UL   // ~4m max round trip

const int pwmFreq = 5000;
const int pwmResolution = 8;

// ================= STATE =================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

volatile int   distanceCm      = MAX_DISTANCE_CM;
volatile bool  obstacleFlag    = false;
int            thresholdCm     = 15;
bool           autoAvoidMode   = false;
String         currentMotion   = "stop";      // forward/backward/left/right/stop
unsigned long  lastCmdTime     = 0;
const unsigned long DEADMAN_MS = 800;         // safety auto-stop window

unsigned long lastSensorPoll   = 0;
const unsigned long SENSOR_INTERVAL_MS = 100;  // 10 Hz sensor read
unsigned long lastBroadcast    = 0;
const unsigned long BROADCAST_INTERVAL_MS = 200; // 5 Hz dashboard update

// ================= RGB HELPERS =================
void setRGB(int r, int g, int b){
  ledcWrite(RED_PIN, r);
  ledcWrite(GREEN_PIN, g);
  ledcWrite(BLUE_PIN, b);
}
void rgbIdle()      { setRGB(0, 60, 0); }        // dim green
void rgbMoving()    { setRGB(0, 0, 80); }         // blue
void rgbObstacle()  { setRGB(120, 0, 0); }        // red
void rgbBoot()      { setRGB(80, 0, 80); }        // purple

// ================= BUZZER HELPERS (non-blocking-ish, short) =================
void beep(int times, int onMs, int offMs){
  for(int i=0;i<times;i++){
    digitalWrite(BUZZER, HIGH); delay(onMs);
    digitalWrite(BUZZER, LOW);  if(i<times-1) delay(offMs);
  }
}

// ================= MOTOR FUNCTIONS =================
void motorForward(){
  digitalWrite(IN1,HIGH); digitalWrite(IN2,LOW);
  digitalWrite(IN3,HIGH); digitalWrite(IN4,LOW);
  ledcWrite(PWMA,255); ledcWrite(PWMB,255);
}
void motorBackward(){
  digitalWrite(IN1,LOW); digitalWrite(IN2,HIGH);
  digitalWrite(IN3,LOW); digitalWrite(IN4,HIGH);
  ledcWrite(PWMA,255); ledcWrite(PWMB,255);
}
void motorLeft(){
  digitalWrite(IN1,LOW); digitalWrite(IN2,HIGH);
  digitalWrite(IN3,HIGH); digitalWrite(IN4,LOW);
  ledcWrite(PWMA,255); ledcWrite(PWMB,255);
}
void motorRight(){
  digitalWrite(IN1,HIGH); digitalWrite(IN2,LOW);
  digitalWrite(IN3,LOW); digitalWrite(IN4,HIGH);
  ledcWrite(PWMA,255); ledcWrite(PWMB,255);
}
void motorStop(){
  digitalWrite(IN1,LOW); digitalWrite(IN2,LOW);
  digitalWrite(IN3,LOW); digitalWrite(IN4,LOW);
  ledcWrite(PWMA,0); ledcWrite(PWMB,0);
}

void applyMotion(const String &cmd){
  currentMotion = cmd;
  lastCmdTime = millis();
  if(cmd=="forward")       motorForward();
  else if(cmd=="backward") motorBackward();
  else if(cmd=="left")     motorLeft();
  else if(cmd=="right")    motorRight();
  else                     motorStop();
}

// ================= ULTRASONIC READ (with timeout / error handling) =================
int readDistanceCm(){
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if(duration == 0) return MAX_DISTANCE_CM;      // timeout -> treat as "clear"
  int cm = duration / 58;
  if(cm <= 0 || cm > MAX_DISTANCE_CM) cm = MAX_DISTANCE_CM;
  return cm;
}

// ================= LOGGING (LittleFS) =================
void logEvent(const String &text){
  File f = LittleFS.open("/log.csv", "a");
  if(!f){ Serial.println("[ERR] log file open failed"); return; }
  // Cap log file size to avoid filling flash (rotate at ~200KB)
  if(f.size() > 200000){
    f.close();
    LittleFS.remove("/log_old.csv");
    LittleFS.rename("/log.csv", "/log_old.csv");
    f = LittleFS.open("/log.csv", "a");
  }
  f.printf("%lu,%s\n", millis(), text.c_str());
  f.close();
}

void broadcastEvent(const String &text){
  logEvent(text);
  StaticJsonDocument<128> doc;
  doc["event"] = text;
  String out; serializeJson(doc, out);
  ws.textAll(out);
}

// ================= WEBSOCKET HANDLING =================
void handleWsMessage(AsyncWebSocketClient *client, void *arg, uint8_t *data, size_t len){
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if(!(info->final && info->index==0 && info->len==len && info->opcode==WS_TEXT)) return;

  StaticJsonDocument<192> doc;
  DeserializationError err = deserializeJson(doc, data, len);
  if(err){ Serial.println("[ERR] bad JSON from client"); return; }

  const char* cmd = doc["cmd"] | "";
  if(!strcmp(cmd,"forward") || !strcmp(cmd,"backward") || !strcmp(cmd,"left") ||
     !strcmp(cmd,"right")   || !strcmp(cmd,"stop")){
    if(!autoAvoidMode || !strcmp(cmd,"stop")){
      applyMotion(String(cmd));
      digitalWrite(BUZZER, HIGH); delay(15); digitalWrite(BUZZER, LOW); // tiny ack click
    }
  } else if(!strcmp(cmd,"setThreshold")){
    int v = doc["value"] | 15;
    thresholdCm = constrain(v, 5, 60);
    broadcastEvent("Threshold set to " + String(thresholdCm) + "cm");
  } else if(!strcmp(cmd,"setAuto")){
    autoAvoidMode = doc["value"] | false;
    broadcastEvent(autoAvoidMode ? "Auto-Avoid ENABLED" : "Manual mode");
    if(!autoAvoidMode) applyMotion("stop");
  }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len){
  if(type == WS_EVT_CONNECT){
    Serial.printf("[WS] client #%u connected\n", client->id());
    setRGB(0,60,0);
  } else if(type == WS_EVT_DISCONNECT){
    Serial.printf("[WS] client #%u disconnected\n", client->id());
  } else if(type == WS_EVT_DATA){
    handleWsMessage(client, arg, data, len);
  }
}

// ================= AUTO-AVOID LOGIC =================
void autoAvoidTick(){
  if(!autoAvoidMode) return;
  if(obstacleFlag){
    applyMotion("stop");
    // brief reverse + turn to clear the obstacle, then stop and wait
    motorBackward(); delay(300);
    motorRight();    delay(300);
    motorStop();
  } else if(currentMotion == "stop"){
    applyMotion("forward");
  }
}

// ================= SETUP =================
void setup(){
  Serial.begin(115200);
  delay(200);

  pinMode(IN1,OUTPUT); pinMode(IN2,OUTPUT);
  pinMode(IN3,OUTPUT); pinMode(IN4,OUTPUT);
  pinMode(BUZZER,OUTPUT);
  pinMode(TRIG_PIN,OUTPUT);
  pinMode(ECHO_PIN,INPUT);

  ledcAttach(PWMA,pwmFreq,pwmResolution);
  ledcAttach(PWMB,pwmFreq,pwmResolution);
  ledcAttach(RED_PIN,pwmFreq,pwmResolution);
  ledcAttach(GREEN_PIN,pwmFreq,pwmResolution);
  ledcAttach(BLUE_PIN,pwmFreq,pwmResolution);

  rgbBoot();
  motorStop();

  if(!LittleFS.begin(true)){
    Serial.println("[ERR] LittleFS mount failed - formatting");
  } else {
    if(!LittleFS.exists("/log.csv")){
      File f = LittleFS.open("/log.csv","w");
      if(f){ f.println("millis,event"); f.close(); }
    }
  }

  // ---- WiFi: AP always on, STA optional ----
  WiFi.mode(strlen(STA_SSID) ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("[AP] IP: "); Serial.println(WiFi.softAPIP());

  if(strlen(STA_SSID)){
    WiFi.begin(STA_SSID, STA_PASSWORD);
    Serial.print("[STA] connecting");
    unsigned long start = millis();
    while(WiFi.status() != WL_CONNECTED && millis()-start < 8000){
      delay(300); Serial.print(".");
    }
    if(WiFi.status() == WL_CONNECTED){
      Serial.print("\n[STA] IP: "); Serial.println(WiFi.localIP());
    } else {
      Serial.println("\n[STA] failed - continuing in AP-only mode");
    }
  }

  // ---- Web server routes ----
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  server.serveStatic("/log.csv", LittleFS, "/log.csv");

  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "OK");
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    request->send(404, "text/plain", "Not found");
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();

  // ---- OTA ----
  ArduinoOTA.setHostname("linkx-rover");
  ArduinoOTA.onStart([](){ Serial.println("[OTA] start"); rgbBoot(); });
  ArduinoOTA.onEnd([](){ Serial.println("[OTA] done"); });
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] error %u\n", e); });
  ArduinoOTA.begin();

  beep(2, 60, 80);
  rgbIdle();
  logEvent("Boot complete");
  Serial.println("Setup complete. Connect to WiFi \"LinkX-Rover\" and open http://192.168.4.1");
}

// ================= LOOP =================
void loop(){
  ArduinoOTA.handle();
  ws.cleanupClients();

  unsigned long now = millis();

  // --- Sensor poll ---
  if(now - lastSensorPoll >= SENSOR_INTERVAL_MS){
    lastSensorPoll = now;
    distanceCm = readDistanceCm();
    bool wasObstacle = obstacleFlag;
    obstacleFlag = (distanceCm <= thresholdCm);

    if(obstacleFlag && !wasObstacle){
      rgbObstacle();
      beep(3, 80, 80);
      broadcastEvent("Obstacle detected at " + String(distanceCm) + "cm");
    } else if(!obstacleFlag && wasObstacle){
      rgbMoving();
    }
    autoAvoidTick();
  }

  // --- Dead-man safety: stop if no command refresh while manually moving ---
  if(!autoAvoidMode && currentMotion != "stop" && (now - lastCmdTime > DEADMAN_MS)){
    applyMotion("stop");
    broadcastEvent("Safety auto-stop (no signal)");
  }

  // --- RGB idle/moving update (only when not currently flagged as obstacle) ---
  if(!obstacleFlag){
    if(currentMotion == "stop") rgbIdle(); else rgbMoving();
  }

  // --- Broadcast live state to dashboard ---
  if(now - lastBroadcast >= BROADCAST_INTERVAL_MS){
    lastBroadcast = now;
    StaticJsonDocument<192> doc;
    doc["distance"] = distanceCm;
    doc["obstacle"] = obstacleFlag;
    doc["motion"]   = currentMotion;
    doc["auto"]     = autoAvoidMode;
    String out; serializeJson(doc, out);
    ws.textAll(out);
  }
}
