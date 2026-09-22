# SereniSuit — Demo 1

Wearable stress-sensing chest suit. ESP32-S3 · AD8232 ECG · OLED · haptic feedback · Wi-Fi + Bluetooth dashboard.

> **Sensor change: the MAX30102 is replaced by the AD8232 ECG.** The MAX30102
> never gave a reliable reading on this build, so Demo 1 now measures the heart
> with 3 chest electrodes. Wiring: `diagrams/SereniSuit_Demo1_Circuit.drawio`,
> page 0 (also `diagrams/AD8232_wiring_page0.png`), and hole by hole on page 0b
> (`AD8232_breadboard_page0b.png`). The old MAX30102 firmware and PPG model are
> kept in `ml/old_ppg/`.
>
> **The OLED is back.** The SSD1306 0.96" screen keeps GPIO8/GPIO9 — the pins the
> MAX30102 used to share — and it is now the only device on that I²C bus, so the
> pull-up resistors are no longer needed. It shows the same status as the website
> and the app. Set `ENABLE_OLED 0` at the top of `src/main.cpp` to run without it.
> The board's RGB status light stays off.

```
demo1/
  diagrams/   SereniSuit_Demo1_Circuit.drawio   page 0  = AD8232 + OLED wiring,
                                                  electrode placement, safety
                                                page 0b = breadboard, hole by hole
                                                pages 1-5 = old MAX30102 version
              AD8232_wiring_page0.png           page 0 as a picture
              AD8232_breadboard_page0b.png      page 0b as a picture
  firmware/   platformio.ini                    ESP32-S3-DevKitC-1 (N8R8)
              src/main.cpp                      AD8232 ECG -> BPM, OLED, ML
                                                 self-test, Wi-Fi dashboard,
                                                 Bluetooth LE, pairing
              src/ecg_model.c                   ECG beat model in plain C - 110 KB
              include/ecg_model.h, ecg_test_beats.h
              include/web_dashboard.h           the Wi-Fi website / phone app page,
                                                 manifest, service worker, icon
  bluetooth-app/  index.html + manifest,        the Bluetooth dashboard (same screen,
                  sw.js, icons                   talks Bluetooth LE instead of Wi-Fi)
  ml/         ecg_ptbdb_beats.py                training, evaluation, export to C
              dataset/ptbdb/                    PTB heartbeats, 14,552 beats
              ecg_model_metrics.json, confusion_matrix_ecg.png
              old_ppg/                          retired MAX30102 firmware + PPG model
  report/     SereniSuit_Report_Demo1.pdf
```

## What runs live, and what does not

| | Runs live in Demo 1 | Status |
|---|---|---|
| **AD8232 ECG → heart rate** — R-peaks, median of 8 beat intervals | **yes** | the sensor |
| **OLED** — the same status on the suit itself, no laptop needed | **yes** | SSD1306, I²C 0x3C |
| **BPM threshold** — outside 60–100 → DANGER + vibration | **yes** | this is the demo |
| **Wi-Fi dashboard** — laptop website + phone app, paired together, show the live reading | **yes** | see below |
| **Bluetooth dashboard** — same screen over Bluetooth LE, no Wi-Fi to join | **yes** | see below |
| **ECG beat-shape classifier** (PTB) — normal vs abnormal beat shape | no | self-tests on-device at boot; does not transfer to other ECG sources yet |

The live alert is a threshold on heart **rate**. The firmware samples the ECG
250 times a second, finds each heartbeat's R-peak with a small Pan–Tompkins-style
detector, takes the median of the last 8 beat-to-beat intervals and turns it into
BPM. Exact arithmetic, no ML.

The ML model classifies the **shape** of single heartbeats (normal vs abnormal),
trained on the PTB Diagnostic ECG heartbeat set. It is demonstrated where the
claim is exact and cannot fail: at boot it scores 8 held-out beats stored in
flash, checks that its answers match Python, and both dashboards show the result.
It does not score the live AD8232 signal — see "Results" for why.

## AD8232 ECG — wiring and electrodes

| AD8232 pin | ESP32-S3 pin | Carries |
|---|---|---|
| 3.3V | 3V3 (+ rail) | power — 3.3 V only, never 5 V |
| GND | G (− rail) | common ground with the ESP32 and DRV8833 |
| OUTPUT | GPIO5 | analog ECG (ADC1, works with Wi-Fi on) |
| LO+ | GPIO6 | HIGH = an electrode is off |
| LO− | GPIO7 | HIGH = an electrode is off |
| SDN | 3V3 (+ rail) | keeps the chip switched on |

And the OLED, on the pins the MAX30102 used to share:

| OLED pin | ESP32-S3 pin | Carries |
|---|---|---|
| GND | G (− rail) | ground |
| VCC | 3V3 (+ rail) | power — the module is 3.3–5 V tolerant |
| SCL | GPIO9 | I²C clock |
| SDA | GPIO8 | I²C data |

At boot the screen shows the ML self-test result, then the Wi-Fi name, password
and address and the Bluetooth name — so the demo needs no laptop. After that:
the live BPM, the status (ATTACH ELECTRODES · READING… · NORMAL · DANGER!) and
how many devices are paired. It answers at 0x3C; if it stays blank, try 0x3D
(`OLED_ADDR` in `src/main.cpp`).

- Take out the MAX30102, its 4 wires and the 10 kΩ pull-up resistors. Neither the
  AD8232 nor the OLED needs them: the AD8232's outputs are driven, the firmware
  turns on the ESP32's own pull-ups on GPIO6/7, and the OLED brings its own I²C
  pull-ups. The OLED and the motor part (GPIO4 → DRV8833) stay as they were.
- **Electrodes:** RA below the wearer's right collarbone, LA below the left
  collarbone, RL on the wearer's lower right ribs. Go by the letters on the lead
  clips — cable colours differ between kits. Clean, dry skin, fresh gel pads,
  sit still while it reads.
- Keep the vibration motor and its wires away from the AD8232 and the electrode
  cable: motor noise shows up in the ECG.
- **Safety:** electrodes on a person only while the suit runs on the power bank,
  or the laptop runs on battery with its charger unplugged. A heart-rate demo,
  not a medical device.

## Wi-Fi dashboard — website + mobile app, paired

The ESP32-S3 has Wi-Fi built in, so no new hardware was needed. The suit makes
its own Wi-Fi network and serves one page from flash. On the laptop it is the
**website**; on the phone, "Add to Home Screen" turns it into the **mobile app**.
Both can be open at the same time.

- **Pairing.** Every open website/app says hello to the suit and repeats it every
  5 s. Each screen shows **"Paired with SereniSuit ✓"** and the list of paired
  devices (e.g. *Website (Mac)* and *Mobile app (Android)*); a pop-up says when
  another device pairs or leaves. Both screens read
  **"Paired · 2 devices"**. A device that goes quiet for 15 s (screen locked,
  walked away) drops off the list and pairs again when it comes back.
- **Same reading everywhere.** One status value drives the website and the app.
  Every screen updates the moment the status changes, and at
  least every 500 ms.
- **Remote sensor ON/OFF.** Flip it on either device; the other device's switch
  follows.
- **Alert after every BPM check.** On every paired device: a pop-up, plus sound
  and vibration once you tap **Enable** under "Alerts on this device". A check
  counts when a reading first appears, when it flips normal ↔ DANGER, or when a
  new fake value is sent (a steady reading does not alert every beat).
- **Demo mode — fake BPM input.** Tap **72 / 45 / 140** or type any BPM. It drives
  the exact path a real ECG reading would — motor and both screens:
  - **Normal (60–100)** → BPM shown only.
  - **Abnormal** → BPM + DANGER banner + vibration motor.
  "Back to live sensor" returns to the live ECG.

### Demo day — power bank only, no laptop cable

1. **Before the review, flash once** from the laptop (`pio run -t upload`), then
   unplug the laptop.
2. **Power the suit from a power bank** (≥ 1 A output) into the DevKit's USB-C.
   It starts on its own network — `SereniSuit-Demo`, password `seresuit123`,
   address `http://192.168.4.1`.
3. **Laptop:** join the `SereniSuit-Demo` Wi-Fi and open `http://192.168.4.1`.
4. **Phone:** join the same Wi-Fi and open `http://192.168.4.1` (or the icon you
   added with Add to Home Screen). Both screens show "Paired ✓ — 2 devices",
   on both screens.
5. Tap **Enable** under "Alerts on this device" on each screen, then use the demo
   buttons or the real ECG (pads on, sitting still — power bank only).

**Or over Bluetooth** (Android phone / Chrome or Edge on the laptop): skip the
Wi-Fi join, open the SereniSuit BT app and tap **Connect via Bluetooth** → pick
**SereniSuit**. You can mix: laptop on Wi-Fi, phone on Bluetooth — see
"Bluetooth dashboard" below.

If a screen says it is not paired, open `http://192.168.4.1` again. `http://serenisuit.local` also works on the laptop and iPhone.

**Things to know**

- The laptop has no internet while it is on `SereniSuit-Demo`. That is expected.
  To keep internet, set `USE_ROUTER_WIFI 1` and put your **phone hotspot** in
  `WIFI_SSID`/`WIFI_PASSWORD`. The laptop then joins the hotspot too; the
  address to open is printed on the Serial Monitor (115200) at start-up. If the hotspot is off, the suit falls back to
  its own network after 8 s.
- **Android** may say "Wi-Fi has no internet". Tap **Keep Wi-Fi connection** (or
  turn mobile data off for the demo), or the page may not load.
- **Phone alerts work while the app is open** (pop-up + sound + vibration;
  iPhone doesn't support web vibration). System pop-up notifications from the
  lock screen need an `https` page, which a device on its own Wi-Fi can't
  provide. The page uses them automatically wherever the browser allows.
- **Power bank.** Estimated draw with Wi-Fi on: ~180 mA idle, ~650 mA brief
  peaks (Wi-Fi transmit + motor start). That is easy for a 1–2 A power bank but
  tight for a 500 mA laptop port. Wi-Fi transmit power is set to 11 dBm to trim
  the peaks. Some power banks switch off under ~50–100 mA; this load should keep
  them on, but run yours for 10 minutes before the review. These are design
  estimates — check them with a USB inline meter.
- **No electrodes / no signal.** With no pads on, the screens show
  **ATTACH ELECTRODES**; if the AD8232 is unplugged or its OUTPUT wire is loose,
  **NO ECG SIGNAL**. The Serial Monitor names the wire to check every 5 s, and
  demo mode still works from either screen.
- After the pads go on, the screens show **READING…** until 4 real beats are in
  (about 5 s), then the BPM.

## Bluetooth dashboard — connect without joining Wi-Fi

The ESP32-S3 also has **Bluetooth LE** built in (no "classic" Bluetooth), so no
new parts again. The suit advertises as **`SereniSuit`** at the same time as it
runs its Wi-Fi, so you can mix them: e.g. laptop on Wi-Fi + phone on Bluetooth.
Every screen shows the same reading and one shared paired list, with each device
tagged **· Wi-Fi** or **· Bluetooth**.

The Bluetooth dashboard is `demo1/bluetooth-app/` — the same screen as the Wi-Fi
page (pairing, live BPM, sensors switch, alerts, 72 / 45 / 140 demo buttons), but
it talks to the suit with Web Bluetooth.

| Device | Bluetooth dashboard | Use instead |
|---|---|---|
| Laptop — Chrome or Edge (Mac / Windows / Linux) | ✅ | — |
| Android phone — Chrome | ✅ (installable as an app) | — |
| iPhone / iPad (any browser) | ❌ Apple has no Web Bluetooth | Wi-Fi dashboard, or the Bluefy app |
| Firefox / Safari on a laptop | ❌ | Chrome/Edge, or Wi-Fi dashboard |

**Why it needs a link, not the suit's address:** browsers only allow Bluetooth
on a secure page (`https://` or `http://localhost`). The suit's own page is plain
`http://192.168.4.1`, so the Bluetooth app is hosted separately:

1. **Put it online once (free, 5 minutes):** create a GitHub repository (e.g.
   `serenisuit-app`), upload the six files from `demo1/bluetooth-app/`, then
   Settings → Pages → Deploy from branch `main` / root. Your link is
   `https://<your-github-name>.github.io/serenisuit-app/`.
2. **Phone (Android):** open that link in Chrome **once with internet** → menu ⋮ →
   **Install app / Add to Home screen**. After that it opens offline (it keeps a
   copy), which is what you want at the venue.
3. **Laptop:** open the same link in Chrome/Edge. Because Bluetooth doesn't need
   the suit's Wi-Fi, the laptop keeps its normal internet. Offline fallback:
   `cd demo1/bluetooth-app && python3 -m http.server 8000`, then open
   `http://localhost:8000`.
4. Tap **Connect via Bluetooth** → pick **SereniSuit** in the list → the card
   turns green: "Paired with SereniSuit ✓". No PIN is needed.

**Things to know**

- On a Mac, the first time, allow Chrome to use Bluetooth (System Settings →
  Privacy & Security → Bluetooth). On Android, allow Nearby devices / Location if
  Chrome asks.
- Range is a few metres to about 10 m indoors. If the link drops, the page
  reconnects by itself; if it gives up, tap **Reconnect via Bluetooth**.
- Bluetooth pages get system notifications too (they are https), once you tap
  **Enable** under "Alerts on this device".
- Up to 3 Bluetooth connections at once, plus the Wi-Fi devices.
- Turn Bluetooth off with `ENABLE_BLUETOOTH 0` in `src/main.cpp` if you don't need it.

### Settings (top of `src/main.cpp`)

| Setting | Default | Meaning |
|---|---|---|
| `USE_ROUTER_WIFI` | `0` | `0` = suit's own network (recommended); `1` = join `WIFI_SSID` first |
| `AP_SSID` / `AP_PASSWORD` | `SereniSuit-Demo` / `seresuit123` | the suit's own network |
| `WIFI_TX_POWER` | `WIFI_POWER_11dBm` | lower = smaller current peaks, shorter range |
| `ENABLE_BLUETOOTH` | `1` | Bluetooth LE dashboard on/off |
| `BLE_NAME` | `SereniSuit` | name shown in the Bluetooth device list |
| `PAIR_TIMEOUT_MS` | `15000` | silence before a device is shown as unpaired |
| `ECG_OUT_PIN` / `ECG_LOP_PIN` / `ECG_LOM_PIN` | `5` / `6` / `7` | AD8232 OUTPUT / LO+ / LO− |
| `ENABLE_LIVE_BEAT_SHAPE` | `0` | `1` = also score live beats with the PTB model (not reliable yet, see Results) |
| `ENABLE_OLED` | `1` | `0` = run without the screen (everything else is the same) |
| `OLED_ADDR` | `0x3C` | try `0x3D` if the screen stays blank |

No filesystem upload step: the whole page (HTML/CSS/JS, manifest, service
worker) is compiled into the firmware from `include/web_dashboard.h`.

## Flash

```bash
cd demo1/firmware
pio run -t upload
pio device monitor          # 115200, only needed while the laptop is connected
```

Serial (laptop connected): `m` re-run ML self-test · `t` force DANGER · `p` live ECG
for the Serial Plotter · `i` Wi-Fi status/URL · `h` help

What the Serial Monitor should show: `SereniSuit Demo 1 booting... (ECG: AD8232)`,
the OLED wiring line (or `OLED not found at 0x3C …` if the screen is missing —
everything else still runs),
the ML self-test ending in `correct: 8/8 ... matches Python: yes`, then every 5 s
either `ECG: electrodes not attached`, `ECG: no signal on GPIO5 ...` or
`ECG: level ..., swing ..., beats ..., BPM 72`.

The first upload after adding Bluetooth also writes a new partition table
(`default_8MB.csv`, 3.2 MB per app slot) — Wi-Fi + Bluetooth together don't fit
the old 1.25 MB slot. Nothing stored on the board is lost that matters here.

## Retrain

```bash
cd demo1/ml
pip install scikit-learn pandas numpy matplotlib
python3 ecg_ptbdb_beats.py      # about a minute; rewrites firmware/src/ecg_model.c
                                # and include/ecg_model.h, ecg_test_beats.h
pip install neurokit2           # optional: also runs the transfer check below
```

Dataset: `dataset/ptbdb/ptbdb_normal.csv` and `ptbdb_abnormal.csv` — the PTB
heartbeats from the Kaggle "ECG Heartbeat Categorization Dataset" (beats cut from
PhysioNet's PTB Diagnostic ECG Database by Kachuee, Fazeli & Sarrafzadeh, 2018).
Each row is one heartbeat: 187 values at 125 Hz, scaled 0–1, starting at the
R-peak, plus the label.

## Results — ECG beat shape, 14,552 PTB heartbeats

Normal 4,046 beats (healthy controls) vs abnormal 10,506 (myocardial infarction
and other heart conditions). Held-out 20 % test split, 2,911 beats:

| Model | Accuracy | ROC-AUC | Macro-F1 | Size |
|---|---|---|---|---|
| Logistic regression (reference) | 0.8245 | 0.8661 | 0.7591 | 1 KB |
| Random forest, 200 trees (reference) | 0.9711 | 0.9939 | 0.9634 | too big for the MCU |
| **MLP 187 → 128 → 32 → 1 — deployed** | **0.9608** | **0.9852** | **0.9516** | **110 KB** |

5-fold cross-validation on the training part: 0.9567 ± 0.0045. Recall: normal
0.943, abnormal 0.968 (`ml/confusion_matrix_ecg.png`). On the ESP32 it runs as
plain C — 28,225 weights, no ML library; the boot self-test prints the time per
beat and checks all 8 answers against Python.

### Read these numbers with two limits

1. **The same patient is on both sides of the split.** The CSVs have no patient
   IDs, so beats from one person land in both train and test. The numbers are
   optimistic for a new person. This is why the ECG model line was retired
   earlier; it is back only as an on-device capability demo, not as a claim.
2. **It does not transfer to other ECG sources.** Run on healthy ECG that is not
   from PTB — cut into beats exactly the way the firmware would — it called only
   0–14 % of the normal beats "normal":

| Normal ECG, not from PTB | Beats | Called normal |
|---|---|---|
| real ECG (neurokit2 sample, 1000 Hz) | 35 | 14.3 % |
| real resting ECG, 5 min | 212 | 0.0 % |
| simulated ECG 60 / 75 / 90 BPM (ECGSYN) | 40 / 55 / 65 | 10.0 / 5.5 / 0.0 % |

The model has learned PTB's recording setup as much as heart physiology. So the
live AD8232 signal is **not** scored (`ENABLE_LIVE_BEAT_SHAPE 0`); the live
beat-cutting pipeline is in the firmware, switched off. Making it work needs
AD8232 recordings from our own suit to retrain on — Phase 2.

The earlier PPG arousal work (MAX30102, 22 participants, wearer-calibrated
accuracy 0.677) is kept with its script in `ml/old_ppg/`.

## Scope

Heart rate from a single-lead ECG (AD8232) — not blood pressure, and not a
diagnosis: one chest lead cannot replace a 12-lead clinical ECG. Research
prototype, not a medical device.

The Wi-Fi and Bluetooth dashboards are local only: devices on the suit's own
Wi-Fi (or the same hotspot) or within Bluetooth range. GitHub Pages only hosts
the page itself; readings go straight from the suit to the screen over Bluetooth
and are not stored or sent anywhere else.
