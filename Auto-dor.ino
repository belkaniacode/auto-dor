/*
 * =============================================================
 *  Auto-dor  --  Управление откатными воротами Alutech RTO-500
 * =============================================================
 *
 *  Плата:  ESP32-DevKitC V2  (ESP32-WROOM-32 / NodeMCU-32S 38pin)
 *  IDE:    Arduino IDE  (плата "ESP32 Dev Module")
 *
 *  Управление:
 *  - Физические кнопки (GPIO 14, GPIO 12)
 *  - Wi-Fi веб-интерфейс (телефон)
 *
 *  Два реле    -> мотор открытия / мотор закрытия  (230В AC!)
 *  Два LED     -> индикация направления движения (мигают)
 *  Два концевика -> крайние положения ворот
 *
 *  !!  КРИТИЧЕСКАЯ ЗАЩИТА  !!
 *  Оба реле НИКОГДА не включаются одновременно.
 *  Между переключением направления — пауза минимум 1 секунда.
 *
 *  Логика кнопок (физических и веб):
 *  - "Открыть" в покое       -> ворота открываются
 *  - "Закрыть" в покое       -> ворота закрываются
 *  - Любая команда при движении -> СТОП
 *  - "Открыть" а концевик "Открыто" зажат -> ничего
 *  - "Закрыть" а концевик "Закрыто" зажат -> ничего
 *
 *  После включения питания — реле выключены, ждём команды.
 * =============================================================
 */

#include <WiFi.h>
#include <WebServer.h>

// ========================  WI-FI  ============================
// !!! ЗАПОЛНИТЬ ПЕРЕД ЗАЛИВКОЙ !!!
#define WIFI_SSID     "ИМЯ_ВАШЕЙ_СЕТИ"
#define WIFI_PASSWORD "ПАРОЛЬ_ВАШЕЙ_СЕТИ"

// Авторизация веб-интерфейса (HTTP Basic Auth)
#define WEB_LOGIN     "admin"
#define WEB_PASS      "gate123"

// ========================  ПИНЫ  =============================

// Реле (active LOW — реле срабатывает при LOW на входе)
#define RELAY_OPEN_PIN   26   // Реле 1 — открытие
#define RELAY_CLOSE_PIN  27   // Реле 2 — закрытие

// Светодиоды
#define LED_OPEN_PIN     25   // LED — ворота открываются
#define LED_CLOSE_PIN    13   // LED — ворота закрываются

// Кнопки (INPUT_PULLUP, нажатие = LOW)
#define BTN_OPEN_PIN     14   // Кнопка "Открыть"
#define BTN_CLOSE_PIN    12   // Кнопка "Закрыть"

// Концевые выключатели (INPUT_PULLUP, нормально замкнутые NC)
// NC: обычное состояние = замкнут на GND = LOW
//     сработал (ворота в крайнем положении) = разомкнут = HIGH
#define LIMIT_OPEN_PIN   32   // Концевик "Ворота открыты"
#define LIMIT_CLOSE_PIN  33   // Концевик "Ворота закрыты"

// ========================  НАСТРОЙКИ  ========================

#define DEBOUNCE_MS         80     // Дебаунс кнопок, мс
#define RELAY_SWITCH_DELAY  1000   // Пауза между сменой направления, мс
#define MOTOR_TIMEOUT_MS    60000  // Таймаут работы мотора, мс (60 сек)
#define LED_BLINK_INTERVAL  300    // Интервал мигания LED, мс
#define WIFI_CONNECT_TIMEOUT 15000 // Таймаут подключения Wi-Fi, мс

// ========================  СОСТОЯНИЯ  ========================

enum GateState {
  STATE_IDLE,       // Ворота стоят, ждём команду
  STATE_OPENING,    // Мотор крутит на открытие
  STATE_CLOSING     // Мотор крутит на закрытие
};

// ========================  ПЕРЕМЕННЫЕ  =======================

GateState state = STATE_IDLE;

unsigned long motorStartTime   = 0;   // Когда мотор включился
unsigned long lastRelayOffTime = 0;   // Когда последнее реле выключилось
unsigned long lastLedToggle    = 0;   // Для мигания LED
bool ledState = false;                // Текущее состояние LED (вкл/выкл)

// Дебаунс кнопок — запоминаем ВРЕМЯ нажатия и предыдущее состояние
unsigned long lastBtnOpenChange  = 0;
unsigned long lastBtnCloseChange = 0;
bool prevBtnOpen  = false;   // предыдущее состояние кнопки "Открыть"
bool prevBtnClose = false;   // предыдущее состояние кнопки "Закрыть"

// Wi-Fi
WebServer server(80);
bool wifiConnected = false;

// ========================  ФУНКЦИИ БЕЗОПАСНОСТИ  =============

void allRelaysOff() {
  digitalWrite(RELAY_OPEN_PIN,  HIGH);  // HIGH = реле ВЫКЛ (active LOW)
  digitalWrite(RELAY_CLOSE_PIN, HIGH);
  lastRelayOffTime = millis();
}

void allLedsOff() {
  digitalWrite(LED_OPEN_PIN,  LOW);
  digitalWrite(LED_CLOSE_PIN, LOW);
  ledState = false;
}

void stopMotor(const char* reason) {
  allRelaysOff();
  allLedsOff();
  state = STATE_IDLE;
  Serial.print("СТОП: ");
  Serial.println(reason);
}

// ========================  ВКЛЮЧЕНИЕ РЕЛЕ  ===================

bool startOpening() {
  // NC-концевик: HIGH = сработал (разомкнут), LOW = норма (замкнут)
  if (digitalRead(LIMIT_OPEN_PIN) == HIGH) {
    Serial.println("Ворота уже открыты (концевик зажат), игнорирую");
    return false;
  }

  if (millis() - lastRelayOffTime < RELAY_SWITCH_DELAY) {
    Serial.println("Ожидание паузы перед сменой направления...");
    return false;
  }

  allRelaysOff();
  delayMicroseconds(100);
  digitalWrite(RELAY_CLOSE_PIN, HIGH);
  digitalWrite(RELAY_OPEN_PIN, LOW);

  if (digitalRead(RELAY_OPEN_PIN) == LOW && digitalRead(RELAY_CLOSE_PIN) == LOW) {
    allRelaysOff();
    allLedsOff();
    state = STATE_IDLE;
    Serial.println("!!! АВАРИЙНАЯ ОСТАНОВКА: обнаружено два реле одновременно !!!");
    return false;
  }

  motorStartTime = millis();
  state = STATE_OPENING;
  Serial.println("-> Мотор: ОТКРЫТИЕ");
  return true;
}

bool startClosing() {
  if (digitalRead(LIMIT_CLOSE_PIN) == HIGH) {
    Serial.println("Ворота уже закрыты (концевик зажат), игнорирую");
    return false;
  }

  if (millis() - lastRelayOffTime < RELAY_SWITCH_DELAY) {
    Serial.println("Ожидание паузы перед сменой направления...");
    return false;
  }

  allRelaysOff();
  delayMicroseconds(100);
  digitalWrite(RELAY_OPEN_PIN, HIGH);
  digitalWrite(RELAY_CLOSE_PIN, LOW);

  if (digitalRead(RELAY_OPEN_PIN) == LOW && digitalRead(RELAY_CLOSE_PIN) == LOW) {
    allRelaysOff();
    allLedsOff();
    state = STATE_IDLE;
    Serial.println("!!! АВАРИЙНАЯ ОСТАНОВКА: обнаружено два реле одновременно !!!");
    return false;
  }

  motorStartTime = millis();
  state = STATE_CLOSING;
  Serial.println("-> Мотор: ЗАКРЫТИЕ");
  return true;
}

// ========================  ВЕБ-ИНТЕРФЕЙС  ===================

const char* getStateText() {
  switch (state) {
    case STATE_OPENING: return "opening";
    case STATE_CLOSING: return "closing";
    default:            return "idle";
  }
}

const char* getStateRus() {
  switch (state) {
    case STATE_OPENING: return "Открываются...";
    case STATE_CLOSING: return "Закрываются...";
    default:            return "Стоят";
  }
}

bool checkAuth() {
  if (!server.authenticate(WEB_LOGIN, WEB_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

// GET /status — JSON статус (для автообновления на странице)
void handleStatus() {
  String json = "{\"state\":\"";
  json += getStateText();
  json += "\",\"stateRus\":\"";
  json += getStateRus();
  json += "\"}";
  server.send(200, "application/json", json);
}

// GET /action?cmd=open|close|stop — выполнить команду
void handleCommand() {
  if (!checkAuth()) return;

  String cmd = server.arg("cmd");
  String result = "ok";

  if (cmd == "open") {
    if (state == STATE_OPENING || state == STATE_CLOSING) {
      stopMotor("веб: остановка при движении");
      result = "stopped";
    } else {
      Serial.println("Веб: команда ОТКРЫТЬ");
      if (!startOpening()) result = "blocked";
    }
  } else if (cmd == "close") {
    if (state == STATE_OPENING || state == STATE_CLOSING) {
      stopMotor("веб: остановка при движении");
      result = "stopped";
    } else {
      Serial.println("Веб: команда ЗАКРЫТЬ");
      if (!startClosing()) result = "blocked";
    }
  } else if (cmd == "stop") {
    if (state != STATE_IDLE) {
      stopMotor("веб: команда СТОП");
    }
    result = "stopped";
  } else {
    result = "unknown";
  }

  String json = "{\"result\":\"" + result + "\",\"state\":\"";
  json += getStateText();
  json += "\",\"stateRus\":\"";
  json += getStateRus();
  json += "\"}";
  server.send(200, "application/json", json);
}

// GET / — главная страница с кнопками
void handleRoot() {
  if (!checkAuth()) return;

  String html = R"rawhtml(<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, user-scalable=no">
<title>Auto-dor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,sans-serif;background:#1a1a2e;color:#eee;
  display:flex;flex-direction:column;align-items:center;
  justify-content:center;min-height:100vh;padding:20px}
h1{font-size:1.5em;margin-bottom:8px;color:#e94560}
#status{font-size:1.2em;margin-bottom:30px;padding:10px 24px;
  border-radius:12px;background:#16213e;min-width:200px;text-align:center}
.buttons{display:flex;flex-direction:column;gap:16px;width:100%;max-width:320px}
.btn{font-size:1.4em;padding:20px;border:none;border-radius:16px;
  color:#fff;cursor:pointer;font-weight:bold;
  transition:transform 0.1s,opacity 0.1s;user-select:none}
.btn:active{transform:scale(0.95);opacity:0.8}
.btn-open{background:#0f3460}
.btn-stop{background:#e94560}
.btn-close{background:#533483}
.btn:disabled{opacity:0.4;transform:none}
.dot{display:inline-block;width:12px;height:12px;border-radius:50%;
  margin-right:8px;vertical-align:middle}
.dot-idle{background:#53d769}
.dot-opening{background:#007aff;animation:pulse 0.6s infinite alternate}
.dot-closing{background:#af52de;animation:pulse 0.6s infinite alternate}
@keyframes pulse{from{opacity:1}to{opacity:0.3}}
</style>
</head>
<body>
<h1>Auto-dor</h1>
<div id="status"><span class="dot dot-idle"></span>Стоят</div>
<div class="buttons">
  <button class="btn btn-open" onclick="send('open')">ОТКРЫТЬ</button>
  <button class="btn btn-stop" onclick="send('stop')">СТОП</button>
  <button class="btn btn-close" onclick="send('close')">ЗАКРЫТЬ</button>
</div>
<script>
function send(cmd){
  fetch('/action?cmd='+cmd)
    .then(r=>r.json())
    .then(d=>upd(d.state,d.stateRus))
    .catch(()=>{});
}
function upd(s,t){
  var el=document.getElementById('status');
  el.innerHTML='<span class="dot dot-'+s+'"></span>'+t;
}
setInterval(function(){
  fetch('/status')
    .then(r=>r.json())
    .then(d=>upd(d.state,d.stateRus))
    .catch(()=>{});
},2000);
</script>
</body>
</html>)rawhtml";

  server.send(200, "text/html", html);
}

// ========================  SETUP  ============================

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== Auto-dor: Управление воротами ===");
  Serial.println("Плата: ESP32-DevKitC V2 (ESP32-WROOM-32)");
  Serial.println();

  // --- Реле: выход, сразу ВЫКЛЮЧИТЬ (HIGH = OFF для active LOW) ---
  pinMode(RELAY_OPEN_PIN,  OUTPUT);
  pinMode(RELAY_CLOSE_PIN, OUTPUT);
  allRelaysOff();  // ПЕРВЫМ ДЕЛОМ — выключить реле!

  // --- Светодиоды: выход ---
  pinMode(LED_OPEN_PIN,  OUTPUT);
  pinMode(LED_CLOSE_PIN, OUTPUT);
  allLedsOff();

  // --- Кнопки: вход с подтяжкой ---
  pinMode(BTN_OPEN_PIN,  INPUT_PULLUP);
  pinMode(BTN_CLOSE_PIN, INPUT_PULLUP);

  // --- Концевики: вход с подтяжкой ---
  pinMode(LIMIT_OPEN_PIN,  INPUT_PULLUP);
  pinMode(LIMIT_CLOSE_PIN, INPUT_PULLUP);

  state = STATE_IDLE;

  // --- Wi-Fi: подключение к роутеру ---
  Serial.print("Wi-Fi: подключение к ");
  Serial.print(WIFI_SSID);
  Serial.print(" ");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_CONNECT_TIMEOUT) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.print("Wi-Fi подключён! IP: ");
    Serial.println(WiFi.localIP());
    Serial.println("Откройте в браузере: http://" + WiFi.localIP().toString());
    Serial.print("Логин: ");
    Serial.print(WEB_LOGIN);
    Serial.print(" / Пароль: ");
    Serial.println(WEB_PASS);
  } else {
    Serial.println("Wi-Fi НЕ подключён. Работаем только с кнопками.");
  }

  // --- Веб-сервер ---
  server.on("/", handleRoot);
  server.on("/action", handleCommand);
  server.on("/status", handleStatus);
  server.begin();

  Serial.println();
  Serial.println("Инициализация завершена. Ожидание команд...");
  Serial.println();
}

// ========================  LOOP  =============================

void loop() {
  // --- Wi-Fi: обработка HTTP-запросов (неблокирующий) ---
  server.handleClient();

  unsigned long now = millis();

  // ---- Чтение входов ----
  bool btnOpenRaw   = (digitalRead(BTN_OPEN_PIN)  == LOW);
  bool btnCloseRaw  = (digitalRead(BTN_CLOSE_PIN) == LOW);
  // NC-концевики: HIGH = сработал (разомкнут), LOW = норма (замкнут)
  bool limitOpenHit  = (digitalRead(LIMIT_OPEN_PIN)  == HIGH);
  bool limitCloseHit = (digitalRead(LIMIT_CLOSE_PIN) == HIGH);

  // ---- Дебаунс: определяем ФРОНТ нажатия (было отпущено -> стало нажато) ----
  bool btnOpenEdge  = false;
  bool btnCloseEdge = false;

  if (btnOpenRaw != prevBtnOpen && (now - lastBtnOpenChange > DEBOUNCE_MS)) {
    lastBtnOpenChange = now;
    prevBtnOpen = btnOpenRaw;
    if (btnOpenRaw) btnOpenEdge = true;
  }

  if (btnCloseRaw != prevBtnClose && (now - lastBtnCloseChange > DEBOUNCE_MS)) {
    lastBtnCloseChange = now;
    prevBtnClose = btnCloseRaw;
    if (btnCloseRaw) btnCloseEdge = true;
  }

  // ==========================================================
  //  ПРИОРИТЕТ 1:  КОНЦЕВИКИ во время движения
  //  Проверяем ТОЛЬКО концевик в направлении движения.
  // ==========================================================
  if (state == STATE_OPENING && limitOpenHit) {
    stopMotor("концевик ОТКРЫТО сработал, ворота открыты");
    return;
  }
  if (state == STATE_CLOSING && limitCloseHit) {
    stopMotor("концевик ЗАКРЫТО сработал, ворота закрыты");
    return;
  }

  // ==========================================================
  //  ПРИОРИТЕТ 2:  ТАЙМАУТ — мотор работает слишком долго
  // ==========================================================
  if ((state == STATE_OPENING || state == STATE_CLOSING) &&
      (now - motorStartTime > MOTOR_TIMEOUT_MS)) {
    stopMotor("ТАЙМАУТ: мотор работал > 60 сек!");
    return;
  }

  // ==========================================================
  //  ПРИОРИТЕТ 3:  ЗАЩИТА ОТ ДВУХ РЕЛЕ ОДНОВРЕМЕННО
  // ==========================================================
  if (digitalRead(RELAY_OPEN_PIN) == LOW && digitalRead(RELAY_CLOSE_PIN) == LOW) {
    Serial.println("!!! КРИТИЧЕСКАЯ ЗАЩИТА: два реле одновременно — ВЫКЛЮЧАЮ !!!");
    allRelaysOff();
    allLedsOff();
    state = STATE_IDLE;
    return;
  }

  // ==========================================================
  //  ОБРАБОТКА ФИЗИЧЕСКИХ КНОПОК
  // ==========================================================

  if (btnOpenEdge) {
    if (state == STATE_OPENING || state == STATE_CLOSING) {
      stopMotor("кнопка нажата при движении — остановка");
    } else if (state == STATE_IDLE) {
      Serial.println("Кнопка ОТКРЫТЬ нажата");
      startOpening();
    }
  }

  if (btnCloseEdge) {
    if (state == STATE_OPENING || state == STATE_CLOSING) {
      stopMotor("кнопка нажата при движении — остановка");
    } else if (state == STATE_IDLE) {
      Serial.println("Кнопка ЗАКРЫТЬ нажата");
      startClosing();
    }
  }

  // ==========================================================
  //  МИГАНИЕ СВЕТОДИОДОВ
  // ==========================================================
  if (state == STATE_OPENING || state == STATE_CLOSING) {
    if (now - lastLedToggle >= LED_BLINK_INTERVAL) {
      lastLedToggle = now;
      ledState = !ledState;

      if (state == STATE_OPENING) {
        digitalWrite(LED_OPEN_PIN,  ledState ? HIGH : LOW);
        digitalWrite(LED_CLOSE_PIN, LOW);
      } else {
        digitalWrite(LED_CLOSE_PIN, ledState ? HIGH : LOW);
        digitalWrite(LED_OPEN_PIN,  LOW);
      }
    }
  }
}
