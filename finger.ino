#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ---------- Relay ----------
#define RELAY_PIN 13
const unsigned long RELAY_ON_DURATION = 1000; // relay nyala 5 detik lalu mati otomatis

unsigned long relayOnAt = 0;
bool relayIsOn = false;

// ---------- WiFi AP ----------
const char* AP_SSID = "Fingerprint-ESP32";
const char* AP_PASS = "12345678"; // min 8 karakter

WebServer server(80);

// ---------- Login (form + cookie session, single-user) ----------
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS = "admin123";
String sessionToken = ""; // kosong = belum ada yang login

String generateToken() {
  String t = "";
  for (int i = 0; i < 4; i++) t += String(esp_random(), HEX);
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

// ---------- Sensor ----------
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// ---------- LCD I2C ----------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------- Nama untuk tiap ID (PERSISTEN via NVS/Preferences) ----------
Preferences prefs;

String getFingerName(int id) {
  String key = "n" + String(id);
  return prefs.getString(key.c_str(), "");
}

void setFingerName(int id, String name) {
  String key = "n" + String(id);
  prefs.putString(key.c_str(), name);
}

void deleteFingerName(int id) {
  String key = "n" + String(id);
  prefs.remove(key.c_str());
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

// List sekarang bersumber dari SENSOR langsung (bukan RAM), dipadukan dengan nama dari NVS
void handleList() {
  if (!requireLogin()) return;
  String json = "[";
  bool first = true;
  for (int id = 1; id <= 127; id++) {
    uint8_t p = finger.loadModel(id);
    if (p == FINGERPRINT_OK) {
      String name = getFingerName(id);
      if (name.length() == 0) name = "(Tanpa Nama)";
      if (!first) json += ",";
      json += "{\"id\":" + String(id) + ",\"name\":\"" + name + "\"}";
      first = false;
    }
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
  if (id < 1 || id > 127 || finger.loadModel(id) != FINGERPRINT_OK) {
    server.send(404, "text/plain", "ID tidak ditemukan di sensor");
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
  lcd.print("    System ON    ");
  lcd.setCursor(0, 1);
  lcd.print(" Tempelkan Jari ");
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
        state = ST_VERIFY_CONVERT;
      }
      break;

    case ST_VERIFY_CONVERT:
      p = finger.image2Tz(1);
      if (p == FINGERPRINT_OK) {
        state = ST_VERIFY_SEARCH;
      } else {
        Serial.println("[SCAN] Error konversi citra.");
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Scan Error!");
        delay(1500);
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
      } else {
        Serial.println("[SCAN] Jari TIDAK COCOK / Belum terdaftar.");

        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("AKSES DITOLAK!");
        lcd.setCursor(0, 1);
        lcd.print(" Jari Tak Kenal ");
        delay(2000);

        addHistory(0, "(tidak dikenal)", false);
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

  Wire.begin(21, 22);
  delay(200);

  lcd.init();
  lcd.begin(16, 2);
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Booting System..");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  prefs.begin("fpnames", false); // namespace penyimpanan nama, persisten

  mySerial.begin(57600, SERIAL_8N1, 16, 17);

  if (finger.verifyPassword()) {
    Serial.println("Sensor fingerprint terdeteksi.");
    finger.getParameters();
  } else {
    Serial.println("Sensor TIDAK terdeteksi, cek wiring!");
    lcd.setCursor(0, 1);
    lcd.print("Sensor Error!   ");
  }

  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // Wajib supaya server.header("Cookie") bisa dibaca
  const char* headerKeys[] = {"Cookie"};
  server.collectHeaders(headerKeys, 1);

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
}
