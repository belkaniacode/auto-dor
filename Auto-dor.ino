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
#define WIFI_SSID     "SSID"
#define WIFI_PASSWORD "PASSWORD"

// Статический IP (поменяй под свою сеть)
IPAddress staticIP(192, 168, 1, 50);      // IP адрес ESP32
IPAddress gateway(192, 168, 1, 1);        // Шлюз (IP роутера)
IPAddress subnet(255, 255, 255, 0);       // Маска подсети
IPAddress dns(8, 8, 8, 8);               // DNS (Google)

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
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
  background:#0f1117;color:#e8e8ed;display:flex;flex-direction:column;
  align-items:center;min-height:100vh;padding:16px 16px 32px}
.footer{margin-top:16px;font-size:0.7em;color:rgba(255,255,255,0.25);
  text-align:center;letter-spacing:0.3px}
.footer a{color:rgba(255,255,255,0.35);text-decoration:none}
.footer a:hover{color:rgba(255,255,255,0.5)}
.card{width:100%;max-width:380px;background:linear-gradient(145deg,#141420,#1a1a2e);
  border-radius:24px;padding:28px 24px;border:1px solid rgba(255,255,255,0.06);
  box-shadow:0 8px 32px rgba(0,0,0,0.4)}
.logo{width:140px;height:auto;display:block;margin:0 auto 6px;opacity:0.92}
.divider{height:1px;background:linear-gradient(90deg,transparent,rgba(255,255,255,0.08),transparent);
  margin:16px 0}
#status{font-size:0.95em;padding:12px 20px;border-radius:14px;
  background:rgba(255,255,255,0.04);text-align:center;letter-spacing:0.3px;
  border:1px solid rgba(255,255,255,0.05)}
.buttons{display:flex;flex-direction:column;gap:12px;margin-top:20px}
.btn{font-size:1.1em;padding:18px;border:none;border-radius:14px;
  color:#fff;cursor:pointer;font-weight:600;letter-spacing:0.5px;
  transition:all 0.15s ease;user-select:none;
  box-shadow:0 2px 8px rgba(0,0,0,0.3)}
.btn:active{transform:scale(0.97);filter:brightness(0.85)}
.btn-open{background:linear-gradient(135deg,#1a3a6c,#2563eb)}
.btn-stop{background:linear-gradient(135deg,#8b1a2b,#dc2626)}
.btn-close{background:linear-gradient(135deg,#4a1d7a,#7c3aed)}
.btn:disabled{opacity:0.35;transform:none;filter:none}
.dot{display:inline-block;width:10px;height:10px;border-radius:50%;
  margin-right:8px;vertical-align:middle;box-shadow:0 0 6px currentColor}
.dot-idle{background:#34d399;color:#34d399}
.dot-opening{background:#3b82f6;color:#3b82f6;animation:pulse 0.6s infinite alternate}
.dot-closing{background:#a855f7;color:#a855f7;animation:pulse 0.6s infinite alternate}
@keyframes pulse{from{opacity:1}to{opacity:0.25}}
</style>
</head>
<body>
<div class="card">
<svg class="logo" viewBox="0 0 263 117" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M243 109.026H82.1289V9.02576H243V109.026ZM126.818 20.1136C124.564 20.1137 122.451 20.4949 120.48 21.2572C118.509 22.0086 116.772 23.1248 115.27 24.6058C113.767 26.076 112.591 27.8786 111.741 30.0131C110.903 32.1475 110.483 34.5977 110.483 37.3636C110.483 40.8921 111.17 43.9363 112.542 46.4955C113.925 49.0545 115.848 51.0256 118.309 52.4086C120.77 53.7914 123.628 54.4827 126.884 54.4828C129.802 54.4828 132.389 53.9114 134.644 52.7679C136.909 51.6136 138.684 49.9583 139.969 47.8021C141.265 45.6351 141.912 43.027 141.912 39.9779V35.3383H127.08V41.807H133.152C133.129 42.7405 132.903 43.5572 132.471 44.2572C132.013 44.9976 131.322 45.5691 130.396 45.972C129.482 46.375 128.332 46.5765 126.949 46.5765C125.381 46.5765 124.069 46.2067 123.013 45.4662C121.956 44.7257 121.162 43.658 120.628 42.264C120.094 40.8701 119.827 39.1929 119.827 37.2328C119.827 35.2945 120.105 33.639 120.66 32.267C121.226 30.895 122.043 29.8442 123.11 29.1146C124.188 28.385 125.49 28.0199 127.015 28.0199C127.723 28.0199 128.371 28.1017 128.959 28.265C129.558 28.4175 130.081 28.6514 130.527 28.9672C130.985 29.2721 131.365 29.6486 131.67 30.0951C131.975 30.5306 132.198 31.0366 132.34 31.6136H141.521C141.346 29.9366 140.857 28.3954 140.051 26.9906C139.245 25.5858 138.183 24.3717 136.865 23.348C135.558 22.3135 134.05 21.5187 132.34 20.9633C130.641 20.397 128.8 20.1136 126.818 20.1136ZM155.046 20.5717L144.004 54.0258H153.805L155.738 47.6224H166.769L168.702 54.0258H178.504L167.461 20.5717H155.046ZM176.588 20.5717V27.889H186.65V54.0258H195.603V27.889H205.665V20.5717H176.588ZM209.083 20.5717V54.0258H233.129V46.7074H218.165V40.9574H231.952V33.639H218.165V27.889H233.193V20.5717H209.083ZM161.384 29.7845L164.718 40.8265H157.789L161.123 29.7845H161.384Z" fill="#C3C3C3"/><rect y="100.026" width="82.1289" height="9" fill="#C3C3C3"/><rect x="33.0209" y="67.0258" width="55.0348" height="9" fill="#C3C3C3"/><path d="M0 100.026L82.2963 9.13266L88.0557 15.0258L5.75945 105.919L0 100.026Z" fill="#C3C3C3"/><rect x="88" y="2.02576" width="14" height="114" fill="#717171"/><rect x="249" y="2.02576" width="14" height="114" fill="#717171"/><path d="M118.199 100.026L124.821 67.0258H132.048L126.438 95.0145H144.008L142.995 100.026H118.199Z" fill="#6928D8"/><path d="M148.955 100.026L153.942 75.2758H160.993L156.006 100.026H148.955Z" fill="#6928D8"/><path d="M173.23 85.5238L170.347 100.026H163.277L168.263 75.2758H175.022L174.165 79.4813H174.535C175.47 78.0956 176.782 76.9945 178.47 76.1781C180.158 75.3617 182.125 74.9535 184.372 74.9535C186.411 74.9535 188.125 75.3187 189.514 76.0492C190.904 76.7689 191.891 77.827 192.475 79.2235C193.059 80.6093 193.15 82.2904 192.748 84.267L189.553 100.026H182.502L185.482 85.1693C185.82 83.5258 185.567 82.2367 184.723 81.3021C183.891 80.3568 182.554 79.8842 180.71 79.8842C179.476 79.8842 178.34 80.1097 177.301 80.5609C176.262 81.0013 175.386 81.6405 174.672 82.4784C173.97 83.3163 173.49 84.3314 173.23 85.5238Z" fill="#6928D8"/><path d="M204.649 92.2592L206.032 85.2177H207.162L219.959 75.2758H228.199L212.538 87.4574H211.096L204.649 92.2592ZM196.682 100.026L203.305 67.0258H210.356L203.734 100.026H196.682ZM215.46 100.026L207.98 88.7626L213.57 84.6537L223.894 100.026H215.46Z" fill="#6928D8"/><path d="M156.011 71.2631C155.896 71.2631 155.78 71.2396 155.669 71.1902C154.026 70.4565 152.964 68.8582 152.964 67.1183C152.964 65.3783 154.026 63.7799 155.669 63.0462C156.076 62.8647 156.559 63.0344 156.748 63.4254C156.937 63.8164 156.761 64.2806 156.354 64.4622C155.282 64.941 154.589 65.9835 154.589 67.1183C154.589 68.253 155.282 69.2956 156.354 69.7742C156.761 69.9559 156.937 70.4201 156.748 70.8111C156.611 71.0957 156.317 71.2631 156.011 71.2631Z" fill="#818181"/><path d="M160.001 71.2579C159.696 71.2579 159.403 71.0916 159.265 70.8084C159.075 70.4179 159.249 69.9532 159.656 69.7702C160.722 69.2897 161.411 68.2488 161.411 67.1183C161.411 65.9878 160.722 64.9468 159.656 64.4664C159.249 64.2834 159.075 63.8186 159.265 63.4283C159.455 63.0379 159.939 62.8698 160.345 63.0527C161.979 63.789 163.036 65.3849 163.036 67.1184C163.036 68.8518 161.979 70.4476 160.345 71.184C160.234 71.2341 160.116 71.2579 160.001 71.2579Z" fill="#818181"/><path d="M154.34 73.2107C154.226 73.2107 154.11 73.1873 153.999 73.1379C152.822 72.6123 151.824 71.781 151.115 70.7339C150.385 69.6577 150 68.4075 150 67.1183C150 65.829 150.385 64.5788 151.115 63.5026C151.824 62.4555 152.822 61.6242 153.999 61.0986C154.405 60.917 154.889 61.0867 155.078 61.4777C155.267 61.8687 155.09 62.333 154.683 62.5146C152.825 63.3443 151.624 65.1513 151.624 67.1183C151.624 69.0852 152.825 70.8922 154.683 71.7219C155.09 71.9035 155.267 72.3677 155.078 72.7587C154.94 73.0433 154.647 73.2107 154.34 73.2107Z" fill="#818181"/><path d="M161.678 73.2025C161.373 73.2025 161.081 73.0362 160.943 72.7529C160.752 72.3625 160.927 71.8977 161.333 71.7148C163.181 70.8821 164.376 69.078 164.376 67.1183C164.376 65.1587 163.181 63.3544 161.333 62.5219C160.927 62.3389 160.752 61.8741 160.943 61.4837C161.133 61.0935 161.617 60.9253 162.023 61.1081C163.194 61.6358 164.186 62.4671 164.892 63.512C165.617 64.586 166 65.833 166 67.1183C166 68.4036 165.617 69.6506 164.892 70.7246C164.186 71.7696 163.194 72.6008 162.023 73.1284C161.911 73.1787 161.794 73.2025 161.678 73.2025Z" fill="#818181"/><circle cx="158" cy="67.0258" r="2" fill="#DA1F1F"/></svg>
<div class="divider"></div>
<div id="status"><span class="dot dot-idle"></span>Стоят</div>
<div class="buttons">
  <button class="btn btn-open" onclick="send('open')">ОТКРЫТЬ</button>
  <button class="btn btn-stop" onclick="send('stop')">СТОП</button>
  <button class="btn btn-close" onclick="send('close')">ЗАКРЫТЬ</button>
</div>
</div>
<div class="footer">created by <a href="https://github.com/belkaniacode/auto-dor" target="_blank">belkaniacode</a></div>
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
  WiFi.config(staticIP, gateway, subnet, dns);
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
