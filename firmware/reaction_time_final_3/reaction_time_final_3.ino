// ============================================================
//  BioE101 FINAL PROJECT — EMG Reaction Time Measurement
//  Memory-optimised for Arduino Uno (2 KB SRAM)
// ============================================================
//
//  COMMANDS (Serial Monitor, 115200 baud):
//    'c' → Calibrate (stay relaxed ~3 sec)
//    's' → Start trial
//    'p' → Print all results
//    'r' → Reset
//
//  HOW TO USE IN WOKWI SIMULATOR:
//    1. Send 'c' — leave potentiometer centered, wait 3 sec.
//       std = 0.00 and threshold = 0.00 is NORMAL in simulation
//       (real hardware will have noise and a non-zero threshold).
//    2. Send 's' — wait for random delay, then watch which LED lights up.
//    3. Turn the potentiometer knob slightly away from center
//       → this simulates your EMG signal crossing the threshold.
//       Serial Monitor will print "EMG detected — TIMER ON!"
//    4. Click the matching button (same number as the LED).
//       Reaction time prints in ms.
//
//  PIN MAP:
//    A0          → Conditioned EMG signal (0–5 V, centered ~2.5 V)
//    D2,D3,D4,D5 → LEDs  (pin → 330Ω → LED anode; cathode → GND)
//    D6,D7,D8,D9 → Buttons (pin → button → GND; INPUT_PULLUP used)
// ============================================================

// ── Tunable parameters ──────────────────────────────────────
float THRESHOLD_MULTIPLIER = 4.0;
int   MIN_RAND_DELAY_MS    = 1500;
int   MAX_RAND_DELAY_MS    = 5000;
int   TIMEOUT_MS           = 8000;
int   DEBOUNCE_MS          = 30;

// ── Memory budget ────────────────────────────────────────────
#define MAX_TRIALS    10
#define RMS_WINDOW    32
#define LOG_SIZE      48

// ── Hardware constants ───────────────────────────────────────
const float VCC         = 5.0;
const int   ADC_MAX     = 1023;
const int   LED_PINS[4] = {2, 3, 4, 5};
const int   BTN_PINS[4] = {6, 7, 8, 9};
const int   SENSOR_PIN  = A0;

// ── Calibration ─────────────────────────────────────────────
bool  calibrated         = false;
float baseline_mean_V    = 2.5;
float baseline_std_V     = 0.05;
float detection_thresh_V = 0.0;

// ── RMS ring buffer ──────────────────────────────────────────
float   rmsRing[RMS_WINDOW];
uint8_t rmsHead = 0;
uint8_t rmsCnt  = 0;

// ── Sensor log ───────────────────────────────────────────────
float   sensorLog[LOG_SIZE];
uint8_t logIdx = 0;

// ── Results storage ──────────────────────────────────────────
unsigned long trialResults[MAX_TRIALS];
bool          trialValid[MAX_TRIALS];
uint8_t       trialCount = 0;

// ── State machine ────────────────────────────────────────────
enum State : uint8_t { IDLE, WAITING_SENSOR, TIMING, SHOW_RESULT };
State currentState = IDLE;

// ── Trial state ──────────────────────────────────────────────
int8_t        activeLED       = -1;
int8_t        activeButton    = -1;
unsigned long ledOnTime       = 0;
unsigned long sensorTime      = 0;
unsigned long reactionTime_ms = 0;
bool          sensorFired     = false;

// ── Debounce — needs TWO state arrays to work correctly ──────
//   lastBtn    = raw reading updated every loop tick
//   stableBtn  = confirmed debounced state (only changes after DEBOUNCE_MS)
//   The press event fires on the falling edge of stableBtn (HIGH→LOW).
unsigned long lastDebounce[4] = {0, 0, 0, 0};
bool          lastBtn[4]      = {HIGH, HIGH, HIGH, HIGH};  // raw
bool          stableBtn[4]    = {HIGH, HIGH, HIGH, HIGH};  // debounced

// ============================================================
void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(A5));

  for (uint8_t i = 0; i < 4; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW);
    pinMode(BTN_PINS[i], INPUT_PULLUP);
  }
  pinMode(SENSOR_PIN, INPUT);

  Serial.println(F("╔══════════════════════════════════════╗"));
  Serial.println(F("║  BioE101 EMG Reaction Time System    ║"));
  Serial.println(F("╠══════════════════════════════════════╣"));
  Serial.println(F("║  c=Calibrate s=Start p=Print r=Reset ║"));
  Serial.println(F("╚══════════════════════════════════════╝"));
  Serial.println(F("Begin with 'c' (muscles relaxed / pot centered)."));
}

// ============================================================
void loop() {
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if      (cmd == 'c') runCalibration();
    else if (cmd == 's') beginTrial();
    else if (cmd == 'p') printResults();
    else if (cmd == 'r') resetAll();
  }

  switch (currentState) {

    case IDLE: break;

    // ── Watching A0 for EMG onset ──────────────────────────
    case WAITING_SENSOR: {
      float v    = readVoltage();
      float rmsV = updateRMS(v);
      logSample(v);

      // In simulation: threshold = 0.00, so turning the pot
      // any amount away from center triggers this immediately.
      // On real hardware: threshold = baseline_std * multiplier.
      if (!sensorFired && rmsV >= detection_thresh_V) {
        sensorFired = true;
        sensorTime  = millis();
        Serial.print(F(">> EMG detected — RMS: "));
        Serial.print(rmsV, 3);
        Serial.print(F(" V  threshold: "));
        Serial.print(detection_thresh_V, 3);
        Serial.println(F(" V — TIMER ON! Press the matching button."));
        currentState = TIMING;
      }

      if (millis() - ledOnTime > (unsigned long)TIMEOUT_MS) {
        Serial.println(F("!! Timeout — no EMG signal detected."));
        Serial.println(F("   In Wokwi: turn the potentiometer knob."));
        Serial.println(F("   On hardware: check electrodes & signal chain."));
        endTrial(false);
      }
      break;
    }

    // ── Waiting for correct button ─────────────────────────
    case TIMING: {
      if (debouncedPress(activeButton)) {
        reactionTime_ms = millis() - sensorTime;
        endTrial(true);
        break;
      }
      for (uint8_t i = 0; i < 4; i++) {
        if (i != (uint8_t)activeButton && debouncedPress(i)) {
          Serial.print(F("!! Wrong button pressed. Correct = button "));
          Serial.println(activeButton + 1);
        }
      }
      if (millis() - sensorTime > (unsigned long)TIMEOUT_MS) {
        Serial.println(F("!! Timeout — no button press detected."));
        endTrial(false);
      }
      break;
    }

    case SHOW_RESULT:
      currentState = IDLE;
      Serial.println(F("s=Next trial  p=Results  c=Recalibrate\n"));
      break;
  }
}

// ============================================================
//  CALIBRATION — Welford online algorithm (no sample array)
// ============================================================
void runCalibration() {
  Serial.println(F("\n─── CALIBRATION — relax / keep pot centered ───"));

  const int N = 300;
  float mean = 0.0, M2 = 0.0;

  for (int i = 1; i <= N; i++) {
    float v     = readVoltage();
    float delta = v - mean;
    mean += delta / i;
    M2   += delta * (v - mean);
    delay(10);
  }

  baseline_mean_V    = mean;
  baseline_std_V     = (N > 1) ? sqrt(M2 / (N - 1)) : 0.0;
  detection_thresh_V = baseline_std_V * THRESHOLD_MULTIPLIER;
  calibrated         = true;

  Serial.print(F("  Baseline mean : ")); Serial.print(baseline_mean_V, 4); Serial.println(F(" V"));
  Serial.print(F("  Baseline std  : ")); Serial.print(baseline_std_V,  4); Serial.println(F(" V"));
  Serial.print(F("  Threshold     : ")); Serial.print(detection_thresh_V, 4); Serial.println(F(" V"));

  if (baseline_std_V < 0.001)
    Serial.println(F("  NOTE: std=0 is normal in Wokwi (pot has no noise)."));
  else if (baseline_std_V > 0.3)
    Serial.println(F("  ⚠ std > 0.3V — noisy signal, check hardware."));
  else
    Serial.println(F("  Calibration OK. Send 's' to begin a trial."));
}

// ============================================================
//  BEGIN TRIAL
// ============================================================
void beginTrial() {
  if (!calibrated)              { Serial.println(F("!! Calibrate first — send 'c'.")); return; }
  if (trialCount >= MAX_TRIALS) { Serial.println(F("!! Max trials. Send 'r' to reset.")); return; }

  sensorFired = false;
  logIdx      = 0;
  rmsHead     = 0;
  rmsCnt      = 0;
  for (uint8_t i = 0; i < RMS_WINDOW; i++) rmsRing[i] = 0.0;

  // Reset debounce state so old button presses don't carry over
  for (uint8_t i = 0; i < 4; i++) {
    lastBtn[i]    = HIGH;
    stableBtn[i]  = HIGH;
    lastDebounce[i] = 0;
  }

  activeLED    = random(0, 4);
  activeButton = activeLED;   // LED n ↔ Button n (same index)

  int waitMs = random(MIN_RAND_DELAY_MS, MAX_RAND_DELAY_MS);
  Serial.print(F("\nTrial ")); Serial.print(trialCount + 1);
  Serial.print(F(" | LED ")); Serial.print(activeLED + 1);
  Serial.print(F(" | Delay: ")); Serial.print(waitMs); Serial.println(F(" ms — GET READY..."));

  delay(waitMs);

  digitalWrite(LED_PINS[activeLED], HIGH);
  ledOnTime = millis();

  Serial.print(F("** STIMULUS: LED ")); Serial.print(activeLED + 1);
  Serial.println(F(" ON"));
  Serial.println(F("   → In Wokwi: turn potentiometer knob, then click matching button."));
  currentState = WAITING_SENSOR;
}

// ============================================================
//  END TRIAL
// ============================================================
void endTrial(bool success) {
  digitalWrite(LED_PINS[activeLED], LOW);

  trialValid[trialCount]   = success;
  trialResults[trialCount] = success ? reactionTime_ms : 0;
  trialCount++;

  if (success) {
    Serial.print(F("✓ REACTION TIME: "));
    Serial.print(reactionTime_ms);
    Serial.println(F(" ms"));

    if      (reactionTime_ms < 100)  Serial.println(F("  [< 100ms — possible false trigger]"));
    else if (reactionTime_ms < 200)  Serial.println(F("  [100–200ms — excellent]"));
    else if (reactionTime_ms < 350)  Serial.println(F("  [200–350ms — normal]"));
    else if (reactionTime_ms < 500)  Serial.println(F("  [350–500ms — slow]"));
    else                             Serial.println(F("  [> 500ms — very slow]"));

    Serial.print(F("SENSOR_LOG:"));
    for (uint8_t i = 0; i < logIdx; i++) {
      Serial.print(sensorLog[i], 3);
      if (i < logIdx - 1) Serial.print(',');
    }
    Serial.println();
  } else {
    Serial.println(F("  Trial = INVALID."));
  }
  currentState = SHOW_RESULT;
}

// ============================================================
//  PRINT ALL RESULTS
// ============================================================
void printResults() {
  Serial.println(F("\n═══════ RESULTS ═══════"));
  if (trialCount == 0) { Serial.println(F("No trials yet.")); return; }

  unsigned long sum = 0;
  unsigned long minRT = 0xFFFFFFFF, maxRT = 0;
  uint8_t valid = 0;

  for (uint8_t i = 0; i < trialCount; i++) {
    Serial.print(F("Trial ")); Serial.print(i + 1); Serial.print(F(": "));
    if (trialValid[i]) {
      Serial.print(trialResults[i]); Serial.println(F(" ms"));
      sum += trialResults[i];
      if (trialResults[i] < minRT) minRT = trialResults[i];
      if (trialResults[i] > maxRT) maxRT = trialResults[i];
      valid++;
    } else {
      Serial.println(F("INVALID"));
    }
  }

  if (valid > 0) {
    float mean = (float)sum / valid;
    float var  = 0.0;
    for (uint8_t i = 0; i < trialCount; i++) {
      if (trialValid[i]) { float d = trialResults[i] - mean; var += d * d; }
    }
    Serial.println(F("───────────────────────"));
    Serial.print(F("Valid : ")); Serial.println(valid);
    Serial.print(F("Mean  : ")); Serial.print(mean, 1);           Serial.println(F(" ms"));
    Serial.print(F("Min   : ")); Serial.print(minRT);             Serial.println(F(" ms"));
    Serial.print(F("Max   : ")); Serial.print(maxRT);             Serial.println(F(" ms"));
    Serial.print(F("StdDev: ")); Serial.print(sqrt(var/valid),1); Serial.println(F(" ms"));
  }
  Serial.println(F("═══════════════════════\n"));
}

// ============================================================
//  RESET
// ============================================================
void resetAll() {
  trialCount   = 0;
  calibrated   = false;
  currentState = IDLE;
  for (uint8_t i = 0; i < MAX_TRIALS; i++) { trialResults[i] = 0; trialValid[i] = false; }
  Serial.println(F("Reset done. Send 'c' to recalibrate."));
}

// ============================================================
//  READ VOLTAGE FROM A0
// ============================================================
float readVoltage() {
  return analogRead(SENSOR_PIN) * (VCC / ADC_MAX);
}

// ============================================================
//  ROLLING RMS ENVELOPE (deviation from baseline mean)
// ============================================================
float updateRMS(float v) {
  float dev = v - baseline_mean_V;
  rmsRing[rmsHead] = dev * dev;
  rmsHead = (rmsHead + 1) % RMS_WINDOW;
  if (rmsCnt < RMS_WINDOW) rmsCnt++;
  float s = 0.0;
  for (uint8_t i = 0; i < rmsCnt; i++) s += rmsRing[i];
  return sqrt(s / rmsCnt);
}

// ============================================================
//  LOG SAMPLE
// ============================================================
void logSample(float v) {
  if (logIdx < LOG_SIZE) sensorLog[logIdx++] = v;
}

// ============================================================
//  DEBOUNCED BUTTON PRESS — fixed two-array implementation
//
//  How it works:
//    lastBtn[]   tracks the raw pin reading every loop tick.
//    stableBtn[] only updates after DEBOUNCE_MS of stable signal.
//    A press event fires exactly once when stableBtn transitions HIGH→LOW.
//
//  Bug in previous version: lastBtn was updated before checking
//  the stable state, so the edge condition was never satisfied.
// ============================================================
bool debouncedPress(int8_t idx) {
  bool reading = digitalRead(BTN_PINS[idx]);

  // If raw reading changed, restart the debounce timer
  if (reading != lastBtn[idx]) {
    lastDebounce[idx] = millis();
    lastBtn[idx]      = reading;
  }

  // Only update stable state after signal has been steady for DEBOUNCE_MS
  if ((millis() - lastDebounce[idx]) > (unsigned long)DEBOUNCE_MS) {
    if (reading != stableBtn[idx]) {
      stableBtn[idx] = reading;
      // Fire only on the falling edge (HIGH → LOW = button pressed)
      if (reading == LOW) return true;
    }
  }
  return false;
}
