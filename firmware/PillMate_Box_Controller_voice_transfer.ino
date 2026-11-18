
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <time.h>
#include <LittleFS.h>

// =========================================================
// PILLMATE BOX CONTROLLER - ESP32-S3-N16R8
// =========================================================

// Wi-Fi Access Point Credentials
const char* AP_SSID = "PillMate-S3";
const char* AP_PASS = "12345678";

// PillStrap X is discovered automatically over ESP-NOW broadcast.
uint8_t bandMacAddress[6] = {0};
bool bandPeerReady = false;
volatile bool bandPeerPending = false;
uint8_t pendingBandMac[6] = {0};
const uint8_t ESPNOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
unsigned long lastDiscoveryBroadcastMs = 0;
const unsigned long DISCOVERY_INTERVAL_MS = 2000;

// Hardware Pin Definitions
const int SERVO_PINS[4] = {17, 21, 47, 48}; // Drawers 1-4
const int PIR_PIN       = 5;            // Motion / Hand Sensor
const int BUZZER_PIN    = 6;            // Box Piezo Buzzer
const int RTC_DAT_PIN   = 40;           // DS1302 IO / DAT
const int RTC_CLK_PIN   = 41;           // DS1302 SCLK / CLK
const int RTC_RST_PIN   = 42;           // DS1302 CE / RST

// Servo Angles
const int SERVO_CLOSED_ANGLE = 0;
const int SERVO_OPEN_ANGLE   = 85;

// Hardware Objects
WebServer server(80);
Preferences prefs;
Servo drawerServos[4];
ThreeWire myWire(RTC_DAT_PIN, RTC_CLK_PIN, RTC_RST_PIN);
RtcDS1302<ThreeWire> Rtc(myWire);

// Voice reminder files are stored locally per drawer as 16 kHz, mono, PCM16 WAV.
File voiceUploadFile;
bool voiceUploadOk = false;
String voiceUploadError = "";
size_t voiceUploadBytes = 0;
const size_t MAX_VOICE_FILE_BYTES = 1500000;

// ESP-NOW Packet Structure (Matches PillStrap X)
typedef struct struct_message {
  char command[32];
} struct_message;

struct_message incomingData;
struct_message outgoingData;
esp_now_peer_info_t peerInfo;

// Regimen Data Structure for Each Drawer (1-4)
struct DrawerSlot {
  bool active;
  String medName;
  int pillsPerDose;
  String dayPattern;     // "daily", "alternate", "weekly", "custom_days"
  int dayInterval;       // Every N days
  String dailyMode;      // "fixed_times" or "hourly_interval"
  int dosesPerDay;       // 1-4
  String times[4];       // "HH:MM"
  String firstDoseTime;  // "HH:MM"
  int hourInterval;      // Every X hours
  int pillsLoaded;       // Current inventory count
  int courseDays;        // Total duration in days
  uint32_t startEpochDay;// Day index when saved
  String lastDispensedHM;// "HH:MM" last triggered
};

DrawerSlot slots[4];

// System State
unsigned long lastWatchSeen = 0;
bool alarmActive = false;
int activeAlarmDrawer = -1;
unsigned long lastAlarmRepeatMs = 0;
int lastCheckedMinute = -1;

// =========================================================
// HARDWARE & ESP-NOW HELPERS
// =========================================================

void sendEspNowPacket(const uint8_t* destination, const char* cmd) {
  memset(&outgoingData, 0, sizeof(outgoingData));
  strncpy(outgoingData.command, cmd, sizeof(outgoingData.command) - 1);
  esp_now_send(destination, (const uint8_t*)&outgoingData, sizeof(outgoingData));
}

void sendToBand(const char* cmd) {
  if (!bandPeerReady) {
    Serial.println("PillStrap X not paired yet; dose alert will retry after discovery.");
    return;
  }
  sendEspNowPacket(bandMacAddress, cmd);
}

bool addBandPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = 1;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  esp_err_t result = esp_now_add_peer(&peer);
  return result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
}

void processPendingBandPeer() {
  if (!bandPeerPending) return;
  uint8_t mac[6];
  noInterrupts();
  memcpy(mac, pendingBandMac, 6);
  bandPeerPending = false;
  interrupts();
  if (addBandPeer(mac)) {
    memcpy(bandMacAddress, mac, 6);
    bandPeerReady = true;
    Serial.printf("PillStrap X paired: %02X:%02X:%02X:%02X:%02X:%02X\n",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    sendToBand("BOX_HELLO");
  }
}

void beepBox(int count, int durationMs) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < count - 1) delay(120);
  }
}

void openDrawer(int index) {
  if (index < 0 || index > 3) return;
  Serial.printf("Opening Drawer %d\n", index + 1);
  drawerServos[index].write(SERVO_OPEN_ANGLE);
}

void closeDrawer(int index) {
  if (index < 0 || index > 3) return;
  Serial.printf("Closing Drawer %d\n", index + 1);
  drawerServos[index].write(SERVO_CLOSED_ANGLE);
}

// ESP-NOW Receive Callback (ESP32 Arduino Core v3.x)
void OnDataRecv(const esp_now_recv_info_t* recv_info, const uint8_t* dataBytes, int len) {
  if (recv_info == nullptr || dataBytes == nullptr || len != (int)sizeof(struct_message)) return;
  struct_message packet = {};
  memcpy(&packet, dataBytes, sizeof(packet));
  packet.command[sizeof(packet.command) - 1] = '\0';

  if (strcmp(packet.command, "WATCH_HELLO") == 0) {
    memcpy((void*)pendingBandMac, recv_info->src_addr, 6);
    bandPeerPending = true;
  } else if (strcmp(packet.command, "WATCH_PING") == 0) {
    lastWatchSeen = millis();
  } else if (strcmp(packet.command, "WATCH_CONFIRM") == 0) {
    lastWatchSeen = millis();
    Serial.println("PillStrap X confirmed dose!");
    if (alarmActive && activeAlarmDrawer >= 0) {
      closeDrawer(activeAlarmDrawer);
      alarmActive = false;
      activeAlarmDrawer = -1;
    }
  }
}

void OnDataSent(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
  (void)tx_info;
  Serial.print("Box -> Band Packet: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "OK" : "Failed");
}

// =========================================================
// RTC & FLASH STORAGE (PREFERENCES)
// =========================================================

void syncRtcFromEpoch(uint32_t unixEpoch) {
  if (unixEpoch < 1700000000UL) return;
  // RtcDS1302 uses seconds since Jan 1, 2000 (946684800 offset from 1970)
  uint32_t rtc2000Sec = unixEpoch - 946684800UL;
  RtcDateTime syncedTime;
  syncedTime.InitWithEpoch32Time(rtc2000Sec);
  if (syncedTime.IsValid()) {
    Rtc.SetDateTime(syncedTime);
    Serial.println("DS1302 RTC synced from browser clock.");
  }
}

uint32_t getCurrentEpochDay() {
  RtcDateTime now = Rtc.GetDateTime();
  if (!now.IsValid()) return millis() / 86400000UL;
  return now.TotalSeconds() / 86400UL;
}

void saveSlotToFlash(int idx) {
  String ns = "slot" + String(idx);
  prefs.begin(ns.c_str(), false);
  prefs.putBool("active", slots[idx].active);
  prefs.putString("medName", slots[idx].medName);
  prefs.putInt("ppDose", slots[idx].pillsPerDose);
  prefs.putString("dPat", slots[idx].dayPattern);
  prefs.putInt("dInt", slots[idx].dayInterval);
  prefs.putString("dMode", slots[idx].dailyMode);
  prefs.putInt("dpDay", slots[idx].dosesPerDay);
  for (int t = 0; t < 4; t++) {
    prefs.putString(("t" + String(t)).c_str(), slots[idx].times[t]);
  }
  prefs.putString("fTime", slots[idx].firstDoseTime);
  prefs.putInt("hInt", slots[idx].hourInterval);
  prefs.putInt("loaded", slots[idx].pillsLoaded);
  prefs.putInt("cDays", slots[idx].courseDays);
  prefs.putUInt("sDay", slots[idx].startEpochDay);
  prefs.end();
}

void loadAllSlotsFromFlash() {
  for (int idx = 0; idx < 4; idx++) {
    String ns = "slot" + String(idx);
    prefs.begin(ns.c_str(), true);
    slots[idx].active        = prefs.getBool("active", false);
    slots[idx].medName       = prefs.getString("medName", "");
    slots[idx].pillsPerDose  = prefs.getInt("ppDose", 1);
    slots[idx].dayPattern    = prefs.getString("dPat", "daily");
    slots[idx].dayInterval   = prefs.getInt("dInt", 1);
    slots[idx].dailyMode     = prefs.getString("dMode", "fixed_times");
    slots[idx].dosesPerDay   = prefs.getInt("dpDay", 1);
    for (int t = 0; t < 4; t++) {
      slots[idx].times[t]    = prefs.getString(("t" + String(t)).c_str(), "08:00");
    }
    slots[idx].firstDoseTime = prefs.getString("fTime", "08:00");
    slots[idx].hourInterval  = prefs.getInt("hInt", 6);
    slots[idx].pillsLoaded   = prefs.getInt("loaded", 0);
    slots[idx].courseDays    = prefs.getInt("cDays", 7);
    slots[idx].startEpochDay = prefs.getUInt("sDay", 0);
    slots[idx].lastDispensedHM = "";
    prefs.end();
  }
}

// =========================================================
// WEB DASHBOARD HTML (PURPLE & YELLOW + [THEKIRAN.IN](http://thekiran.in/) SYNC)
// =========================================================

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>PillMate + PillStrap X Control Panel</title>
  <style>
    * { box-sizing: border-box; font-family: system-ui, -apple-system, sans-serif; }
    body { margin: 0; padding: 18px; background: #120822; color: #ffffff; }
    .container { max-width: 560px; margin: 0 auto; }
    .header {
      background: linear-gradient(135deg, #4c1d95, #2e1065);
      border: 2px solid #facc15;
      border-radius: 16px;
      padding: 18px 20px;
      margin-bottom: 18px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 10px;
    }
    .header h1 { margin: 0; font-size: 22px; color: #facc15; }
    .subtitle { font-size: 12px; color: #e9d5ff; display: block; margin-top: 2px; }
    .badge {
      background: #facc15;
      color: #1e1035;
      font-weight: 800;
      font-size: 12px;
      padding: 6px 12px;
      border-radius: 999px;
    }
    .card {
      background: #1e1035;
      border: 1px solid #6b21a8;
      border-radius: 16px;
      padding: 20px;
      margin-bottom: 16px;
      box-shadow: 0 8px 24px rgba(0, 0, 0, 0.35);
    }
    .section-title {
      margin: 18px 0 12px;
      padding-bottom: 6px;
      border-bottom: 1px solid #581c87;
      color: #fde047;
      font-size: 14px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    .section-title:first-child { margin-top: 0; }
    label { display: block; font-size: 13px; font-weight: 600; color: #e9d5ff; margin-bottom: 6px; }
    input, select {
      width: 100%;
      padding: 10px 12px;
      margin-bottom: 14px;
      border-radius: 8px;
      border: 1px solid #7e22ce;
      background: #2e1065;
      color: #ffffff;
      font-size: 14px;
      outline: none;
    }
    input:focus, select:focus { border-color: #facc15; }
    .row { display: flex; gap: 12px; }
    .col { flex: 1; }
    .sub-box {
      background: #280e54;
      border: 1px dashed #9333ea;
      border-radius: 10px;
      padding: 12px;
      margin-bottom: 14px;
    }
    .summary-banner {
      background: #2e1065;
      border-left: 4px solid #facc15;
      padding: 12px 14px;
      border-radius: 8px;
      margin: 14px 0 18px;
      font-size: 13px;
      color: #f3e8ff;
      line-height: 1.5;
    }
    .summary-banner strong { color: #facc15; }
    .btn-yellow {
      width: 100%;
      padding: 14px;
      border: none;
      border-radius: 10px;
      background: #facc15;
      color: #1e1035;
      font-weight: 800;
      font-size: 15px;
      cursor: pointer;
      box-shadow: 0 4px 14px rgba(250, 204, 21, 0.35);
    }
    .btn-purple {
      width: 100%;
      padding: 12px;
      margin-top: 10px;
      border: 1px solid #c084fc;
      border-radius: 10px;
      background: #581c87;
      color: #ffffff;
      font-weight: 700;
      font-size: 14px;
      cursor: pointer;
    }
    .hidden { display: none; }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <div>
        <h1>PillMate Controller</h1>
        <span class="subtitle">[thekiran.in](http://thekiran.in/) Smart Dispenser Setup</span>
      </div>
      <span class="badge" id="watchStatus">PillStrap X: Checking...</span>
    </div>

    <div class="card">
      <form id="medForm">
        <div class="section-title">1. Drawer & Medicine Details</div>
        <div class="row">
          <div class="col">
            <label for="drawer">Dispenser Drawer</label>
            <select id="drawer" name="drawer">
              <option value="1">Drawer 1</option>
              <option value="2">Drawer 2</option>
              <option value="3">Drawer 3</option>
              <option value="4">Drawer 4</option>
            </select>
          </div>
          <div class="col">
            <label for="pillsPerDose">Pills per Dose</label>
            <input type="number" id="pillsPerDose" name="pillsPerDose" min="1" max="10" value="1" oninput="updateSummary()">
          </div>
        </div>

        <label for="medName">Medicine / Supplement Name</label>
        <input type="text" id="medName" name="medName" placeholder="e.g., Vitamin D3 or Amoxicillin" required>

        <div class="section-title">2. Day Frequency Pattern</div>
        <label for="dayPattern">How often should this pill be taken?</label>
        <select id="dayPattern" name="dayPattern" onchange="toggleDayOptions(); updateSummary();">
          <option value="daily">Every Day (Interval: 1 day)</option>
          <option value="alternate">Alternate Days (Every 2 days)</option>
          <option value="weekly">Weekly (Every 7 days)</option>
          <option value="custom_days">Custom Day Interval (Every N days)</option>
        </select>

        <div id="customDayBox" class="sub-box hidden">
          <label for="dayInterval">Repeat after how many days?</label>
          <input type="number" id="dayInterval" name="dayInterval" min="1" max="60" value="3" oninput="updateSummary()">
        </div>

        <div class="section-title">3. Daily Dose Schedule & Intervals</div>
        <label for="dailyMode">How is it taken within a scheduled day?</label>
        <select id="dailyMode" name="dailyMode" onchange="toggleDailyMode(); updateSummary();">
          <option value="fixed_times">Specific Times per Day (1x to 4x daily)</option>
          <option value="hourly_interval">Every X Hours (Fixed Interval)</option>
        </select>

        <div id="fixedTimesBox" class="sub-box">
          <label for="dosesPerDay">Number of Doses per Day</label>
          <select id="dosesPerDay" name="dosesPerDay" onchange="renderTimeSlots(); updateSummary();">
            <option value="1">1 Time a Day (Once Daily)</option>
            <option value="2">2 Times a Day (Morning & Night)</option>
            <option value="3">3 Times a Day (Morning, Afternoon, Night)</option>
            <option value="4">4 Times a Day</option>
          </select>

          <div class="row">
            <div class="col" id="slot1">
              <label for="time1">Dose 1 Time</label>
              <input type="time" id="time1" name="time1" value="08:00">
            </div>
            <div class="col hidden" id="slot2">
              <label for="time2">Dose 2 Time</label>
              <input type="time" id="time2" name="time2" value="14:00">
            </div>
          </div>
          <div class="row">
            <div class="col hidden" id="slot3">
              <label for="time3">Dose 3 Time</label>
              <input type="time" id="time3" name="time3" value="20:00">
            </div>
            <div class="col hidden" id="slot4">
              <label for="time4">Dose 4 Time</label>
              <input type="time" id="time4" name="time4" value="23:00">
            </div>
          </div>
        </div>

        <div id="intervalBox" class="sub-box hidden">
          <div class="row">
            <div class="col">
              <label for="firstDoseTime">First Dose Starts At</label>
              <input type="time" id="firstDoseTime" name="firstDoseTime" value="08:00">
            </div>
            <div class="col">
              <label for="hourInterval">Repeat Every (Hours)</label>
              <input type="number" id="hourInterval" name="hourInterval" min="1" max="24" value="6" oninput="updateSummary()">
            </div>
          </div>
        </div>

        <div class="section-title">4. Stock Count & Course Duration</div>
        <div class="row">
          <div class="col">
            <label for="pillsLoaded">Total Pills Put In Now</label>
            <input type="number" id="pillsLoaded" name="pillsLoaded" min="1" max="200" value="14" oninput="updateSummary()">
          </div>
          <div class="col">
            <label for="courseDays">Course Duration (Days)</label>
            <input type="number" id="courseDays" name="courseDays" min="1" max="365" value="7" oninput="updateSummary()">
          </div>
        </div>

        <div class="summary-banner" id="summaryBox">Calculating...</div>

        <button type="button" class="btn-yellow" onclick="saveAndSyncToKiran()">
          Save & Sync Data to [thekiran.in](http://thekiran.in/) &rarr;
        </button>
      </form>

      <button type="button" class="btn-purple" onclick="fetch('/ring')">
        Test PillStrap X Wrist Vibration
      </button>
      <button type="button" class="btn-purple" onclick="fetch('/test-drawer?id=' + document.getElementById('drawer').value)">
        Open / Test Selected Drawer Servo
      </button>

      <div class="section-title">5. Voice Reminder Audio</div>
      <div class="sub-box">
        <label for="voiceDrawer">Save recording to drawer</label>
        <select id="voiceDrawer">
          <option value="1">Drawer 1</option><option value="2">Drawer 2</option>
          <option value="3">Drawer 3</option><option value="4">Drawer 4</option>
        </select>
        <label for="voiceFile">Select a WAV recording (16 kHz, mono, PCM 16-bit)</label>
        <input id="voiceFile" type="file" accept=".wav,audio/wav">
        <button type="button" class="btn-yellow" onclick="uploadVoiceReminder()">Upload voice reminder to this drawer</button>
        <p id="voiceUploadStatus" style="font-size:12px;color:#e9d5ff;line-height:1.5">The recording is stored on the dispenser for offline use. Maximum file size: 1.5 MB.</p>
      </div>
    </div>
  </div>

  <script>
    function toggleDayOptions() {
      const pattern = document.getElementById('dayPattern').value;
      const customBox = document.getElementById('customDayBox');
      const dayIntervalInput = document.getElementById('dayInterval');
      if (pattern === 'custom_days') {
        customBox.classList.remove('hidden');
      } else {
        customBox.classList.add('hidden');
        if (pattern === 'daily') dayIntervalInput.value = 1;
        if (pattern === 'alternate') dayIntervalInput.value = 2;
        if (pattern === 'weekly') dayIntervalInput.value = 7;
      }
    }

    function toggleDailyMode() {
      const mode = document.getElementById('dailyMode').value;
      document.getElementById('fixedTimesBox').classList.toggle('hidden', mode !== 'fixed_times');
      document.getElementById('intervalBox').classList.toggle('hidden', mode !== 'hourly_interval');
    }

    function renderTimeSlots() {
      const count = parseInt(document.getElementById('dosesPerDay').value);
      for (let i = 1; i <= 4; i++) {
        document.getElementById('slot' + i).classList.toggle('hidden', i > count);
      }
    }

    function getMetrics() {
      const drawer = document.getElementById('drawer').value;
      const medName = document.getElementById('medName').value.trim() || ("Drawer " + drawer + " Pill");
      const pillsPerDose = parseInt(document.getElementById('pillsPerDose').value) || 1;
      const dayInterval = parseInt(document.getElementById('dayInterval').value) || 1;
      const dailyMode = document.getElementById('dailyMode').value;
      const pillsLoaded = parseInt(document.getElementById('pillsLoaded').value) || 0;
      const courseDays = parseInt(document.getElementById('courseDays').value) || 1;

      let dosesOnActiveDay = 1;
      if (dailyMode === 'fixed_times') {
        dosesOnActiveDay = parseInt(document.getElementById('dosesPerDay').value) || 1;
      } else {
        const hrs = parseInt(document.getElementById('hourInterval').value) || 6;
        dosesOnActiveDay = Math.max(1, Math.floor(24 / hrs));
      }

      const pillsOnActiveDay = dosesOnActiveDay * pillsPerDose;
      const pillsTakenEveryday = Number((pillsOnActiveDay / dayInterval).toFixed(2));
      const activeDaysInCourse = Math.ceil(courseDays / dayInterval);
      const totalPillsNeeded = activeDaysInCourse * pillsOnActiveDay;

      return { drawer, medName, pillsLoaded, pillsOnActiveDay, pillsTakenEveryday, dayInterval, courseDays, totalPillsNeeded };
    }

    function updateSummary() {
      const m = getMetrics();
      const stockNote = m.pillsLoaded >= m.totalPillsNeeded
        ? "<span style='color:#4ade80;'>Enough stock for full " + m.courseDays + "-day course.</span>"
        : "<span style='color:#facc15;'>Refill needed mid-course (" + m.pillsLoaded + " put in vs " + m.totalPillsNeeded + " needed).</span>";

      document.getElementById('summaryBox').innerHTML =
        "<strong>Daily Average:</strong> " + m.pillsTakenEveryday + " pills/day (every " + m.dayInterval + " day(s))<br>" +
        "<strong>Course Total:</strong> " + m.totalPillsNeeded + " pills over " + m.courseDays + " days &mdash; " + stockNote;
    }

    function sendToKiranDashboard(medicine) {
      const params = new URLSearchParams({
        pillmate_sync: "1",
        drawer: medicine.drawer,
        med_name: medicine.medName,
        pills_put_in: medicine.pillsPutIn,
        pills_per_day: medicine.pillsPerDay,
        pills_on_dose_day: medicine.pillsOnDoseDay || "",
        interval_days: medicine.intervalDays || "",
        course_days: medicine.courseDays || ""
      });

      window.location.href =
        "[https://thekiran.in/dashboard/](https://thekiran.in/dashboard/)?" + params.toString();
    }

    async function saveAndSyncToKiran() {
      const form = document.getElementById('medForm');

      if (!form.reportValidity()) return;

      const formData = new URLSearchParams(new FormData(form));
      const m = getMetrics();

      // Sync device clock from phone browser time
      const now = new Date();
      formData.set('epoch', Math.floor(now.getTime() / 1000));

      // Save locally to the ESP32-S3 before syncing to the website
      try {
        const response = await fetch('/save', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: formData.toString()
        });

        if (!response.ok) {
          throw new Error('ESP32 save failed: ' + response.status);
        }
      } catch (error) {
        alert('Could not save medicine details to PillMate. Please try again.');
        console.error('PillMate local save failed:', error);
        return;
      }

      // Sync the saved configuration to the Kiran dashboard
      sendToKiranDashboard({
        drawer: m.drawer,
        medName: m.medName,
        pillsPutIn: m.pillsLoaded,
        pillsPerDay: m.pillsTakenEveryday,
        pillsOnDoseDay: m.pillsOnActiveDay,
        intervalDays: m.dayInterval,
        courseDays: m.courseDays
      });
    }

    async function uploadVoiceReminder() {
      const file = document.getElementById('voiceFile').files[0];
      const drawer = document.getElementById('voiceDrawer').value;
      const status = document.getElementById('voiceUploadStatus');
      if (!file) { status.textContent = 'Choose a WAV file first.'; return; }
      if (!file.name.toLowerCase().endsWith('.wav') || file.size > 1500000) {
        status.textContent = 'Choose a WAV file smaller than 1.5 MB.'; return;
      }
      const form = new FormData();
      form.append('audio', file, file.name);
      status.textContent = 'Uploading… keep this page open.';
      try {
        const response = await fetch('/voice-upload?drawer=' + drawer, { method: 'POST', body: form });
        const message = await response.text();
        if (!response.ok) throw new Error(message || ('HTTP ' + response.status));
        status.textContent = 'Saved to Drawer ' + drawer + '. ' + message;
      } catch (error) {
        status.textContent = 'Upload failed: ' + (error.message || 'Please retry.');
      }
    }

    function pollWatchStatus() {
      fetch('/status').then(r => r.json()).then(data => {
        const badge = document.getElementById('watchStatus');
        if (data.watchConnected) {
          badge.textContent = "PillStrap X: Connected";
          badge.style.background = "#4ade80";
        } else {
          badge.textContent = "PillStrap X: Offline";
          badge.style.background = "#facc15";
        }
      }).catch(() => {});
    }

    toggleDayOptions();
    renderTimeSlots();
    updateSummary();
    pollWatchStatus();
    setInterval(pollWatchStatus, 4000);
  </script>
</body>
</html>
)rawliteral";

// =========================================================
// WEB SERVER ROUTE HANDLERS
// =========================================================

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleVoiceUploadData() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    voiceUploadOk = false;
    voiceUploadError = "";
    voiceUploadBytes = 0;
    int drawer = server.arg("drawer").toInt();
    if (drawer < 1 || drawer > 4) {
      voiceUploadError = "Invalid drawer number.";
      return;
    }
    if (!LittleFS.begin(false)) {
      voiceUploadError = "Local filesystem is unavailable. Install a partition scheme with LittleFS and reflash.";
      return;
    }
    String path = "/voice" + String(drawer) + ".wav";
    if (LittleFS.exists(path)) LittleFS.remove(path);
    voiceUploadFile = LittleFS.open(path, "w");
    if (!voiceUploadFile) voiceUploadError = "Could not create the voice file.";
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (voiceUploadError.length() || !voiceUploadFile) return;
    if (voiceUploadBytes == 0) {
      if (upload.currentSize < 44 ||
          memcmp(upload.buf, "RIFF", 4) != 0 ||
          memcmp(upload.buf + 8, "WAVE", 4) != 0 ||
          memcmp(upload.buf + 12, "fmt ", 4) != 0) {
        voiceUploadError = "Invalid WAV file.";
        voiceUploadFile.close();
        return;
      }
      uint16_t format = upload.buf[20] | (upload.buf[21] << 8);
      uint16_t channels = upload.buf[22] | (upload.buf[23] << 8);
      uint32_t sampleRate = upload.buf[24] | (upload.buf[25] << 8) |
                            (upload.buf[26] << 16) | (upload.buf[27] << 24);
      uint16_t bits = upload.buf[34] | (upload.buf[35] << 8);
      if (format != 1 || channels != 1 || sampleRate != 16000 || bits != 16) {
        voiceUploadError = "Audio must be PCM 16-bit, mono, 16 kHz WAV.";
        voiceUploadFile.close();
        return;
      }
    }
    if (voiceUploadBytes + upload.currentSize > MAX_VOICE_FILE_BYTES) {
      voiceUploadError = "Recording exceeds the 1.5 MB limit.";
      voiceUploadFile.close();
      return;
    }
    size_t written = voiceUploadFile.write(upload.buf, upload.currentSize);
    if (written != upload.currentSize) {
      voiceUploadError = "Flash write failed.";
      voiceUploadFile.close();
      return;
    }
    voiceUploadBytes += written;
  } else if (upload.status == UPLOAD_FILE_END) {
    if (voiceUploadFile) voiceUploadFile.close();
    if (!voiceUploadError.length() && voiceUploadBytes >= 44) voiceUploadOk = true;
    else if (!voiceUploadError.length()) voiceUploadError = "The uploaded WAV file is empty.";
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (voiceUploadFile) voiceUploadFile.close();
    voiceUploadError = "Upload was interrupted.";
  }
}

void handleVoiceUploadComplete() {
  if (voiceUploadOk && !voiceUploadError.length()) {
    server.send(200, "text/plain", "Voice reminder stored locally (" + String(voiceUploadBytes) + " bytes).");
  } else {
    server.send(400, "text/plain", voiceUploadError.length() ? voiceUploadError : "Voice upload failed.");
  }
}


void handleSave() {
  if (server.hasArg("epoch")) {
    uint32_t ep = (uint32_t)strtoul(server.arg("epoch").c_str(), NULL, 10);
    syncRtcFromEpoch(ep);
  }

  int drawerNum = server.arg("drawer").toInt();
  if (drawerNum < 1 || drawerNum > 4) {
    server.send(400, "text/plain", "Invalid drawer");
    return;
  }

  int idx = drawerNum - 1;
  slots[idx].active        = true;
  slots[idx].medName       = server.arg("medName");
  slots[idx].pillsPerDose  = max(1, (int)server.arg("pillsPerDose").toInt());
  slots[idx].dayPattern    = server.arg("dayPattern");
  slots[idx].dayInterval   = max(1, (int)server.arg("dayInterval").toInt());
  slots[idx].dailyMode     = server.arg("dailyMode");
  slots[idx].dosesPerDay   = constrain((int)server.arg("dosesPerDay").toInt(), 1, 4);
  slots[idx].times[0]      = server.arg("time1");
  slots[idx].times[1]      = server.arg("time2");
  slots[idx].times[2]      = server.arg("time3");
  slots[idx].times[3]      = server.arg("time4");
  slots[idx].firstDoseTime = server.arg("firstDoseTime");
  slots[idx].hourInterval  = constrain((int)server.arg("hourInterval").toInt(), 1, 24);
  slots[idx].pillsLoaded   = max(0, (int)server.arg("pillsLoaded").toInt());
  slots[idx].courseDays    = max(1, (int)server.arg("courseDays").toInt());
  slots[idx].startEpochDay = getCurrentEpochDay();
  slots[idx].lastDispensedHM = "";

  saveSlotToFlash(idx);
  beepBox(1, 120);

  Serial.printf("Saved Drawer %d: %s (%d pills loaded, every %d day(s))\n",
                drawerNum, slots[idx].medName.c_str(), slots[idx].pillsLoaded, slots[idx].dayInterval);

  server.send(200, "text/plain", "Saved");
}

void handleRing() {
  Serial.println("Manual test ring triggered -> PillStrap X");
  sendToBand("RING_BAND");
  beepBox(1, 100);
  server.send(200, "text/plain", "Ringing");
}

void handleTestDrawer() {
  int id = server.arg("id").toInt();
  if (id >= 1 && id <= 4) {
    int idx = id - 1;
    openDrawer(idx);
    delay(1500);
    closeDrawer(idx);
  }
  server.send(200, "text/plain", "Tested");
}

void handleStatus() {
  bool connected = (lastWatchSeen > 0) && (millis() - lastWatchSeen < 15000);
  String json = "{\\"watchConnected\\":" + String(connected ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// =========================================================
// SCHEDULE CHECKER ENGINE
// =========================================================

bool isTimeMatch(const DrawerSlot& s, int curH, int curM, const String& curHM) {
  if (s.dailyMode == "fixed_times") {
    for (int i = 0; i < s.dosesPerDay; i++) {
      if (s.times[i] == curHM) return true;
    }
    return false;
  } else {
    int startH = s.firstDoseTime.substring(0, 2).toInt();
    int startM = s.firstDoseTime.substring(3, 5).toInt();
    if (curM != startM) return false;
    int diffH = (curH - startH + 24) % 24;
    return (diffH % s.hourInterval) == 0;
  }
}

void triggerDoseDispense(int idx, const String& curHM) {
  slots[idx].lastDispensedHM = curHM;
  if (slots[idx].pillsLoaded >= slots[idx].pillsPerDose) {
    slots[idx].pillsLoaded -= slots[idx].pillsPerDose;
  } else {
    slots[idx].pillsLoaded = 0;
  }
  saveSlotToFlash(idx);

  activeAlarmDrawer = idx;
  alarmActive = true;
  lastAlarmRepeatMs = millis();

  openDrawer(idx);
  beepBox(2, 200);
  sendToBand("RING_BAND");

  Serial.printf("DOSE TRIGGERED -> Drawer %d (%s) | Remaining pills: %d\n",
                idx + 1, slots[idx].medName.c_str(), slots[idx].pillsLoaded);
}

// =========================================================
// SETUP & LOOP
// =========================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== PillMate Box (ESP32-S3-N16R8) Booting ===");

  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < 4; i++) {
    drawerServos[i].attach(SERVO_PINS[i]);
    closeDrawer(i);
  }

  Rtc.Begin();
  if (!Rtc.GetIsRunning()) {
    Rtc.SetIsRunning(true);
  }
  if (!Rtc.IsDateTimeValid()) {
    Rtc.SetDateTime(RtcDateTime(__DATE__, __TIME__));
  }

  loadAllSlotsFromFlash();

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, 1);
  Serial.print("Hotspot IP: ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Box STA MAC: ");
  Serial.println(WiFi.macAddress());

  WiFi.setSleep(false);
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_register_send_cb(OnDataSent);
    memset(&peerInfo, 0, sizeof(peerInfo));
    memcpy(peerInfo.peer_addr, ESPNOW_BROADCAST_MAC, 6);
    peerInfo.channel = 1;
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = false;
    esp_err_t broadcastResult = esp_now_add_peer(&peerInfo);
    if (broadcastResult != ESP_OK && broadcastResult != ESP_ERR_ESPNOW_EXIST) {
      Serial.printf("Could not add ESP-NOW broadcast peer: %d\n", (int)broadcastResult);
    }
    Serial.println("ESP-NOW ready; searching for PillStrap X...");
  } else {
    Serial.println("ESP-NOW Init Failed!");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/ring", HTTP_GET, handleRing);
  server.on("/test-drawer", HTTP_GET, handleTestDrawer);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/voice-upload", HTTP_POST, handleVoiceUploadComplete, handleVoiceUploadData);
  server.begin();

  beepBox(1, 150);
  Serial.println("PillMate-S3 Ready.");
}

void loop() {
  server.handleClient();

  processPendingBandPeer();
  if (!bandPeerReady && millis() - lastDiscoveryBroadcastMs >= DISCOVERY_INTERVAL_MS) {
    lastDiscoveryBroadcastMs = millis();
    sendEspNowPacket(ESPNOW_BROADCAST_MAC, "BOX_HELLO");
  }

  RtcDateTime now = Rtc.GetDateTime();
  uint32_t todayEpochDay = getCurrentEpochDay();

  if (now.Minute() != lastCheckedMinute) {
    lastCheckedMinute = now.Minute();

    char hmBuf[6];
    snprintf(hmBuf, sizeof(hmBuf), "%02d:%02d", now.Hour(), now.Minute());
    String curHM = String(hmBuf);

    for (int i = 0; i < 4; i++) {
      if (!slots[i].active || slots[i].pillsLoaded <= 0) continue;

      long elapsedDays = (long)(todayEpochDay - slots[i].startEpochDay);
      if (elapsedDays < 0) elapsedDays = 0;

      if (elapsedDays >= slots[i].courseDays) continue;
      if ((elapsedDays % slots[i].dayInterval) != 0) continue;

      if (slots[i].lastDispensedHM != curHM && isTimeMatch(slots[i], now.Hour(), now.Minute(), curHM)) {
        triggerDoseDispense(i, curHM);
      }
    }
  }

  if (alarmActive && activeAlarmDrawer >= 0) {
    if (digitalRead(PIR_PIN) == HIGH) {
      Serial.println("PIR detected pill retrieval! Closing drawer.");
      delay(1500);
      closeDrawer(activeAlarmDrawer);
      alarmActive = false;
      activeAlarmDrawer = -1;
    }
    else if (millis() - lastAlarmRepeatMs > 15000) {
      lastAlarmRepeatMs = millis();
      beepBox(1, 150);
      sendToBand("RING_BAND");
    }
  }

  delay(10);
} &#x20;