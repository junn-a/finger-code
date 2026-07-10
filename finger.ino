#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define RELAY_PIN 13
const unsigned long RELAY_ON_DURATION = 5000; // relay nyala 5 detik lalu mati otomatis

unsigned long relayOnAt = 0;
bool relayIsOn = false;

// ---------- WiFi AP ----------
const char* AP_SSID = "Fingerprint-ESP32";
const char* AP_PASS = "12345678"; // min 8 karakter

WebServer server(80);

// ---------- Sensor ----------
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// ---------- LCD I2C ----------
// Menggunakan alamat 0x27 dengan ukuran layar 16 kolom x 2 baris
LiquidCrystal_I2C lcd(0x27, 16, 2); 

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

// ---------- Semua variabel status ----------
String statusMsg = "Sensor siap. Masukkan ID lalu klik Mulai Enroll.";

// ================= HTML =================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fingerprint Manager</title>
<style>
  body{font-family:Arial,sans-serif;background:#f2f2f2;display:flex;justify-content:center;padding:20px;}
  .card{background:#fff;padding:20px;border-radius:10px;box-shadow:0 2px 8px rgba(0,0,0,0.15);max-width:340px;width:100%;margin-bottom:16px;}
  h2{margin-top:0;font-size:18px;text-align:center;}
  input[type=number]{width:100%;padding:10px;font-size:16px;box-sizing:border-box;margin-bottom:12px;border:1px solid #ccc;border-radius:6px;}
  button{width:100%;padding:12px;font-size:16px;color:#fff;border:none;border-radius:6px;cursor:pointer;margin-bottom:6px;}
  .btn-blue{background:#2563eb;} .btn-gray{background:#6b7280;}
  button:disabled{background:#c3c3c3;}
  #status{margin-top:10px;padding:12px;background:#f8f9fa;border-radius:6px;font-size:14px;min-height:30px;white-space:pre-line;}
  #list{font-size:14px;white-space:pre-line;background:#f8f9fa;padding:12px;border-radius:6px;max-height:200px;overflow-y:auto;}
</style>
</head>
<body>
<div>

  <div class="card">
    <h2>1. Enroll Sidik Jari Baru</h2>
    <input type="number" id="fid" min="1" max="127" placeholder="ID (1-127)">
    <button class="btn-blue" id="btnEnroll" onclick="startEnroll()">Mulai Enroll</button>
    <div id="status">Menunggu perintah...</div>
  </div>

  <div class="card">
    <h2>2. Data Tersimpan di Sensor</h2>
    <button class="btn-gray" onclick="loadList()">Refresh Data</button>
    <div id="list">Klik refresh untuk lihat data...</div>
  </div>

</div>
<script>
let pollingEnroll = null;

function startEnroll(){
  const id = document.getElementById('fid').value;
  if(!id || id < 1 || id > 127){ alert('Isi ID 1-127'); return; }
  document.getElementById('btnEnroll').disabled = true;
  fetch('/enroll?id=' + id).then(()=> {
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
  document.getElementById('list').innerText = 'Memuat...';
  fetch('/list').then(r => r.text()).then(t => {
    document.getElementById('list').innerText = t;
  });
}
</script>
</body>
</html>
)rawliteral";

// ================= Handlers =================
void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleEnroll() {
  if (state >= ST_WAIT_FINGER_1 && state <= ST_STORE) {
    server.send(200, "text/plain", "Masih ada proses enroll berjalan.");
    return;
  }
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "ID tidak ada");
    return;
  }
  enrollId = server.arg("id").toInt();
  if (enrollId < 1 || enrollId > 127) {
    server.send(400, "text/plain", "ID harus 1-127");
    return;
  }
  
  state = ST_WAIT_FINGER_1;
  statusMsg = "Tempelkan jari untuk ID " + String(enrollId) + "...";
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Mode: Enroll ID");
  lcd.setCursor(0, 1);
  lcd.print("Tempel Jari " + String(enrollId));
  
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  server.send(200, "text/plain", statusMsg);
}

void handleList() {
  String result = "";
  int used = 0;
  int capacity = finger.capacity; 
  if(capacity == 0) capacity = 127; 

  for (int id = 1; id <= capacity; id++) {
    uint8_t p = finger.loadModel(id);
    if (p == FINGERPRINT_OK) {
      result += "ID " + String(id) + " : terisi\n";
      used++;
    }
  }
  result = "Total tersimpan: " + String(used) + " / " + String(capacity) + "\n\n" + result;
  server.send(200, "text/plain", result);
}

void showLcdStandby() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("    Akses AI    "); 
  lcd.setCursor(0, 1);
  lcd.print(" Tempelkan Jari ");
}

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
        Serial.print("[RELAY] Jari COCOK! ID: ");
        Serial.println(finger.fingerID);
        
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("AKSES DIIZINKAN!");
        lcd.setCursor(0, 1);
        lcd.print("ID: " + String(finger.fingerID) + " Open Lock");
        
        turnOnRelay(); 
      } else {
        Serial.println("[SCAN] Jari TIDAK COCOK / Belum terdaftar.");
        
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("AKSES DITOLAK!");
        lcd.setCursor(0, 1);
        lcd.print(" Jari Tak Kenal ");
        delay(2000); 
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
        
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Enroll Sukses!");
        lcd.setCursor(0, 1);
        lcd.print("ID Baru: " + String(enrollId));
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

  // Inisialisasi komunikasi I2C secara eksplisit sebelum LCD dinyalakan
  Wire.begin(21, 22); 
  delay(200);

  // Inisialisasi LCD alternatif yang kompatibel dengan banyak library pasaran
  lcd.init();
  lcd.begin(16, 2);  // Memaksa inisialisasi ulang dimensi 16x2 jika init() bawaan macet
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Booting System..");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); 

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

  server.on("/", handleRoot);
  server.on("/enroll", handleEnroll);
  server.on("/status", handleStatus);
  server.on("/list", handleList);
  server.begin();
  
  delay(1000); 
  showLcdStandby();
}

void loop() {
  server.handleClient();
  processStateMachine(); 
  checkRelayTimeout();   
}
