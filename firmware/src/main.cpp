/* ============================================================================
 * SereniSuit — Demo 1 firmware  (ESP32-S3-DevKitC-1 N8R8)
 *
 * WHAT RUNS LIVE  (this is the demo)
 *   AD8232 single-lead ECG (chest electrodes) -> R-peaks -> BPM on the
 *   website and the mobile app.
 *   Outside 60-100 BPM -> "DANGER" + vibration through the DRV8833.
 *   This is a threshold on heart RATE. Deterministic arithmetic, not ML.
 *
 * STANDALONE DEMO  (power bank only, no laptop cable)
 *   The suit makes its own Wi-Fi network (SereniSuit-Demo). Join it from the
 *   laptop AND the phone, open http://192.168.4.1 on both - they pair and run
 *   side by side.
 *
 * OLED  (SSD1306 0.96", I2C: SDA 8, SCL 9, address 0x3C)
 *   At boot it shows the ML self-test result, then the Wi-Fi name, password
 *   and address, so the demo needs no laptop. After that: the live BPM, the
 *   status, and how many devices are paired. Set ENABLE_OLED 0 to run without
 *   it - the suit works exactly the same, just without the screen.
 *
 * WI-FI DASHBOARD  (website on the laptop + mobile app on the phone, same page)
 *     - "Paired" status: every open website/app says hello over a WebSocket
 *       and heart-beats every 5 s; the device lists who is paired
 *     - the SAME status label drives the website and the app
 *     - remote ON/OFF for the ECG sensor
 *     - an alert on every device after every BPM check
 *     - demo mode: type a BPM into the website or app and it drives the
 *       motor and both screens exactly as a live reading would
 *       (normal = shown only, abnormal = DANGER + vibration)
 *
 * BLUETOOTH  (optional second link, runs at the same time as Wi-Fi)
 *   The suit advertises as "SereniSuit"; the Bluetooth dashboard
 *   (demo1/bluetooth-app/index.html, Chrome or Edge on a laptop or Android
 *   phone) connects without joining any Wi-Fi.
 *
 * WHAT THE ML DOES HERE  (proof of capability, never drives the alert)
 *   A beat-SHAPE classifier trained on the PTB Diagnostic ECG heartbeat set
 *   (14,552 beats, ml/ecg_ptbdb_beats.py) is compiled in as plain C: an MLP
 *   187 -> 128 -> 32 -> 1, 110 KB, no ML library. At boot it scores 8
 *   held-out beats stored in flash and the result is shown on the dashboards.
 *   Offline: test accuracy 0.961, ROC-AUC 0.985 on a random beat split
 *   (optimistic for a new person: the CSVs have no patient IDs).
 *
 *   It is NOT run on the live AD8232 signal (ENABLE_LIVE_BEAT_SHAPE 0):
 *   checked on healthy ECG from other sources, it called only 0-14 % of
 *   normal beats "typical" - it has learned PTB's recording setup, not just
 *   heart physiology. Live use needs our own AD8232 recordings to retrain on
 *   (Phase 2). The live pipeline is kept below, switched off.
 *
 * SAFETY: only attach the electrodes to a person while the suit runs on the
 *   power bank, or the laptop runs on battery with its charger unplugged.
 *
 * Serial commands (115200, only when a laptop is connected):
 *     m  - re-run the ML self-test
 *     t  - toggle forced DANGER
 *     p  - stream the ECG for a serial plotter (again = stop)
 *     i  - print Wi-Fi status / dashboard URL
 *     h  - help
 * ==========================================================================*/

/* ----------------------------------------------------------- options ---
 * ENABLE_OLED 1 = the SSD1306 OLED is fitted (SDA 8, SCL 9, 0x3C)
 *             0 = no screen; the website, the phone app and the Serial
 *                 Monitor show everything instead. */
#ifndef ENABLE_OLED
#define ENABLE_OLED 1
#endif

#include <Arduino.h>
#if ENABLE_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
#endif
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <freertos/semphr.h>

#include "ecg_model.h"
#include "ecg_test_beats.h"
#include "web_dashboard.h"

/* ----------------------------------------------------------------- pins */
#define ECG_OUT_PIN   5          // AD8232 OUTPUT -> GPIO5 (ADC1, works with Wi-Fi on)
#define ECG_LOP_PIN   6          // AD8232 LO+   (HIGH = an electrode is off)
#define ECG_LOM_PIN   7          // AD8232 LO-
#define MOTOR_PIN     4          // -> DRV8833 IN1  (12-pin board: IN1 IN2 VCC GND IN3 IN4)
#define RGB_LED_PIN   38         // on-board RGB LED - kept dark ("RGB@IO38"; v1.0 boards: 48)
#define I2C_SDA       8          // OLED SDA (the MAX30102 used to share this bus)
#define I2C_SCL       9          // OLED SCL
#define OLED_ADDR     0x3C       // try 0x3D if the screen stays blank
#define SCREEN_W      128
#define SCREEN_H       64

#if ENABLE_OLED
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
static bool oledOk = false;                  // the screen answered on I2C
#endif

/* ----------------------------------------------------------- options */
#define ENABLE_LIVE_BEAT_SHAPE 0 // 1 = score live beats with the PTB model (see above - not reliable yet)

/* ------------------------------------------------------------- Wi-Fi ---
 * USE_ROUTER_WIFI 0  ->  the suit creates its own network (DEFAULT, best for
 *                        the review: no router, no laptop cable, fixed
 *                        address http://192.168.4.1).
 * USE_ROUTER_WIFI 1  ->  join WIFI_SSID first (e.g. your phone's hotspot, so
 *                        the laptop keeps internet); if that fails within
 *                        WIFI_CONNECT_TIMEOUT_MS it falls back to its own
 *                        network. The address it got is printed on the Serial Monitor.
 * ---------------------------------------------------------------------- */
#define USE_ROUTER_WIFI         0
#define WIFI_SSID               "YOUR_WIFI_SSID"
#define WIFI_PASSWORD           "YOUR_WIFI_PASSWORD"
#define WIFI_CONNECT_TIMEOUT_MS 8000

#define AP_SSID         "SereniSuit-Demo"
#define AP_PASSWORD     "seresuit123"   // >= 8 chars, required for WPA2
#define AP_CHANNEL      1
#define AP_MAX_STATIONS 4               // laptop + phone + 2 spare
#define MDNS_NAME       "serenisuit"    // also reachable as http://serenisuit.local

/* 11 dBm is plenty for one room and trims the Wi-Fi transmit current spikes,
 * which matters on a power bank that is also feeding the motor. */
#define WIFI_TX_POWER   WIFI_POWER_11dBm

/* -------------------------------------------------------- Bluetooth ---
 * Bluetooth LE only - the ESP32-S3 has no "classic" Bluetooth. Web Bluetooth
 * works in Chrome / Edge on laptops and Android; iPhone Safari does not
 * support it (use the Wi-Fi dashboard there). Set to 0 to switch it off.
 * ---------------------------------------------------------------------- */
#define ENABLE_BLUETOOTH  1
#define BLE_NAME          "SereniSuit"
#define BLE_SERVICE_UUID  "5e7e5000-24e2-498b-a477-c675190a743e"
#define BLE_STATE_UUID    "5e7e5001-24e2-498b-a477-c675190a743e"  // read+notify: live status
#define BLE_DEVICES_UUID  "5e7e5002-24e2-498b-a477-c675190a743e"  // read+notify: paired devices
#define BLE_CMD_UUID      "5e7e5003-24e2-498b-a477-c675190a743e"  // write: hello/sensors/demo
#define BLE_INFO_UUID     "5e7e5004-24e2-498b-a477-c675190a743e"  // read: network, ML self-test
#define BLE_ID_BASE       0x10000UL     // pairing ids for Bluetooth links (WebSocket ids stay below)

#if ENABLE_BLUETOOTH
  #include <NimBLEDevice.h>
#endif

#define STATE_BROADCAST_MS 500          // how often every screen is refreshed
#define MAX_PAIRED         6            // devices listed as paired at once
#define PAIR_TIMEOUT_MS    15000        // silent this long -> no longer paired

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

/* --------------------------------------------------------- LEDC compat --
 * Arduino-ESP32 core 3.x replaced ledcSetup/ledcAttachPin with ledcAttach.
 * This builds on both, so it does not matter which core PlatformIO resolves.
 * ---------------------------------------------------------------------- */
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define MOTOR_INIT()  ledcAttach(MOTOR_PIN, MOTOR_FREQ, MOTOR_RES)
  #define MOTOR_SET(d)  ledcWrite(MOTOR_PIN, (d))
#else
  #define MOTOR_INIT()  do { ledcSetup(MOTOR_CH, MOTOR_FREQ, MOTOR_RES); \
                             ledcAttachPin(MOTOR_PIN, MOTOR_CH); } while (0)
  #define MOTOR_SET(d)  ledcWrite(MOTOR_CH, (d))
#endif

/* ------------------------------------------------------------ thresholds */
#define BPM_NORMAL_LOW    60
#define BPM_NORMAL_HIGH  100
#define DEMO_BPM_MIN       1
#define DEMO_BPM_MAX     300

/* --------------------------------------------------------- motor / PWM */
#define MOTOR_CH        0
#define MOTOR_FREQ   5000
#define MOTOR_RES       8
#define MOTOR_DUTY    153      // 60% of 255 -> ~3.0 V from the 5 V rail
#define MOTOR_PULSE_MS 400

/* ------------------------------------ requests from the website / app ---
 * Written by the web server or Bluetooth task, read by loop(). Plain flags/values only -
 * loop() does the actual work, so the two tasks never fight over the
 * beat-averaging buffer. */
static volatile bool     sensorsEnabled     = true;
static volatile bool     demoActive         = false;
static volatile int      demoBpm            = 0;
static volatile uint32_t demoSeq            = 0;     // bumps on every fake input
static volatile bool     resetAvgRequested  = false;
static volatile bool     pairingChanged     = false;

/* ------------------------------------------------------ shared state ---
 * Touched by both tasks -> guarded by stateLock. */
struct PairedDevice {
  bool          used;
  uint32_t      id;          // WebSocket client id, or BLE_ID_BASE + BLE connection
  char          via;         // 'w' = Wi-Fi, 'b' = Bluetooth
  char          label[32];   // "Website (Mac)", "Mobile app (Android)" ...
  char          token[12];   // random tag the page picks, so it can spot itself
  unsigned long lastSeen;
};
static PairedDevice      paired[MAX_PAIRED];
static String            lastStateJson =
    "{\"status\":\"booting\",\"bpm\":0,\"danger\":false,\"sensorsOn\":true,"
    "\"demoActive\":false,\"checkSeq\":0,\"paired\":0,\"devices\":[]}";
static SemaphoreHandle_t stateLock = nullptr;

static inline void lockState()   { xSemaphoreTake(stateLock, portMAX_DELAY); }
static inline void unlockState() { xSemaphoreGive(stateLock); }

/* ------------------------------------------ boot-time facts (read-only) */
static char netName[33]   = "";
static char ipStr[20]     = "0.0.0.0";
static bool ownNetwork    = true;       // true = suit's own AP
static int  mlCorrect     = 0;
static unsigned long mlMeanUs = 0;

/* ================================================== pairing table ===
 * All helpers expect the caller to hold stateLock. */
static bool pairUpsert(uint32_t id, const char *label, char via, const char *token) {
  for (int i = 0; i < MAX_PAIRED; i++) {
    if (paired[i].used && paired[i].id == id) {
      strlcpy(paired[i].label, label, sizeof(paired[i].label));
      strlcpy(paired[i].token, token, sizeof(paired[i].token));
      paired[i].lastSeen = millis();
      return false;                                   // already paired
    }
  }
  for (int i = 0; i < MAX_PAIRED; i++) {
    if (!paired[i].used) {
      paired[i].used = true;
      paired[i].id   = id;
      paired[i].via  = via;
      strlcpy(paired[i].label, label, sizeof(paired[i].label));
      strlcpy(paired[i].token, token, sizeof(paired[i].token));
      paired[i].lastSeen = millis();
      return true;                                    // newly paired
    }
  }
  return false;                                       // table full
}

static bool pairRemove(uint32_t id) {
  for (int i = 0; i < MAX_PAIRED; i++) {
    if (paired[i].used && paired[i].id == id) { paired[i].used = false; return true; }
  }
  return false;
}

static bool pairExpire() {
  bool changed = false;
  unsigned long now = millis();
  for (int i = 0; i < MAX_PAIRED; i++) {
    if (paired[i].used && now - paired[i].lastSeen > PAIR_TIMEOUT_MS) {
      paired[i].used = false;
      changed = true;
    }
  }
  return changed;
}

static int pairCount() {
  int n = 0;
  for (int i = 0; i < MAX_PAIRED; i++) if (paired[i].used) n++;
  return n;
}

/* ================================================ shared commands ===
 * The website (HTTP / WebSocket) and the Bluetooth page both end up here,
 * so a command does exactly the same thing whichever link it came over. */
static bool cmdHello(uint32_t clientId, const char *label, char via, const char *token) {
  lockState();
  bool isNew = pairUpsert(clientId, label, via, token);
  unlockState();
  if (isNew) pairingChanged = true;
  return isNew;
}

static void cmdSensors(bool on) {
  if (on && !sensorsEnabled) resetAvgRequested = true;   // no stale average
  sensorsEnabled = on;
}

static bool cmdDemo(int v) {
  if (v < DEMO_BPM_MIN || v > DEMO_BPM_MAX) return false;
  demoBpm    = v;
  demoActive = true;
  demoSeq    = demoSeq + 1;                              // every fake input counts as a new check
  return true;
}

static void cmdDemoClear() {
  demoActive = false;
  resetAvgRequested = true;
}

static bool          motorOn      = false;
static unsigned long motorToggle  = 0;
static bool          forceDanger  = false;   // serial 't'
static bool          plotEcg      = false;   // serial 'p'

/* ------------------------------------------------------------ status ---
 * ONE status value drives the website and the app, so both always say the
 * same thing. */
enum SuitStatus { ST_BOOTING, ST_SENSORS_OFF, ST_SENSOR_ERROR, ST_LEADS_OFF,
                  ST_READING, ST_NORMAL, ST_DANGER };
static const char *STATUS_NAME[] = { "booting", "sensors_off", "sensor_error",
                                     "leads_off", "reading", "normal", "danger" };

/* ================================================================ ECG ===
 * AD8232 single-lead ECG, sampled at 250 Hz by its own FreeRTOS task
 * (loop() runs every 20 ms - far too slow for an ECG).
 *
 * Heart rate - a simplified Pan-Tompkins QRS detector:
 *   band-pass 5-15 Hz -> derivative -> square -> 150 ms moving-window
 *   integral -> adaptive threshold, 250 ms refractory period, T-wave check.
 *   BPM = 60 / median of the last 8 R-R intervals (needs MIN_BEATS first).
 *
 * Electrodes - LO+ / LO- go HIGH when an electrode is off -> "leads_off".
 *   With a pull-up, a missing or unpowered AD8232 also reads "leads off".
 *
 * Beat shape (ML, off by default - ENABLE_LIVE_BEAT_SHAPE) - a 0.5 Hz
 *   high-passed copy is averaged down to 125 Hz (also used by the 'p'
 *   plotter) and kept for 10 s; loop() cuts beats out of it exactly like
 *   the PTB dataset and scores them with the MLP. */
#define ECG_FS            250          // Hz
#define ECG_PERIOD_MS     4
#define RR_KEEP           8            // R-R intervals in the median
#define MIN_BEATS         4            // intervals needed before a BPM is shown
#define RR_MIN_MS         300          // 200 BPM
#define RR_MAX_MS         2000         // 30 BPM
#define LEARN_SAMPLES     (2 * ECG_FS) // 2 s to learn the signal level
#define NO_BEAT_RELEARN   (3 * ECG_FS) // no beat for 3 s -> learn again
#define LEADS_OFF_DEBOUNCE (ECG_FS / 10)   // 100 ms off  -> leads off
#define LEADS_ON_DEBOUNCE  (ECG_FS / 2)    // 500 ms on   -> leads on
#define FLAT_RANGE        25           // ADC counts over 2 s -> no signal
#define MWI_LEN           38           // 150 ms at 250 Hz
#define SHAPE_FS          125
#define SHAPE_WIN         (10 * SHAPE_FS)  // 10-second window, 1250 samples

struct Biquad {                       // transposed direct form II
  float b0, b1, b2, a1, a2, z1, z2;
  float step(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

/* 2nd-order Butterworth high- or low-pass (RBJ cookbook, Q = 0.707) */
static void biquadDesign(Biquad &q, bool highpass, float fc, float fs) {
  float w0 = 2.0f * (float)M_PI * fc / fs, c = cosf(w0), alpha = sinf(w0) / (2.0f * 0.70710678f);
  float a0 = 1.0f + alpha;
  if (highpass) { q.b0 = (1 + c) / 2; q.b1 = -(1 + c); q.b2 = (1 + c) / 2; }
  else          { q.b0 = (1 - c) / 2; q.b1 =  (1 - c); q.b2 = (1 - c) / 2; }
  q.a1 = -2.0f * c;
  q.a2 = 1.0f - alpha;
  q.b0 /= a0; q.b1 /= a0; q.b2 /= a0; q.a1 /= a0; q.a2 /= a0;
  q.z1 = q.z2 = 0;
}

/* everything below is owned by the ECG task, except the "published" block */
static Biquad   qrsHp, qrsLp;
static float    dBuf[5];
static float    mwiBuf[MWI_LEN], mwiSum;
static int      mwiPos;
static float    mwiPrev1, mwiPrev2;
static float    spki, npki, qrsThr, lastQrsPeak;
static float    learnMax, learnSum;
static uint32_t ecgN;                 // samples since the detector (re)started
static int32_t  lastQrsN;             // sample index of the last QRS, -1 = none yet
static uint16_t rrMs[RR_KEEP];
static int      rrCount, rrPos;
static int      leadsOffRun, leadsOnRun;
static int      rawMinBlk = 4095, rawMaxBlk = 0, blkN;
static float    hpPrevX, hpPrevY;      // 0.5 Hz high-pass for the beat-shape copy
static float    decimAcc;
static int      decimN;
static float    shapeRing[SHAPE_WIN];
static int      shapeRingPos, shapeFill;

/* published by the ECG task, read by loop() - single 32-bit values */
static volatile bool     ecgLeadsOff    = true;
static volatile bool     ecgSignalBad   = false;
static volatile bool     ecgHave        = false;   // enough beats for a BPM
static volatile int      ecgBpm         = 0;
static volatile uint32_t ecgBeats       = 0;       // QRS complexes since boot
static volatile int      ecgRawMid      = 0;       // last 2-s block: centre and swing
static volatile int      ecgRawSwing    = 0;
static volatile bool     ecgResetReq    = false;   // loop() -> task: start over
static volatile bool     shapeReady     = false;   // task -> loop(): 10 s window ready
static float             shapeWin[SHAPE_WIN];      // written only while !shapeReady
static volatile int16_t  plotRing[64];
static volatile uint8_t  plotHead;

static void qrsReset() {
  biquadDesign(qrsHp, true, 5.0f, ECG_FS);
  biquadDesign(qrsLp, false, 15.0f, ECG_FS);
  memset(dBuf, 0, sizeof(dBuf));
  memset(mwiBuf, 0, sizeof(mwiBuf));
  mwiSum = 0; mwiPos = 0; mwiPrev1 = mwiPrev2 = 0;
  spki = npki = qrsThr = lastQrsPeak = 0;
  learnMax = learnSum = 0;
  ecgN = 0;
  lastQrsN = -1;
  rrCount = rrPos = 0;
  ecgHave = false;
  ecgBpm = 0;
  shapeFill = -2 * SHAPE_FS;            // let the 0.5 Hz high-pass settle for 2 s first
}

static int rrMedianMs() {
  uint16_t t[RR_KEEP];
  int n = rrCount < RR_KEEP ? rrCount : RR_KEEP;
  memcpy(t, rrMs, sizeof(t));
  for (int i = 1; i < n; i++)                       // insertion sort, n <= 8
    for (int j = i; j > 0 && t[j - 1] > t[j]; j--) { uint16_t k = t[j]; t[j] = t[j - 1]; t[j - 1] = k; }
  return n % 2 ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2;
}

/* one band-passed, integrated sample -> true when a QRS was just confirmed */
static bool qrsStep(float raw) {
  float bp = qrsLp.step(qrsHp.step(raw));
  memmove(dBuf + 1, dBuf, 4 * sizeof(float));
  dBuf[0] = bp;
  float der = (2 * dBuf[0] + dBuf[1] - dBuf[3] - 2 * dBuf[4]) / 8.0f;
  float sq  = der * der;
  mwiSum += sq - mwiBuf[mwiPos];
  mwiBuf[mwiPos] = sq;
  mwiPos = (mwiPos + 1) % MWI_LEN;
  float mwi = mwiSum / MWI_LEN;
  ecgN++;

  bool beat = false;
  if (ecgN <= LEARN_SAMPLES) {                      // learn the level first
    if (ecgN > ECG_FS / 2) {                        // skip the filters settling
      if (mwi > learnMax) learnMax = mwi;
      learnSum += mwi;
    }
    if (ecgN == LEARN_SAMPLES) {
      spki = 0.33f * learnMax;
      npki = 0.5f * learnSum / (LEARN_SAMPLES - ECG_FS / 2);
      qrsThr = npki + 0.25f * (spki - npki);
    }
  } else if (mwiPrev1 > mwiPrev2 && mwiPrev1 >= mwi) {   // local maximum one sample ago
    float    pk = mwiPrev1;
    int32_t  t  = (int32_t)ecgN - 1;
    int32_t  since = lastQrsN < 0 ? INT32_MAX : t - lastQrsN;
    if (pk > qrsThr && since > ECG_FS / 4) {        // 250 ms refractory
      bool tWave = since < (ECG_FS * 36) / 100 && pk < 0.5f * lastQrsPeak;
      if (tWave) {
        npki = 0.125f * pk + 0.875f * npki;
      } else {
        spki = 0.125f * pk + 0.875f * spki;
        if (lastQrsN >= 0) {
          int ms = (int)(since * 1000 / ECG_FS);
          if (ms >= RR_MIN_MS && ms <= RR_MAX_MS) {
            rrMs[rrPos] = (uint16_t)ms;
            rrPos = (rrPos + 1) % RR_KEEP;
            if (rrCount < RR_KEEP * 1000) rrCount++;
          }
        }
        lastQrsN = t;
        lastQrsPeak = pk;
        beat = true;
      }
    } else {
      npki = 0.125f * pk + 0.875f * npki;
    }
    qrsThr = npki + 0.25f * (spki - npki);
  }
  mwiPrev2 = mwiPrev1;
  mwiPrev1 = mwi;

  /* nothing for 3 s (electrode moved, big artefact) -> learn again */
  if (ecgN > LEARN_SAMPLES && (lastQrsN < 0 ? (int32_t)ecgN - LEARN_SAMPLES : (int32_t)ecgN - lastQrsN) > NO_BEAT_RELEARN) {
    qrsReset();
  }
  return beat;
}

/* one ADC sample every 4 ms */
static void ecgTick() {
  if (ecgResetReq) { ecgResetReq = false; qrsReset(); }

  /* electrodes: debounced LO+ / LO- */
  bool off = digitalRead(ECG_LOP_PIN) || digitalRead(ECG_LOM_PIN);
  if (off) { leadsOnRun = 0;  if (++leadsOffRun >= LEADS_OFF_DEBOUNCE && !ecgLeadsOff) { ecgLeadsOff = true; qrsReset(); } }
  else     { leadsOffRun = 0; if (++leadsOnRun  >= LEADS_ON_DEBOUNCE  &&  ecgLeadsOff) { ecgLeadsOff = false; qrsReset(); } }

  int raw = analogRead(ECG_OUT_PIN);

  /* signal check over 2-s blocks: flat or stuck at a rail = no ECG */
  if (raw < rawMinBlk) rawMinBlk = raw;
  if (raw > rawMaxBlk) rawMaxBlk = raw;
  if (++blkN >= 2 * ECG_FS) {
    ecgRawMid   = (rawMinBlk + rawMaxBlk) / 2;
    ecgRawSwing = rawMaxBlk - rawMinBlk;
    ecgSignalBad = (rawMaxBlk - rawMinBlk) < FLAT_RANGE || rawMinBlk > 4050 || rawMaxBlk < 45;
    rawMinBlk = 4095; rawMaxBlk = 0; blkN = 0;
  }

  /* 125 Hz copy for the plotter and the beat-shape model */
  float hp = 0.98757f * (hpPrevY + raw - hpPrevX);  // 0.5 Hz high-pass
  hpPrevX = raw; hpPrevY = hp;
  decimAcc += hp;
  if (++decimN == 2) {
    float v = decimAcc / 2.0f;
    decimAcc = 0; decimN = 0;
    plotRing[plotHead & 63] = (int16_t)v;
    plotHead++;
    shapeRing[shapeRingPos] = v;
    shapeRingPos = (shapeRingPos + 1) % SHAPE_WIN;
    if (++shapeFill >= SHAPE_WIN) {                 // a full new 10 s window
      shapeFill = 0;
      if (ENABLE_LIVE_BEAT_SHAPE && !shapeReady && !ecgLeadsOff && !ecgSignalBad && ecgHave) {
        for (int i = 0; i < SHAPE_WIN; i++) shapeWin[i] = shapeRing[(shapeRingPos + i) % SHAPE_WIN];
        shapeReady = true;
      }
    }
  }

  if (!sensorsEnabled || ecgLeadsOff) return;       // nothing to detect
  if (qrsStep((float)raw)) ecgBeats = ecgBeats + 1;
  int n = rrCount < RR_KEEP ? rrCount : RR_KEEP;
  if (n >= MIN_BEATS) {
    ecgBpm  = (60000 + rrMedianMs() / 2) / rrMedianMs();
    ecgHave = true;
  } else {
    ecgHave = false;
  }
}

static void ecgTask(void *) {
  TickType_t next = xTaskGetTickCount();
  for (;;) {
    ecgTick();
    vTaskDelayUntil(&next, pdMS_TO_TICKS(ECG_PERIOD_MS));
  }
}

/* ---------------------------------------------- beat shape (ML, live) ---
 * Same recipe as the PTB dataset (Kachuee et al. 2018) and as
 * extract_beats() in ml/ecg_ptbdb_beats.py:
 *   scale the 10 s window to 0..1 (flip it if the QRS points down),
 *   R-peaks = local maxima above 0.9, T = median R-R,
 *   beat = R-peak .. R-peak + 1.2 T, zero-padded to 187 samples. */
static int  shapePct   = -1;          // % typical beats in the last window, -1 = none
static int  shapeBeats = 0;

static int cmpFloat(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

/* returns the number of beats scored; *typical = how many looked typical */
static int shapeScoreWindow(float *x, int n, int *typical) {
  static float tmp[SHAPE_WIN];
  static int   r[64];
  static float beat[ECG_BEAT_LEN];
  *typical = 0;
  memcpy(tmp, x, n * sizeof(float));
  qsort(tmp, n, sizeof(float), cmpFloat);
  float med = n % 2 ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2;
  float lo = 1e30f, hi = -1e30f;
  for (int i = 0; i < n; i++) { x[i] -= med; if (x[i] < lo) lo = x[i]; if (x[i] > hi) hi = x[i]; }
  if (-lo > hi) {                                   // QRS points down: electrodes swapped
    for (int i = 0; i < n; i++) x[i] = -x[i];
    float t = lo; lo = -hi; hi = -t;
  }
  if (hi - lo < 1e-9f) return 0;
  for (int i = 0; i < n; i++) x[i] = (x[i] - lo) / (hi - lo);

  int nr = 0;
  for (int i = 1; i < n - 1 && nr < 64; i++) {
    if (x[i] > x[i - 1] && x[i + 1] <= x[i] && x[i] > 0.9f) {
      if (nr == 0 || i - r[nr - 1] > (SHAPE_FS / 4)) r[nr++] = i;      // > 31 samples apart
      else if (x[i] > x[r[nr - 1]]) r[nr - 1] = i;
    }
  }
  if (nr < 2) return 0;
  int d[63];
  for (int i = 0; i < nr - 1; i++) d[i] = r[i + 1] - r[i];
  for (int i = 1; i < nr - 1; i++)
    for (int j = i; j > 0 && d[j - 1] > d[j]; j--) { int k = d[j]; d[j] = d[j - 1]; d[j - 1] = k; }
  int m = nr - 1;
  int T = m % 2 ? d[m / 2] : (d[m / 2 - 1] + d[m / 2]) / 2;   // int(np.median)
  int len = (12 * T) / 10;
  if (len > ECG_BEAT_LEN) len = ECG_BEAT_LEN;

  int scored = 0;
  for (int k = 0; k < nr; k++) {
    if (r[k] + len > n) continue;
    memset(beat, 0, sizeof(beat));
    memcpy(beat, x + r[k], len * sizeof(float));
    if (ecg_beat_abnormal_prob(beat) < 0.5f) (*typical)++;
    scored++;
  }
  return scored;
}

/* ======================================================= ML self-test ===*/
static void runSelfTest() {
  int correct = 0;
  unsigned long total_us = 0;
  float maxDiff = 0;

  Serial.println();
  Serial.println(F("=== ECG beat-shape model (PTB) - on-device self-test ==="));
  Serial.println(F("beat  true      predicted   P(abnormal)   us"));

  for (int w = 0; w < N_TEST_BEATS; w++) {
    unsigned long t0 = micros();
    float p = ecg_beat_abnormal_prob(test_beats[w]);
    unsigned long dt = micros() - t0;
    total_us += dt;
    float diff = fabsf(p - test_beat_expected[w]);
    if (diff > maxDiff) maxDiff = diff;

    int pred = (p > 0.5f) ? 1 : 0;
    if (pred == test_beat_labels[w]) correct++;

    Serial.printf("  %d   %-8s  %-10s  %8.3f   %5lu%s\n",
                  w,
                  test_beat_labels[w] ? "abnormal" : "normal",
                  pred                ? "abnormal" : "normal",
                  p, dt,
                  pred == test_beat_labels[w] ? "" : "   <-- MISS");
  }

  mlCorrect = correct;
  mlMeanUs  = total_us / N_TEST_BEATS;

  Serial.printf("correct: %d/%d   mean inference %lu us   matches Python: %s\n",
                correct, N_TEST_BEATS, mlMeanUs, maxDiff < 1e-3f ? "yes" : "NO");
  Serial.printf("model: MLP 187-128-32-1, %d weights (%d KB), no ML library\n",
                ECG_MODEL_PARAMS, (int)(ECG_MODEL_PARAMS * 4 / 1024));
  Serial.printf("offline test accuracy 0.961 (AUC 0.985) - random beat split, optimistic for a new person\n");
#if ENABLE_LIVE_BEAT_SHAPE
  Serial.printf("live beats ARE scored (research preview); alerts use the 60-100 BPM threshold\n");
#else
  Serial.printf("live AD8232 beats are not scored (does not transfer beyond PTB yet); alerts use 60-100 BPM\n");
#endif
  Serial.printf("free heap: %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.println(F("======================================================="));
  Serial.println();
}

/* ======================================================== Wi-Fi info ===*/
static void printWifiInfo() {
  Serial.printf("Wi-Fi: %s \"%s\"   Dashboard: http://%s/  (or http://%s.local/)\n",
                ownNetwork ? "own network" : "joined", netName, ipStr, MDNS_NAME);
  if (ownNetwork) Serial.printf("Wi-Fi password: %s\n", AP_PASSWORD);
#if ENABLE_BLUETOOTH
  Serial.printf("Bluetooth: \"%s\"  (open demo1/bluetooth-app in Chrome/Edge)\n", BLE_NAME);
#endif
}

static void startOwnNetwork() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, AP_MAX_STATIONS);
  WiFi.setTxPower(WIFI_TX_POWER);
  ownNetwork = true;
  strlcpy(netName, AP_SSID, sizeof(netName));
  strlcpy(ipStr, WiFi.softAPIP().toString().c_str(), sizeof(ipStr));
}

static void startWiFi() {
#if USE_ROUTER_WIFI
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                 // snappier WebSocket updates
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setTxPower(WIFI_TX_POWER);
    ownNetwork = false;
    strlcpy(netName, WIFI_SSID, sizeof(netName));
    strlcpy(ipStr, WiFi.localIP().toString().c_str(), sizeof(ipStr));
  } else {
    Serial.println(F("Could not join WIFI_SSID - starting the suit's own network"));
    WiFi.disconnect(true);
    startOwnNetwork();
  }
#else
  startOwnNetwork();
#endif

  if (MDNS.begin(MDNS_NAME)) MDNS.addService("http", "tcp", 80);
  printWifiInfo();
}

/* ================================================= broadcast state ===*/
/* Builds the one JSON snapshot every website/app renders, caches it for
 * newly connected clients, and pushes it to all of them. */
#if ENABLE_BLUETOOTH
static void bleNotify(const String &state, const String &devices);   // defined below
#endif

static void broadcastState(SuitStatus st, int bpm, uint32_t checkSeq) {
  String out, bleState, bleDevices;

  lockState();
  JsonDocument doc;
  doc["status"]     = STATUS_NAME[st];
  doc["bpm"]        = bpm;
  doc["danger"]     = (st == ST_DANGER);
  doc["sensorsOn"]  = (bool)sensorsEnabled;
  doc["demoActive"] = (bool)demoActive;
  doc["checkSeq"]   = checkSeq;
  doc["shape"]      = shapePct;          // % typical beat shape, -1 = none yet
  doc["shapeN"]     = shapeBeats;
  doc["paired"]     = pairCount();
  JsonArray devices = doc["devices"].to<JsonArray>();
  for (int i = 0; i < MAX_PAIRED; i++) {
    if (!paired[i].used) continue;
    JsonObject d = devices.add<JsonObject>();
    d["id"]    = paired[i].id;
    d["label"] = String(paired[i].label) + (paired[i].via == 'b' ? " · Bluetooth" : " · Wi-Fi");
    d["via"]   = paired[i].via == 'b' ? "bt" : "wifi";
  }
  serializeJson(doc, out);
  lastStateJson = out;

#if ENABLE_BLUETOOTH
  /* Bluetooth packets are small, so the live status uses short keys
   * (s=status b=bpm so=sensors on da=demo active q=check number n=paired
   *  sh=% typical beat shape sn=beats scored). */
  JsonDocument bs;
  bs["s"]  = STATUS_NAME[st];
  bs["b"]  = bpm;
  bs["so"] = sensorsEnabled ? 1 : 0;
  bs["da"] = demoActive ? 1 : 0;
  bs["q"]  = checkSeq;
  bs["n"]  = pairCount();
  bs["sh"] = shapePct;
  bs["sn"] = shapeBeats;
  serializeJson(bs, bleState);

  JsonDocument bd;                      // [[id, label, "wifi"|"bt", token], ...]
  JsonArray list = bd.to<JsonArray>();
  for (int i = 0; i < MAX_PAIRED; i++) {
    if (!paired[i].used) continue;
    JsonArray e = list.add<JsonArray>();
    e.add(paired[i].id);
    e.add(String(paired[i].label));
    e.add(paired[i].via == 'b' ? "bt" : "wifi");
    e.add(String(paired[i].token));
  }
  serializeJson(bd, bleDevices);
#endif
  unlockState();

  ws.textAll(out);
#if ENABLE_BLUETOOTH
  bleNotify(bleState, bleDevices);
#endif
}

/* ======================================================= WebSocket ===*/
static void onWsEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      String snapshot;
      lockState();
      snapshot = lastStateJson;
      unlockState();
      client->text(snapshot);          // show the reading at once; paired after "hello"
      break;
    }

    case WS_EVT_DISCONNECT: {
      lockState();
      bool removed = pairRemove(client->id());
      unlockState();
      if (removed) pairingChanged = true;
      break;
    }

    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)) break;

      JsonDocument msg;
      if (deserializeJson(msg, data, len) != DeserializationError::Ok) break;

      const char *kind = msg["type"] | "";
      if (strcmp(kind, "hello") == 0) {          // sent on open and every 5 s
        const char *label = msg["device"] | "Device";
        const char *token = msg["token"] | "";
        if (cmdHello(client->id(), label, 'w', token)) {
          client->text(String("{\"type\":\"paired\",\"id\":") + client->id() + "}");
        }
      }
      break;
    }

    default:
      break;
  }
}

/* ======================================================= HTTP routes ===*/
static void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    AsyncWebServerResponse *res = req->beginResponse_P(200, "text/html", DASHBOARD_HTML);  // streamed from flash
    res->addHeader("Cache-Control", "no-cache");
    req->send(res);
  });

  server.on("/manifest.json", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "application/manifest+json", DASHBOARD_MANIFEST);
  });

  server.on("/icon.svg", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "image/svg+xml", DASHBOARD_ICON_SVG);
  });

  server.on("/sw.js", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "application/javascript", DASHBOARD_SW);
  });

  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *req) {
    String snapshot;
    lockState();
    snapshot = lastStateJson;
    unlockState();
    req->send(200, "application/json", snapshot);
  });

  /* boot-time facts - set once in setup(), read-only afterwards */
  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["network"]   = netName;
    doc["ownNet"]    = ownNetwork;
    doc["ip"]        = ipStr;
    doc["mdns"]      = MDNS_NAME ".local";
    doc["sensor"]    = "AD8232 ECG";
#if ENABLE_OLED
  doc["oled"]      = oledOk ? "SSD1306 0x3C" : "not found - check SDA 8 / SCL 9";
#else
  doc["oled"]      = "not fitted";
#endif
    doc["mlCorrect"] = mlCorrect;
    doc["mlTotal"]   = N_TEST_BEATS;
    doc["mlUs"]      = mlMeanUs;
    doc["mlKb"]      = (int)(ECG_MODEL_PARAMS * 4 / 1024);
#if ENABLE_BLUETOOTH
    doc["bluetooth"] = BLE_NAME;
#endif
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/sensors", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len) == DeserializationError::Ok && !doc["on"].isNull()) {
        cmdSensors(doc["on"].as<bool>());
      }
      req->send(200, "application/json", "{\"ok\":true}");
    });

  server.on("/api/demo", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      bool ok = false;
      if (deserializeJson(doc, data, len) == DeserializationError::Ok) {
        bool clearFlag = doc["clear"] | false;
        if (clearFlag) {
          cmdDemoClear();
          ok = true;
        } else if (!doc["bpm"].isNull()) {
          ok = cmdDemo(doc["bpm"].as<int>());
        }
      }
      req->send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
    });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
}

/* ======================================================== Bluetooth ===
 * GATT service "SereniSuit":
 *   STATE   read + notify   {"s":"normal","b":72,"so":1,"da":0,"q":5,"n":2,"sh":-1,"sn":0}
 *   DEVICES read + notify   [[id,"Website (Mac)","wifi","k3f9"], ...]
 *   CMD     write           {"type":"hello","device":"...","token":"..."}
 *                           {"type":"sensors","on":false}
 *                           {"type":"demo","bpm":140} / {"type":"demo","clear":true}
 *   INFO    read            network, address, ECG sensor, ML self-test
 * A Bluetooth link counts as paired once its page says hello (same rule as
 * Wi-Fi) and drops off the list when the link closes. */
#if ENABLE_BLUETOOTH
static NimBLECharacteristic *bleStateChr   = nullptr;
static NimBLECharacteristic *bleDevicesChr = nullptr;
static NimBLECharacteristic *bleInfoChr    = nullptr;
static String                bleLastDevices;

class SuitServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *srv, ble_gap_conn_desc *desc) override {
    NimBLEDevice::startAdvertising();          // stay visible so a second device can join
  }
  void onDisconnect(NimBLEServer *srv, ble_gap_conn_desc *desc) override {
    lockState();
    bool removed = pairRemove((uint32_t)(BLE_ID_BASE + desc->conn_handle));
    unlockState();
    if (removed) pairingChanged = true;         // NimBLE restarts advertising by itself
  }
};

class SuitCommandCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *chr, ble_gap_conn_desc *desc) override {
    NimBLEAttValue value = chr->getValue();
    JsonDocument msg;
    if (deserializeJson(msg, (const char *)value.data(), value.length()) != DeserializationError::Ok) return;

    const char *kind = msg["type"] | "";
    if (strcmp(kind, "hello") == 0) {
      cmdHello((uint32_t)(BLE_ID_BASE + desc->conn_handle),
               msg["device"] | "Device", 'b', msg["token"] | "");
    } else if (strcmp(kind, "sensors") == 0 && !msg["on"].isNull()) {
      cmdSensors(msg["on"].as<bool>());
    } else if (strcmp(kind, "demo") == 0) {
      bool clearFlag = msg["clear"] | false;
      if (clearFlag)                   cmdDemoClear();
      else if (!msg["bpm"].isNull())   cmdDemo(msg["bpm"].as<int>());
    }
  }
};

static void bleSetText(NimBLECharacteristic *chr, const String &text) {
  chr->setValue((const uint8_t *)text.c_str(), text.length());
}

static void startBluetooth() {
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setMTU(247);                   // room for a whole status packet

  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(new SuitServerCallbacks());

  NimBLEService *svc = srv->createService(BLE_SERVICE_UUID);
  bleStateChr   = svc->createCharacteristic(BLE_STATE_UUID,   NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  bleDevicesChr = svc->createCharacteristic(BLE_DEVICES_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  bleInfoChr    = svc->createCharacteristic(BLE_INFO_UUID,    NIMBLE_PROPERTY::READ);
  NimBLECharacteristic *cmd =
                  svc->createCharacteristic(BLE_CMD_UUID,     NIMBLE_PROPERTY::WRITE);
  cmd->setCallbacks(new SuitCommandCallbacks());

  bleSetText(bleStateChr,   "{\"s\":\"booting\",\"b\":0,\"so\":1,\"da\":0,\"q\":0,\"n\":0,\"sh\":-1,\"sn\":0}");
  bleSetText(bleDevicesChr, "[]");
  bleSetText(bleInfoChr,    "{}");
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SERVICE_UUID);
  adv->setScanResponse(true);                  // name goes in the scan response
  adv->start();
}

/* boot-time facts for the Bluetooth page's "Suit" card */
static void bleSetInfo() {
  JsonDocument doc;
  doc["network"]   = netName;
  doc["ownNet"]    = ownNetwork;
  doc["ip"]        = ipStr;
  doc["bluetooth"] = BLE_NAME;
  doc["sensor"]    = "AD8232 ECG";
#if ENABLE_OLED
  doc["oled"]      = oledOk ? "SSD1306 0x3C" : "not found - check SDA 8 / SCL 9";
#else
  doc["oled"]      = "not fitted";
#endif
  doc["mlCorrect"] = mlCorrect;
  doc["mlTotal"]   = N_TEST_BEATS;
  doc["mlUs"]      = mlMeanUs;
  doc["mlKb"]      = (int)(ECG_MODEL_PARAMS * 4 / 1024);
  String out;
  serializeJson(doc, out);
  bleSetText(bleInfoChr, out);
}

static void bleNotify(const String &state, const String &devices) {
  if (!bleStateChr) return;
  bleSetText(bleStateChr, state);
  if (bleStateChr->getSubscribedCount() > 0) bleStateChr->notify();
  if (devices != bleLastDevices) {             // the list only goes out when it changes
    bleLastDevices = devices;
    bleSetText(bleDevicesChr, devices);
    if (bleDevicesChr->getSubscribedCount() > 0) bleDevicesChr->notify();
  }
}
#endif

#if ENABLE_OLED
/* =============================================================== OLED ===
 * The same status the website and the app show, on the suit itself. */
static void oledBottomBar(int nPaired) {
  display.drawFastHLine(0, 53, SCREEN_W, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 56);
  if (nPaired > 0) display.printf("Paired: %d device%s", nPaired, nPaired == 1 ? "" : "s");
  else             display.printf("Open %s", ipStr);
}

static void oledRender(SuitStatus st, int bpm, int nPaired) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  switch (st) {
    case ST_SENSORS_OFF:
      display.setCursor(0, 8);   display.println(F("Sensors OFF"));
      display.setCursor(0, 22);  display.println(F("Turned off from the"));
      display.setCursor(0, 32);  display.println(F("website / app"));
      break;

    case ST_SENSOR_ERROR:
      display.setCursor(0, 4);   display.println(F("No ECG signal"));
      display.setCursor(0, 18);  display.println(F("Check AD8232:"));
      display.setCursor(0, 28);  display.println(F("OUTPUT->5, 3.3V, GND"));
      display.setCursor(0, 40);  display.println(F("Demo input still works"));
      break;

    case ST_LEADS_OFF:
      display.setCursor(0, 8);   display.println(F("Attach electrodes"));
      display.setCursor(0, 24);  display.println(F("RA  right, below"));
      display.setCursor(0, 34);  display.println(F("LA  left, below"));
      display.setCursor(0, 44);  display.println(F("RL  lower right ribs"));
      break;

    case ST_READING:
    case ST_BOOTING:
      display.setCursor(0, 16);  display.println(F("Reading..."));
      display.setCursor(0, 30);  display.println(F("sit still, 4 beats"));
      break;

    case ST_NORMAL:
    case ST_DANGER:
      display.setTextSize(2);
      display.setCursor(0, 2);
      if (bpm > 0) display.printf("%d BPM", bpm);
      else         display.print(F("-- BPM"));
      if (demoActive) {
        display.setTextSize(1);
        display.setCursor(104, 0);
        display.print(F("DEMO"));
      }
      if (st == ST_DANGER) {
        display.setTextSize(2);
        display.setCursor(0, 26);
        display.println(F("DANGER!"));
      } else {
        display.setTextSize(1);
        display.setCursor(0, 28);  display.println(F("Status: NORMAL"));
        display.setCursor(0, 40);  display.println(F("range 60-100 BPM"));
      }
      break;
  }

  oledBottomBar(nPaired);
  display.display();
}

/* Shown at boot so the demo needs no laptop: the ML proof, then how to connect. */
static void oledBootScreens() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);   display.println(F("SereniSuit  ECG"));
  display.setCursor(0, 14);  display.printf("ML self-test: %d/%d\n", mlCorrect, N_TEST_BEATS);
  display.setCursor(0, 24);  display.printf("%lu us per beat\n", mlMeanUs);
  display.setCursor(0, 34);  display.println(F("on-chip, 110 KB model"));
  display.setCursor(0, 48);  display.println(F("Sensor: AD8232 ECG"));
  display.setCursor(0, 56);  display.println(F("pads: RA  LA  RL"));
  display.display();
  delay(2500);

  display.clearDisplay();
  display.setCursor(0, 0);
  display.println(ownNetwork ? F("Join Wi-Fi:") : F("Wi-Fi joined:"));
  display.setCursor(0, 10);  display.println(netName);
  if (ownNetwork) { display.setCursor(0, 20);  display.printf("Pass: %s\n", AP_PASSWORD); }
  display.setCursor(0, 34);  display.println(F("Open on laptop+phone:"));
  display.setCursor(0, 46);  display.printf("http://%s\n", ipStr);
#if ENABLE_BLUETOOTH
  display.setCursor(0, 56);  display.printf("BLE: %s\n", BLE_NAME);
#endif
  display.display();
  delay(4000);
}
#endif   /* ENABLE_OLED */

/* ================================================ Serial Monitor ===
 * No OLED and no status light in Demo 1: the status goes to the screens,
 * and every change is also printed here when a laptop is plugged in. */
static void printStatusChange(SuitStatus st, int bpm, int nPaired) {
  static SuitStatus lastSt = ST_BOOTING;
  static int lastBpm = -1, lastPaired = -1;
  if (st != lastSt || bpm != lastBpm || nPaired != lastPaired) {
    Serial.printf("status: %-12s bpm: %3d   paired devices: %d\n", STATUS_NAME[st], bpm, nPaired);
    lastSt = st; lastBpm = bpm; lastPaired = nPaired;
  }
}

static void printEcgSummary() {
  if (!sensorsEnabled) return;
  if (ecgLeadsOff) {
    Serial.println(F("ECG: electrodes not attached (LO+/LO- high) - "
                     "or check AD8232 3.3V/GND and LO+ -> 6, LO- -> 7"));
  } else if (ecgSignalBad) {
    Serial.printf("ECG: no signal on GPIO%d (level %d, swing %d) - check AD8232 OUTPUT -> 5\n",
                  ECG_OUT_PIN, (int)ecgRawMid, (int)ecgRawSwing);
  } else {
    Serial.printf("ECG: level %d, swing %d, beats %lu, %s", (int)ecgRawMid, (int)ecgRawSwing,
                  (unsigned long)ecgBeats, ecgHave ? "" : "reading...\n");
    if (ecgHave) Serial.printf("BPM %d\n", (int)ecgBpm);
  }
}

/* ============================================================== setup ===*/
void setup() {
  Serial.begin(115200);
  delay(1200);                 // gives the Serial Monitor time to attach after an upload

  MOTOR_INIT();
  MOTOR_SET(0);
  neopixelWrite(RGB_LED_PIN, 0, 0, 0);          // keep the on-board RGB LED dark

#if ENABLE_OLED
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!oledOk) Serial.println(F("OLED not found at 0x3C - try 0x3D, check SDA->8 and SCL->9 "
                                "(everything else still works)"));
#endif

  pinMode(ECG_LOP_PIN, INPUT_PULLUP);           // pulled up: a missing AD8232 reads "leads off"
  pinMode(ECG_LOM_PIN, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetPinAttenuation(ECG_OUT_PIN, ADC_11db);   // 0 - 3.1 V

  Serial.println(F("\nSereniSuit Demo 1 booting... (ECG: AD8232)"));
  Serial.println(F("AD8232 wiring: 3.3V->3V3  GND->G  OUTPUT->5  LO+->6  LO-->7  SDN->3V3"));
#if ENABLE_OLED
  Serial.println(F("OLED wiring:   VCC->3V3  GND->G  SDA->8  SCL->9  (SSD1306 @ 0x3C)"));
#endif

  stateLock = xSemaphoreCreateMutex();
  memset(paired, 0, sizeof(paired));

  startWiFi();
  setupWebServer();
#if ENABLE_BLUETOOTH
  startBluetooth();
#endif
  Serial.printf("Wi-Fi + Bluetooth up at %lu ms\n", millis());

  /* prove the model runs on-device before any sensor is involved */
  runSelfTest();

#if ENABLE_OLED
  if (oledOk) oledBootScreens();               // ML result, then how to connect
#endif

  qrsReset();
  xTaskCreatePinnedToCore(ecgTask, "ecg", 4096, nullptr, 3, nullptr, 1);

#if ENABLE_BLUETOOTH
  bleSetInfo();
#endif
  Serial.println(F("ready. commands: m = self-test, t = force danger, p = ECG plot, i = wifi info, h = help"));
}

/* =============================================================== loop ===*/
void loop() {
  static SuitStatus    prevStatus    = ST_BOOTING;
  static uint32_t      checkSeq      = 0;
  static uint32_t      lastDemoSeq   = 0;
  static unsigned long lastHousekeep = 0;
  static unsigned long lastBroadcast = 0;
  static unsigned long lastSummary   = 0;
  static uint8_t       plotTail      = 0;
#if ENABLE_OLED
  static unsigned long lastOled      = 0;
#endif

  /* ---- serial (only when a laptop happens to be connected) ---- */
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'm') runSelfTest();
    else if (c == 't') {
      forceDanger = !forceDanger;
      Serial.printf("forced danger: %s\n", forceDanger ? "ON" : "off");
    } else if (c == 'p') {
      plotEcg = !plotEcg;
      plotTail = plotHead;
      if (!plotEcg) Serial.println(F("ECG plot stream off"));
    } else if (c == 'i') {
      printWifiInfo();
    } else if (c == 'h') {
      Serial.println(F("m = ML self-test | t = toggle forced danger | p = ECG plot stream | i = wifi info | h = help"));
    }
  }

  /* ---- housekeeping: drop dead sockets and silent devices ---- */
  if (millis() - lastHousekeep > 1000) {
    ws.cleanupClients();
    lockState();
    bool expired = pairExpire();
    unlockState();
    if (expired) pairingChanged = true;
    lastHousekeep = millis();
  }

  if (resetAvgRequested) {
    resetAvgRequested = false;
    ecgResetReq = true;                         // the ECG task starts over
  }

  /* ---- where the BPM comes from: fake input, the ECG, or nothing ---- */
  bool demo     = demoActive;
  bool leadsOff = ecgLeadsOff;
  bool noSignal = ecgSignalBad;
  bool have     = ecgHave;
  int  bpm      = 0;

  if (demo)                              bpm = demoBpm;
  else if (sensorsEnabled && !leadsOff && !noSignal && have) bpm = ecgBpm;

  bool outOfRange = bpm > 0 && (bpm < BPM_NORMAL_LOW || bpm > BPM_NORMAL_HIGH);

  /* ---- one status for every screen ---- */
  SuitStatus st;
  if (forceDanger)                 st = ST_DANGER;
  else if (demo)                   st = outOfRange ? ST_DANGER : ST_NORMAL;
  else if (!sensorsEnabled)        st = ST_SENSORS_OFF;
  else if (leadsOff)               st = ST_LEADS_OFF;
  else if (noSignal)               st = ST_SENSOR_ERROR;
  else if (!have)                  st = ST_READING;
  else                             st = outOfRange ? ST_DANGER : ST_NORMAL;

  /* ---- a "BPM check" completes when a verdict appears or changes, or a
          new fake value arrives -> every paired device alerts once ---- */
  bool verdict = (st == ST_NORMAL || st == ST_DANGER);
  uint32_t ds  = demoSeq;
  if ((verdict && st != prevStatus) || (demo && ds != lastDemoSeq)) {
    checkSeq++;
  }
  lastDemoSeq = ds;
  prevStatus  = st;

  /* ---- beat shape: score the latest 10 s of ECG (research preview) ---- */
  if (!sensorsEnabled || leadsOff || noSignal) { shapePct = -1; shapeBeats = 0; }
  if (shapeReady) {
    int typical = 0;
    unsigned long t0 = micros();
    int n = shapeScoreWindow(shapeWin, SHAPE_WIN, &typical);
    unsigned long dt = micros() - t0;
    shapeReady = false;
    if (n > 0) {
      shapePct   = (100 * typical + n / 2) / n;
      shapeBeats = n;
      Serial.printf("beat shape (research preview): %d of %d beats typical (%d%%), %lu us\n",
                    typical, n, shapePct, dt);
    }
  }

  /* ---- actuator: non-blocking buzz so nothing else ever stalls ---- */
  if (st == ST_DANGER) {
    if (millis() - motorToggle > MOTOR_PULSE_MS) {
      motorOn = !motorOn;
      MOTOR_SET(motorOn ? MOTOR_DUTY : 0);
      motorToggle = millis();
    }
  } else {
    MOTOR_SET(0);
    motorOn = false;
  }

  lockState();
  int nPaired = pairCount();
  unlockState();

#if ENABLE_OLED
  if (oledOk && millis() - lastOled > 250) { oledRender(st, bpm, nPaired); lastOled = millis(); }
#endif

  /* ---- Serial Monitor ---- */
  if (plotEcg) {
    while (plotTail != plotHead) Serial.println((int)plotRing[plotTail++ & 63]);
  } else {
    printStatusChange(st, bpm, nPaired);
    if (millis() - lastSummary > 5000) { printEcgSummary(); lastSummary = millis(); }
  }

  /* ---- website + app: the moment anything changes, and every 500 ms
          regardless, so every screen stays in lockstep ---- */
  static uint32_t   lastSentSeq     = 0;
  static SuitStatus lastSentStatus  = ST_BOOTING;
  static int        lastSentBpm     = -1;
  static int        lastSentShape   = -2;
  static bool       lastSentSensors = true, lastSentDemo = false;
  bool sensorsNow = sensorsEnabled;
  if (pairingChanged || checkSeq != lastSentSeq || st != lastSentStatus || bpm != lastSentBpm ||
      sensorsNow != lastSentSensors || demo != lastSentDemo || shapePct != lastSentShape ||
      millis() - lastBroadcast > STATE_BROADCAST_MS) {
    pairingChanged = false;
    broadcastState(st, bpm, checkSeq);
    lastSentSeq     = checkSeq;
    lastSentStatus  = st;
    lastSentBpm     = bpm;
    lastSentShape   = shapePct;
    lastSentSensors = sensorsNow;
    lastSentDemo    = demo;
    lastBroadcast   = millis();
  }

  delay(20);
}
