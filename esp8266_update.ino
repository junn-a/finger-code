#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <SoftwareSerial.h>

// =====================================================================
// SEMUA PIN DI BAWAH PAKAI NOMOR GPIO LANGSUNG (bukan label D di board).
// Referensi mapping label D -> GPIO di board NodeMCU/Wemos D1 Mini,
// kalau kamu mau cocokkan ke silkscreen board:
//   D0 = GPIO16   D1 = GPIO5    D2 = GPIO4    D3 = GPIO0 (strapping, hindari)
//   D4 = GPIO2 (strapping, hindari)   D5 = GPIO14   D6 = GPIO12   D7 = GPIO13
//   D8 = GPIO15 (strapping, hindari)  RX = GPIO3 (Serial)  TX = GPIO1 (Serial)
//
// Yang dipakai di kode ini (GPIO asli):
//   I2C LCD : SDA = GPIO4, SCL = GPIO5
//   Sensor  : RX  = GPIO14, TX = GPIO12   -> SoftwareSerial
//   Relay   : GPIO13
// Kalau board kamu bukan NodeMCU/D1 Mini (mis. ESP-01 / modul custom),
// tinggal ganti angka GPIO di bawah sesuai pin fisik yang kamu pakai.
// =====================================================================

// ---------- Relay ----------
#define RELAY_PIN 13 // GPIO13
const unsigned long RELAY_ON_DURATION = 1000; // relay nyala lalu mati otomatis

unsigned long relayOnAt = 0;
bool relayIsOn = false;

// ---------- WiFi AP ----------
const char* AP_SSID = "Fingerprint-ESP8266";
const char* AP_PASS = "12345678"; // min 8 karakter

ESP8266WebServer server(80);

// ---------- Login (form + cookie session, single-user) ----------
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "admin123";
String sessionToken = ""; // kosong = belum ada yang login

String generateToken() {
  String t = "";
  for (int i = 0; i < 4; i++) {
    t += String((uint32_t)random(0, 2147483647), HEX);
  }
  return t;
}

bool isLoggedIn() {
  if (!server.hasHeader("Cookie")) return false;
  String cookie = server.header("Cookie");
  int idx = cookie.indexOf("session=");
  if (idx == -1) return false;
  String token = cookie.substring(idx + 8);
  int semi = token.indexOf(';');
  if (semi != -1) token = token.substring(0, semi);
  token.trim();
  return (sessionToken.length() > 0 && token == sessionToken);
}

bool requireLogin() {
  if (!isLoggedIn()) {
    server.sendHeader("Location", "/login");
    server.send(302, "text/plain", "");
    return false;
  }
  return true;
}

// ---------- Sensor (SoftwareSerial, ESP8266 tidak punya HardwareSerial bebas) ----------
#define FINGER_RX 14 // GPIO14
#define FINGER_TX 12 // GPIO12
SoftwareSerial fingerSerial(FINGER_RX, FINGER_TX); // RX, TX
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

bool sensorOk = false;

// ---------- LCD I2C ----------
#define I2C_SDA 4 // GPIO4
#define I2C_SCL 5 // GPIO5
LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================================================================
// ---------- Penyimpanan persisten via EEPROM (pengganti Preferences) ----------
// Layout EEPROM (total dipakai ~3953 byte dari 4096 yang dialokasikan):
//   [0 .. 15]   : bitmask ID terdaftar (128 bit, cukup utk ID 1-127)
//   [16 .. ..]  : slot nama, 31 byte per ID (30 char + null terminator),
//                 slot ID n ada di offset 16 + (n-1)*31
// =====================================================================
#define EEPROM_SIZE 4096
#define BITMASK_BYTES 16
#define NAME_SLOT_SIZE 31
#define NAME_BASE_OFFSET BITMASK_BYTES
#define MAX_ID 127

void eepromInit() {
  EEPROM.begin(EEPROM_SIZE);
}

bool isIdRegistered(int id) {
  if (id < 1 || id > MAX_ID) return false;
  int byteIdx = id / 8;
  int bitIdx = id % 8;
  uint8_t b = EEPROM.read(byteIdx);
  return (b >> bitIdx) & 0x01;
}

void setIdRegisteredBit(int id, bool val) {
  if (id < 1 || id > MAX_ID) return;
  int byteIdx = id / 8;
  int bitIdx = id % 8;
  uint8_t b = EEPROM.read(byteIdx);
  if (val) b |= (1 << bitIdx);
  else b &= ~(1 << bitIdx);
  EEPROM.write(byteIdx, b);
  EEPROM.commit();
}

int nameSlotOffset(int id) {
  return NAME_BASE_OFFSET + (id - 1) * NAME_SLOT_SIZE;
}

String getFingerName(int id) {
  if (id < 1 || id > MAX_ID) return "";
  int base = nameSlotOffset(id);
  char buf[NAME_SLOT_SIZE];
  for (int i = 0; i < NAME_SLOT_SIZE; i++) {
    buf[i] = (char)EEPROM.read(base + i);
    if (buf[i] == 0) break;
  }
  buf[NAME_SLOT_SIZE - 1] = 0; // jaga-jaga
  return String(buf);
}

void setFingerName(int id, String name) {
  if (id < 1 || id > MAX_ID) return;
  if (name.length() > NAME_SLOT_SIZE - 1) name = name.substring(0, NAME_SLOT_SIZE - 1);
  int base = nameSlotOffset(id);
  int i;
  for (i = 0; i < (int)name.length(); i++) {
    EEPROM.write(base + i, (uint8_t)name[i]);
  }
  EEPROM.write(base + i, 0); // null terminator
  EEPROM.commit();
}

void deleteFingerName(int id) {
  if (id < 1 || id > MAX_ID) return;
  int base = nameSlotOffset(id);
  EEPROM.write(base, 0);
  EEPROM.commit();
}

void addRegisteredId(int id) {
  setIdRegisteredBit(id, true);
}

void removeRegisteredId(int id) {
  setIdRegisteredBit(id, false);
}

// Kembalikan array of int ID terdaftar (dibaca dari bitmask, bukan scan sensor)
int getRegisteredIds(int* outArr, int maxOut) {
  int count = 0;
  for (int id = 1; id <= MAX_ID && count < maxOut; id++) {
    if (isIdRegistered(id)) outArr[count++] = id;
  }
  return count;
}

String pendingEnrollName = "";

// ---------- Riwayat scan (RAM only, max 20 terakhir) ----------
#define HISTORY_MAX 20
struct ScanLog {
  int id;
  String name;
  bool success;
  unsigned long secondsAgoAtLog;
};
ScanLog history[HISTORY_MAX];
int historyCount = 0;
int historyHead = 0;

void addHistory(int id, String name, bool success) {
  history[historyHead].id = id;
  history[historyHead].name = name;
  history[historyHead].success = success;
  history[historyHead].secondsAgoAtLog = millis() / 1000;
  historyHead = (historyHead + 1) % HISTORY_MAX;
  if (historyCount < HISTORY_MAX) historyCount++;
}

// ---------- State machine ----------
enum EnrollState {
  ST_IDLE,
  ST_WAIT_FINGER_1,
  ST_CONVERT_1,
  ST_WAIT_REMOVE,
  ST_WAIT_FINGER_2,
  ST_CONVERT_2,
  ST_CREATE_MODEL,
  ST_STORE,
  ST_DONE,
  ST_ERROR,
  ST_VERIFY_WAIT_FINGER,
  ST_VERIFY_CONVERT,
  ST_VERIFY_SEARCH,
  ST_VERIFY_DONE
};

EnrollState state = ST_VERIFY_WAIT_FINGER;
int enrollId = -1;

String statusMsg = "Sensor siap. Masukkan ID lalu klik Mulai Enroll.";

// ---------- Recovery sensor ----------
// Kalau getImage()/image2Tz()/fingerFastSearch() mengembalikan kode error
// komunikasi (bukan FINGERPRINT_OK dan bukan FINGERPRINT_NOFINGER) berkali-kali
// berturut-turut, kemungkinan komunikasi serial ke sensor desync/putus.
// Reinit SoftwareSerial + sensor secara otomatis.
int consecutiveSensorErrors = 0;
const int SENSOR_ERROR_THRESHOLD = 5;
unsigned long lastSensorErrorLog = 0;

void reinitFingerSensor() {
  Serial.println("[SENSOR] Reinit serial & sensor karena error beruntun...");
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sensor Reconnect");
  lcd.setCursor(0, 1);
  lcd.print("Mohon tunggu...");

  fingerSerial.end();
  delay(100);
  fingerSerial.begin(57600);
  delay(100);

  sensorOk = finger.verifyPassword();
  if (sensorOk) {
    Serial.println("[SENSOR] Reconnect berhasil.");
    finger.getParameters();
  } else {
    Serial.println("[SENSOR] Reconnect GAGAL, sensor masih tidak merespons.");
  }
  consecutiveSensorErrors = 0;
  showLcdStandby();
}

// Dipanggil setiap kali dapat kode return non-OK dari operasi sensor di luar
// alur enroll (yang sudah punya penanganan error sendiri).
void reportSensorErrorIfAny(uint8_t p) {
  if (p == FINGERPRINT_OK || p == FINGERPRINT_NOFINGER) {
    consecutiveSensorErrors = 0;
    return;
  }
  consecutiveSensorErrors++;
  if (millis() - lastSensorErrorLog > 1000) {
    Serial.print("[SENSOR] Kode error: ");
    Serial.println(p);
    lastSensorErrorLog = millis();
  }
  if (consecutiveSensorErrors >= SENSOR_ERROR_THRESHOLD) {
    reinitFingerSensor();
  }
}

// ================= HTML: LOGIN =================
const char LOGIN_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Masuk - Fingerprint Manager</title>
<style>
  *{box-sizing:border-box;}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;
       background:linear-gradient(135deg,#eef2ff 0%,#f8fafc 100%);
       margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px;}
  .card{background:#fff;padding:36px 32px;border-radius:18px;box-shadow:0 10px 40px rgba(30,41,59,0.12);
        max-width:340px;width:100%;}
  .logo{width:52px;height:52px;background:linear-gradient(135deg,#4f46e5,#6366f1);border-radius:14px;
        display:flex;align-items:center;justify-content:center;margin:0 auto 18px;font-size:24px;}
  h1{font-size:19px;text-align:center;margin:0 0 4px;color:#1e293b;}
  p.sub{text-align:center;color:#94a3b8;font-size:13px;margin:0 0 24px;}
  label{font-size:12px;color:#475569;font-weight:600;display:block;margin-bottom:6px;}
  input{width:100%;padding:11px 13px;font-size:14px;border:1.5px solid #e2e8f0;border-radius:10px;
        margin-bottom:16px;outline:none;transition:border 0.15s;}
  input:focus{border-color:#6366f1;}
  button{width:100%;padding:12px;font-size:14px;font-weight:600;color:#fff;border:none;border-radius:10px;
         cursor:pointer;background:linear-gradient(135deg,#4f46e5,#6366f1);}
  button:hover{opacity:0.92;}
  .err{background:#fef2f2;color:#dc2626;font-size:13px;padding:10px 12px;border-radius:8px;margin-bottom:16px;text-align:center;}
</style>
</head>
<body>
  <div class="card">
    <div class="logo">🔒</div>
    <h1>Fingerprint Manager</h1>
    <p class="sub">Masuk untuk mengelola akses</p>
    <div id="errBox"></div>
    <form method="POST" action="/login">
      <label>Username</label>
      <input type="text" name="username" autocomplete="username" required>
      <label>Password</label>
      <input type="password" name="password" autocomplete="current-password" required>
      <button type="submit">Masuk</button>
    </form>
  </div>
  <script>
    if (window.location.search.includes('error=1')) {
      document.getElementById('errBox').innerHTML = '<div class="err">Username atau password salah.</div>';
    }
  </script>
</body>
</html>
)rawliteral";

// ================= HTML: MAIN APP =================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fingerprint Manager</title>
<style>
  *{box-sizing:border-box;}
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Arial,sans-serif;
       background:#f8fafc;margin:0;padding:0;color:#1e293b;}
  .topbar{background:#fff;border-bottom:1px solid #e2e8f0;padding:14px 20px;display:flex;
          align-items:center;justify-content:space-between;position:sticky;top:0;z-index:10;}
  .topbar h1{font-size:16px;margin:0;display:flex;align-items:center;gap:8px;}
  .topbar h1 .dot{width:8px;height:8px;background:#22c55e;border-radius:50%;display:inline-block;}
  .logout{background:#fff1f2;color:#e11d48;border:1px solid #fecdd3;padding:7px 14px;border-radius:8px;
          font-size:12.5px;font-weight:600;cursor:pointer;text-decoration:none;}
  .wrap{max-width:440px;margin:0 auto;padding:18px 16px 40px;}
  .tabs{display:flex;background:#fff;border-radius:12px;overflow:hidden;border:1px solid #e2e8f0;
        margin-bottom:16px;padding:4px;gap:4px;}
  .tab{flex:1;padding:10px 4px;text-align:center;font-size:13px;font-weight:600;cursor:pointer;
       background:transparent;color:#64748b;border:none;border-radius:8px;transition:all 0.15s;}
  .tab.active{background:#4f46e5;color:#fff;}
  .panel{display:none;background:#fff;padding:20px;border-radius:14px;border:1px solid #e2e8f0;}
  .panel.active{display:block;}
  h2{margin-top:0;font-size:16px;color:#1e293b;}
  label{font-size:12px;color:#475569;font-weight:600;display:block;margin-bottom:6px;}
  input[type=number],input[type=text]{width:100%;padding:10px 12px;font-size:14px;margin-bottom:14px;
        border:1.5px solid #e2e8f0;border-radius:9px;outline:none;}
  input:focus{border-color:#6366f1;}
  button{padding:11px;font-size:13.5px;font-weight:600;color:#fff;border:none;border-radius:9px;cursor:pointer;}
  .btn-primary{background:#4f46e5;width:100%;}
  .btn-secondary{background:#f1f5f9;color:#334155;width:100%;margin-bottom:12px;border:1px solid #e2e8f0;}
  .btn-danger{background:#fef2f2;color:#dc2626;padding:6px 10px;font-size:11.5px;border:1px solid #fecdd3;}
  .btn-warn{background:#fffbeb;color:#b45309;padding:6px 10px;font-size:11.5px;border:1px solid #fde68a;}
  button:disabled{opacity:0.5;cursor:not-allowed;}
  #status{margin-top:10px;padding:11px 13px;background:#f8fafc;border-radius:9px;font-size:13px;
          white-space:pre-line;border:1px solid #e2e8f0;color:#475569;}
  .row{display:flex;justify-content:space-between;align-items:center;padding:11px 0;
       border-bottom:1px solid #f1f5f9;font-size:13.5px;}
  .row:last-child{border-bottom:none;}
  .rowInfo{display:flex;flex-direction:column;gap:2px;}
  .rowInfo .idTag{font-size:11px;color:#94a3b8;font-weight:600;}
  .rowActions{display:flex;gap:6px;}
  .empty{color:#94a3b8;font-size:13px;text-align:center;padding:24px 0;}
  .badge-ok{background:#f0fdf4;color:#16a34a;padding:3px 9px;border-radius:20px;font-size:11px;font-weight:700;}
  .badge-fail{background:#fef2f2;color:#dc2626;padding:3px 9px;border-radius:20px;font-size:11px;font-weight:700;}
  .timeAgo{color:#94a3b8;font-size:11.5px;margin-top:2px;}
</style>
</head>
<body>
  <div class="topbar">
    <h1><span class="dot"></span> Fingerprint Manager</h1>
    <a class="logout" href="/logout">Keluar</a>
  </div>

  <div class="wrap">
    <div class="tabs">
      <button class="tab active" onclick="showTab('enroll',this)">Enroll</button>
      <button class="tab" onclick="showTab('crud',this)">Daftar</button>
      <button class="tab" onclick="showTab('history',this)">Riwayat</button>
    </div>

    <div id="enroll" class="panel active">
      <h2>Enroll Sidik Jari Baru</h2>
      <label>ID (1-127)</label>
      <input type="number" id="fid" min="1" max="127" placeholder="Contoh: 5">
      <label>Nama Pemilik</label>
      <input type="text" id="fname" placeholder="Contoh: Budi">
      <button class="btn-primary" id="btnEnroll" onclick="startEnroll()">Mulai Enroll</button>
      <div id="status">Menunggu perintah...</div>
    </div>

    <div id="crud" class="panel">
      <h2>Daftar Sidik Jari Terdaftar</h2>
      <button class="btn-secondary" onclick="loadList()">↻ Refresh Daftar</button>
      <div id="list"><div class="empty">Klik refresh untuk lihat data...</div></div>
    </div>

    <div id="history" class="panel">
      <h2>Riwayat Scan (20 terakhir)</h2>
      <button class="btn-secondary" onclick="loadHistory()">↻ Refresh Riwayat</button>
      <div id="historyList"><div class="empty">Klik refresh untuk lihat riwayat...</div></div>
    </div>
  </div>

<script>
let pollingEnroll = null;

function showTab(id, btn){
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  btn.classList.add('active');
  if(id === 'crud') loadList();
  if(id === 'history') loadHistory();
}

function startEnroll(){
  const id = document.getElementById('fid').value;
  const name = document.getElementById('fname').value.trim();
  if(!id || id < 1 || id > 127){ alert('Isi ID 1-127'); return; }
  if(!name){ alert('Isi nama pemilik jari'); return; }
  document.getElementById('btnEnroll').disabled = true;
  fetch('/enroll?id=' + id + '&name=' + encodeURIComponent(name)).then(()=> {
    if(pollingEnroll) clearInterval(pollingEnroll);
    pollingEnroll = setInterval(pollStatus, 800);
  });
}

function pollStatus(){
  fetch('/status').then(r => r.text()).then(t => {
    document.getElementById('status').innerText = t;
    if(t.includes('BERHASIL') || t.includes('GAGAL') || t.includes('Error')){
      clearInterval(pollingEnroll);
      document.getElementById('btnEnroll').disabled = false;
    }
  });
}

function loadList(){
  document.getElementById('list').innerHTML = '<div class="empty">Memuat...</div>';
  fetch('/list').then(r => r.json()).then(data => {
    if(data.length === 0){
      document.getElementById('list').innerHTML = '<div class="empty">Belum ada sidik jari terdaftar</div>';
      return;
    }
    let html = '';
    data.forEach(item => {
      html += '<div class="row"><div class="rowInfo"><span>' + item.name + '</span><span class="idTag">ID ' + item.id + '</span></div>' +
              '<span class="rowActions">' +
              '<button class="btn-warn" onclick="renamePrompt(' + item.id + ')">Ganti Nama</button>' +
              '<button class="btn-danger" onclick="deleteId(' + item.id + ')">Hapus</button>' +
              '</span></div>';
    });
    document.getElementById('list').innerHTML = html;
  });
}

function renamePrompt(id){
  const newName = prompt('Nama baru untuk ID ' + id + ':');
  if(newName === null || newName.trim() === '') return;
  fetch('/rename?id=' + id + '&name=' + encodeURIComponent(newName.trim()))
    .then(() => loadList());
}

function deleteId(id){
  if(!confirm('Hapus sidik jari ID ' + id + ' dari sensor?')) return;
  fetch('/delete?id=' + id).then(() => loadList());
}

function loadHistory(){
  document.getElementById('historyList').innerHTML = '<div class="empty">Memuat...</div>';
  fetch('/history').then(r => r.json()).then(data => {
    if(data.length === 0){
      document.getElementById('historyList').innerHTML = '<div class="empty">Belum ada riwayat</div>';
      return;
    }
    let html = '';
    data.forEach(item => {
      const badge = item.success ? '<span class="badge-ok">COCOK</span>' : '<span class="badge-fail">TIDAK COCOK</span>';
      html += '<div class="row"><div class="rowInfo"><span>' + item.label + '</span><span class="timeAgo">' + item.ago + ' detik lalu</span></div>' + badge + '</div>';
    });
    document.getElementById('historyList').innerHTML = html;
  });
}

setInterval(pollStatus, 1500);
</script>
</body>
</html>
)rawliteral";

// ================= Handlers: Auth =================
void handleLoginPage() {
  server.send(200, "text/html", LOGIN_HTML);
}

void handleLoginPost() {
  String user = server.arg("username");
  String pass = server.arg("password");
  if (user == ADMIN_USER && pass == ADMIN_PASS) {
    sessionToken = generateToken();
    server.sendHeader("Set-Cookie", "session=" + sessionToken + "; Path=/");
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  } else {
    server.sendHeader("Location", "/login?error=1");
    server.send(302, "text/plain", "");
  }
}

void handleLogout() {
  sessionToken = "";
  server.sendHeader("Set-Cookie", "session=; Path=/; Max-Age=0");
  server.sendHeader("Location", "/login");
  server.send(302, "text/plain", "");
}

// ================= Handlers: App =================
void handleRoot() {
  if (!requireLogin()) return;
  server.send(200, "text/html", INDEX_HTML);
}

void handleEnroll() {
  if (!requireLogin()) return;
  if (state >= ST_WAIT_FINGER_1 && state <= ST_STORE) {
    server.send(200, "text/plain", "Masih ada proses enroll berjalan.");
    return;
  }
  if (!server.hasArg("id") || !server.hasArg("name")) {
    server.send(400, "text/plain", "ID atau nama tidak ada");
    return;
  }
  enrollId = server.arg("id").toInt();
  pendingEnrollName = server.arg("name");
  if (enrollId < 1 || enrollId > 127 || pendingEnrollName.length() == 0) {
    server.send(400, "text/plain", "ID harus 1-127 dan nama tidak boleh kosong");
    return;
  }

  state = ST_WAIT_FINGER_1;
  statusMsg = "Tempelkan jari untuk ID " + String(enrollId) + " (" + pendingEnrollName + ")...";

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Mode: Enroll ID");
  lcd.setCursor(0, 1);
  lcd.print("Tempel Jari " + String(enrollId));

  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  if (!requireLogin()) return;
  server.send(200, "text/plain", statusMsg);
}

// List sekarang bersumber dari EEPROM (bitmask + nama), BUKAN scan sensor 1..127.
// Query sensor 127x per request itu blocking lama dan berisiko membuat
// komunikasi serial ke sensor desync setelah dipakai lama.
void handleList() {
  if (!requireLogin()) return;
  int ids[MAX_ID];
  int n = getRegisteredIds(ids, MAX_ID);
  String json = "[";
  for (int i = 0; i < n; i++) {
    String name = getFingerName(ids[i]);
    if (name.length() == 0) name = "(Tanpa Nama)";
    if (i > 0) json += ",";
    json += "{\"id\":" + String(ids[i]) + ",\"name\":\"" + name + "\"}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleRename() {
  if (!requireLogin()) return;
  if (!server.hasArg("id") || !server.hasArg("name")) {
    server.send(400, "text/plain", "Data kurang");
    return;
  }
  int id = server.arg("id").toInt();
  String newName = server.arg("name");
  if (id < 1 || id > 127 || !isIdRegistered(id)) {
    server.send(404, "text/plain", "ID tidak ditemukan di daftar");
    return;
  }
  setFingerName(id, newName);
  server.send(200, "text/plain", "OK");
}

void handleDelete() {
  if (!requireLogin()) return;
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "ID tidak ada");
    return;
  }
  int id = server.arg("id").toInt();
  if (id < 1 || id > 127) {
    server.send(400, "text/plain", "ID tidak valid");
    return;
  }
  finger.deleteModel(id);
  deleteFingerName(id);
  removeRegisteredId(id);
  server.send(200, "text/plain", "OK");
}

void handleHistory() {
  if (!requireLogin()) return;
  String json = "[";
  unsigned long nowSec = millis() / 1000;
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyHead - 1 - i + HISTORY_MAX) % HISTORY_MAX;
    if (i > 0) json += ",";
    String label = "ID " + String(history[idx].id) + " — " + history[idx].name;
    unsigned long ago = nowSec - history[idx].secondsAgoAtLog;
    json += "{\"label\":\"" + label + "\",\"success\":" + (history[idx].success ? "true" : "false") + ",\"ago\":" + String(ago) + "}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// ================= LCD helper =================
void showLcdStandby() {
  lcd.clear();
  lcd.setCursor(0, 0);
  if (sensorOk) {
    lcd.print("    System ON    ");
    lcd.setCursor(0, 1);
    lcd.print(" Tempelkan Jari ");
  } else {
    lcd.print("  Sensor Error  ");
    lcd.setCursor(0, 1);
    lcd.print("  Cek Wiring!   ");
  }
}

// ================= Relay helper =================
void turnOnRelay() {
  digitalWrite(RELAY_PIN, HIGH);
  relayIsOn = true;
  relayOnAt = millis();
}

void checkRelayTimeout() {
  if (relayIsOn && (millis() - relayOnAt >= RELAY_ON_DURATION)) {
    digitalWrite(RELAY_PIN, LOW);
    relayIsOn = false;
    Serial.println("[RELAY] Durasi selesai. Relay kembali OFF.");
    showLcdStandby();
  }
}

// ================= State machine processor (non-blocking) =================
void processStateMachine() {
  static unsigned long lastProcess = 0;
  if (millis() - lastProcess < 50) return;
  lastProcess = millis();

  int p;
  switch (state) {

    // ====== AUTO-SCAN VERIFIKASI (SIAGA SETIAP SAAT) ======
    case ST_VERIFY_WAIT_FINGER:
      p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        Serial.println("[SCAN] Jari terdeteksi, memproses...");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Memproses Jari..");
        consecutiveSensorErrors = 0;
        state = ST_VERIFY_CONVERT;
      } else {
        // p == FINGERPRINT_NOFINGER itu normal (belum ada jari).
        // Selain itu berarti komunikasi ke sensor bermasalah.
        reportSensorErrorIfAny(p);
      }
      break;

    case ST_VERIFY_CONVERT:
      p = finger.image2Tz(1);
      if (p == FINGERPRINT_OK) {
        consecutiveSensorErrors = 0;
        state = ST_VERIFY_SEARCH;
      } else {
        Serial.println("[SCAN] Error konversi citra.");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Scan Error!");
        delay(1500);
        reportSensorErrorIfAny(p);
        state = ST_VERIFY_DONE;
      }
      break;

    case ST_VERIFY_SEARCH:
      p = finger.fingerFastSearch();
      if (p == FINGERPRINT_OK) {
        int matchedId = finger.fingerID;
        String ownerName = getFingerName(matchedId);
        if (ownerName.length() == 0) ownerName = "(Tanpa Nama)";

        Serial.print("[RELAY] Jari COCOK! ID: ");
        Serial.println(matchedId);

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("AKSES DIIZINKAN!");
        lcd.setCursor(0, 1);
        lcd.print(ownerName);

        addHistory(matchedId, ownerName, true);
        turnOnRelay();
        consecutiveSensorErrors = 0;
      } else if (p == FINGERPRINT_NOTFOUND) {
        Serial.println("[SCAN] Jari TIDAK COCOK / Belum terdaftar.");

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("AKSES DITOLAK!");
        lcd.setCursor(0, 1);
        lcd.print(" Jari Tak Kenal ");
        delay(2000);

        addHistory(0, "(tidak dikenal)", false);
        consecutiveSensorErrors = 0;
      } else {
        // Error komunikasi saat search, bukan sekadar "tidak cocok"
        Serial.println("[SCAN] Error saat search di sensor.");
        reportSensorErrorIfAny(p);
      }
      state = ST_VERIFY_DONE;
      break;

    case ST_VERIFY_DONE:
      if (!relayIsOn) {
        showLcdStandby();
      }
      state = ST_VERIFY_WAIT_FINGER;
      break;

    // ====== ENROLL NEW FINGERPRINT ======
    case ST_WAIT_FINGER_1:
      p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        statusMsg = "Citra 1 diambil, memproses...";
        lcd.setCursor(0, 1);
        lcd.print("Citra 1 OK!    ");
        state = ST_CONVERT_1;
      } else if (p != FINGERPRINT_NOFINGER) {
        statusMsg = "Error ambil citra 1";
        state = ST_ERROR;
      }
      break;

    case ST_CONVERT_1:
      p = finger.image2Tz(1);
      if (p == FINGERPRINT_OK) {
        statusMsg = "Angkat jari...";
        lcd.setCursor(0, 1);
        lcd.print("Angkat Jari...  ");
        state = ST_WAIT_REMOVE;
      } else {
        statusMsg = "GAGAL konversi citra 1. Ulangi.";
        state = ST_ERROR;
      }
      break;

    case ST_WAIT_REMOVE:
      p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        statusMsg = "Tempelkan jari yang SAMA sekali lagi...";
        lcd.setCursor(0, 1);
        lcd.print("Tempel Lagi...  ");
        state = ST_WAIT_FINGER_2;
      }
      break;

    case ST_WAIT_FINGER_2:
      p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        statusMsg = "Citra 2 diambil, memproses...";
        lcd.setCursor(0, 1);
        lcd.print("Citra 2 OK!    ");
        state = ST_CONVERT_2;
      } else if (p != FINGERPRINT_NOFINGER) {
        statusMsg = "Error ambil citra 2";
        state = ST_ERROR;
      }
      break;

    case ST_CONVERT_2:
      p = finger.image2Tz(2);
      if (p == FINGERPRINT_OK) {
        state = ST_CREATE_MODEL;
      } else {
        statusMsg = "GAGAL konversi citra 2. Ulangi.";
        state = ST_ERROR;
      }
      break;

    case ST_CREATE_MODEL:
      p = finger.createModel();
      if (p == FINGERPRINT_OK) {
        statusMsg = "Kedua citra cocok, menyimpan...";
        state = ST_STORE;
      } else {
        statusMsg = "GAGAL: kedua sidik jari tidak cocok. Ulangi.";
        state = ST_ERROR;
      }
      break;

    case ST_STORE:
      p = finger.storeModel(enrollId);
      if (p == FINGERPRINT_OK) {
        statusMsg = "BERHASIL! Tersimpan di ID " + String(enrollId);
        setFingerName(enrollId, pendingEnrollName);
        addRegisteredId(enrollId);

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Enroll Sukses!");
        lcd.setCursor(0, 1);
        lcd.print(pendingEnrollName);
        delay(2000);
      } else {
        statusMsg = "GAGAL menyimpan ke sensor";
      }
      state = ST_DONE;
      break;

    case ST_DONE:
    case ST_ERROR:
      if (state == ST_ERROR) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Enroll Gagal!");
        delay(2000);
      }
      consecutiveSensorErrors = 0;
      showLcdStandby();
      state = ST_VERIFY_WAIT_FINGER;
      break;

    default:
      state = ST_VERIFY_WAIT_FINGER;
      break;
  }
}

// ================= Setup & Loop =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  randomSeed(analogRead(A0) + micros());

  Wire.begin(I2C_SDA, I2C_SCL);
  delay(200);

  lcd.init();
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Booting System..");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  eepromInit(); // pengganti Preferences: penyimpanan nama + idlist di EEPROM

  fingerSerial.begin(57600);

  if (finger.verifyPassword()) {
    Serial.println("Sensor fingerprint terdeteksi.");
    finger.getParameters();
    sensorOk = true;
  } else {
    Serial.println("Sensor TIDAK terdeteksi, cek wiring!");
    sensorOk = false;
    lcd.setCursor(0, 1);
    lcd.print("Sensor Error!   ");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // Wajib supaya server.header("Cookie") bisa dibaca.
  // ESP8266WebServer::collectHeaders() variadic (beda dari WebServer versi ESP32
  // yang menerima array+count) — jadi nama header dikirim langsung sebagai argumen.
  server.collectHeaders("Cookie");

  server.on("/login", HTTP_GET, handleLoginPage);
  server.on("/login", HTTP_POST, handleLoginPost);
  server.on("/logout", handleLogout);

  server.on("/", handleRoot);
  server.on("/enroll", handleEnroll);
  server.on("/status", handleStatus);
  server.on("/list", handleList);
  server.on("/rename", handleRename);
  server.on("/delete", handleDelete);
  server.on("/history", handleHistory);
  server.begin();

  delay(1000);
  showLcdStandby();
}

void loop() {
  server.handleClient();
  processStateMachine();
  checkRelayTimeout();
  yield(); // beri kesempatan stack WiFi/OS ESP8266 jalan, cegah watchdog reset
}
