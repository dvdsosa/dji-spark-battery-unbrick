/*
 * spark_unbrick.ino — Recover DJI Spark batteries stuck in Permanent Fail (PF)
 * ---------------------------------------------------------------------------
 * Board: Arduino Uno / Nano (ATmega328P, 5 V, 16 MHz). Serial monitor at 115200.
 *
 * BMS chip: TI BQ40Z307 with DJI firmware (marked "BQ9003"), SMBus 0x0B.
 * Spark unseal key: 0xCCDF7EE0 (low word 0x7EE0 sent first, then high word
 * 0xCCDF). Confirmed by the community in o-gs/dji-firmware-tools #258.
 *
 * WIRING (battery connector, contacts facing you, left to right)
 *
 *     1     2     3     4     5     6
 *    SCL   GND   BAT+  BAT+  GND   SDA
 *
 *    Pin 1 (SCL) -> A5  + 4.7k pull-up to 5V
 *    Pin 6 (SDA) -> A4  + 4.7k pull-up to 5V
 *    Pin 2 (GND) -> Arduino GND
 *    Pin 3 (+) / Pin 5 (-) -> 9-12 V supply (100 ohm in series) ONLY to wake the BMS.
 *                             Never connect BAT+ or the supply to the Arduino!
 *
 *  NOTE: the circuitschools pinout (SDA=pin5, SCL=pin6) is for the Mavic Air,
 *  NOT the Spark.
 *
 * MENU
 *   S  scan bus                   I  info/status (read-only)
 *   T  bus stability test         W  measure SDA/SCL voltage (pull-ups)
 *   D  one CSV status line (read-only, used by the scripts)
 *   U  unseal                     F  full access (TI default key)
 *   P  clear PF data (0x0029)     R  chip reset (0x0041)
 *   L  seal (0x0030)              A  automatic recovery U→P→R→L
 *
 * Use at your own risk. A Li-ion cell that sat deeply discharged for a long
 * time may be damaged: supervise the first charge on a non-flammable
 * surface; do not recover a swollen pack.
 */

#include <Wire.h>

#define BATT_ADDR 0x0B

// Standard SBS commands
#define SBS_MAC        0x00   // ManufacturerAccess (subcommand writes)
#define SBS_TEMP       0x08
#define SBS_VOLTAGE    0x09
#define SBS_CURRENT    0x0A
#define SBS_RSOC       0x0D
#define SBS_FCC        0x10
#define SBS_STATUS     0x16
#define SBS_CYCLES     0x17
#define SBS_DESIGN_CAP 0x18
#define SBS_MFG_DATE   0x1B
#define SBS_SERIAL     0x1C
#define SBS_MFG_NAME   0x20
#define SBS_DEV_NAME   0x21
#define SBS_MAC_DATA   0x23   // ManufacturerData (subcommand responses)
#define SBS_CELL3      0x3D
#define SBS_CELL2      0x3E
#define SBS_CELL1      0x3F
#define SBS_SOH        0x4F

// ManufacturerAccess subcommands (bq40z50 / bq40z307 TRM)
#define MAC_DEVICE_TYPE   0x0001
#define MAC_FW_VERSION    0x0002
#define MAC_PF_DATA_RESET 0x0029
#define MAC_SEAL          0x0030
#define MAC_DEVICE_RESET  0x0041
#define MAC_SAFETY_STATUS 0x0051
#define MAC_PF_STATUS     0x0053
#define MAC_OP_STATUS     0x0054

// OperationStatus: relevant bits
#define OP_SEC_SHIFT 8        // SEC1:SEC0 -> 3 sealed, 2 unsealed, 1 full access
#define OP_PF        (1UL << 12)
#define OP_XDSG      (1UL << 13)
#define OP_XCHG      (1UL << 14)
#define OP_DSG       (1UL << 1)
#define OP_CHG       (1UL << 2)

struct KeyPair { uint16_t w0, w1; };
const KeyPair KEY_SPARK      = { 0x7EE0, 0xCCDF };  // 0xCCDF7EE0
const KeyPair KEY_TI_UNSEAL  = { 0x0414, 0x3672 };  // 0x36720414 (TI default)
const KeyPair KEY_TI_FULL    = { 0xFFFF, 0xFFFF };  // 0xFFFFFFFF (TI default)

#define TIMEOUT_FAST_US  50000UL    // normal reads
#define TIMEOUT_SLOW_US  3000000UL  // flash writes: the chip stretches the clock

// ───────────────────────── low-level SMBus ─────────────────────────

void busTimeout(uint32_t us) {
#ifdef WIRE_HAS_TIMEOUT
  Wire.setWireTimeout(us, true);
#else
  (void)us;
#endif
}

// Returns the Wire.endTransmission() code: 0 = ACK
uint8_t writeWord(uint8_t cmd, uint16_t val) {
  Wire.beginTransmission(BATT_ADDR);
  Wire.write(cmd);
  Wire.write(lowByte(val));
  Wire.write(highByte(val));
  return Wire.endTransmission();
}

int32_t readWordOnce(uint8_t cmd) {
  Wire.beginTransmission(BATT_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return -1;      // repeated start
  if (Wire.requestFrom((uint8_t)BATT_ADDR, (uint8_t)2) != 2) return -1;
  uint16_t lo = Wire.read();
  uint16_t hi = Wire.read();
  return (int32_t)((hi << 8) | lo);
}

// With retries: the Spark connector contacts are unreliable
int32_t readWord(uint8_t cmd) {
  for (uint8_t i = 0; i < 3; i++) {
    int32_t v = readWordOnce(cmd);
    if (v >= 0) return v;
    delay(10);
  }
  return -1;
}

// SMBus block read: [count][data...]. Returns byte count or -1.
int8_t readBlock(uint8_t cmd, uint8_t *buf, uint8_t maxLen) {
  Wire.beginTransmission(BATT_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return -1;
  uint8_t req = maxLen + 1;
  if (req > 32) req = 32;                                // AVR Wire buffer size
  if (Wire.requestFrom((uint8_t)BATT_ADDR, req) == 0) return -1;
  uint8_t len = Wire.read();
  if (len > req - 1) len = req - 1;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.available() ? Wire.read() : 0;
  while (Wire.available()) Wire.read();
  return len;
}

// Subcommand via ManufacturerAccess, response via ManufacturerData (0x23).
// Works on a sealed chip for the status commands.
int8_t macRead(uint16_t sub, uint8_t *buf, uint8_t maxLen) {
  for (uint8_t i = 0; i < 3; i++) {
    if (writeWord(SBS_MAC, sub) == 0) {
      delay(5);
      int8_t n = readBlock(SBS_MAC_DATA, buf, maxLen);
      if (n > 0) return n;
    }
    delay(10);
  }
  return -1;
}

bool macRead32(uint16_t sub, uint32_t &out) {
  uint8_t b[4] = {0};
  int8_t n = macRead(sub, b, 4);
  if (n < 2) return false;
  out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
        ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return true;
}

bool present() {
  Wire.beginTransmission(BATT_ADDR);
  return Wire.endTransmission() == 0;
}

// ───────────────────────── print helpers ─────────────────────────

void printHex(uint32_t v, uint8_t digits) {
  Serial.print(F("0x"));
  for (int8_t s = (digits - 1) * 4; s >= 0; s -= 4) Serial.print((v >> s) & 0xF, HEX);
}

void printString(uint8_t cmd) {
  uint8_t b[21];
  int8_t n = readBlock(cmd, b, 20);
  if (n <= 0) { Serial.println(F("(no reply)")); return; }
  for (int8_t i = 0; i < n; i++) if (b[i] >= 32 && b[i] < 127) Serial.write(b[i]);
  Serial.println();
}

const __FlashStringHelper *secName(uint8_t sec) {
  switch (sec) {
    case 1: return F("FULL ACCESS");
    case 2: return F("UNSEALED");
    case 3: return F("SEALED");
    default: return F("?");
  }
}

// ───────────────────────── status ─────────────────────────

// Returns the security level (1..3) or 0 if unreadable
uint8_t readSec(uint32_t *opOut = NULL) {
  uint32_t op;
  if (!macRead32(MAC_OP_STATUS, op)) return 0;
  if (opOut) *opOut = op;
  return (op >> OP_SEC_SHIFT) & 0x03;
}

// Lowest cell voltage in mV (0 if unreadable)
int32_t minCellMv() {
  const uint8_t regs[3] = { SBS_CELL1, SBS_CELL2, SBS_CELL3 };
  int32_t vmin = 0;
  for (uint8_t i = 0; i < 3; i++) {
    int32_t v = readWord(regs[i]);
    if (v > 0 && (vmin == 0 || v < vmin)) vmin = v;
  }
  return vmin;
}

void printInfo() {
  Serial.println(F("\n============ Battery status ============"));
  if (!present()) {
    Serial.println(F("No reply at 0x0B. Check wiring, pull-ups and the wake-up supply."));
    return;
  }

  Serial.print(F("Manufacturer   : ")); printString(SBS_MFG_NAME);
  Serial.print(F("Device name    : ")); printString(SBS_DEV_NAME);

  uint8_t b[4];
  if (macRead(MAC_DEVICE_TYPE, b, 2) >= 2) {
    Serial.print(F("Device type    : "));
    printHex(b[0] | (b[1] << 8), 4);
    Serial.println();
  }

  int32_t w;
  if ((w = readWord(SBS_SERIAL)) >= 0)  { Serial.print(F("SBS serial     : ")); Serial.println(w); }
  if ((w = readWord(SBS_MFG_DATE)) >= 0) {
    Serial.print(F("Manufactured   : "));
    Serial.print(1980 + ((w >> 9) & 0x7F)); Serial.print('-');
    Serial.print((w >> 5) & 0x0F);          Serial.print('-');
    Serial.println(w & 0x1F);
  }
  if ((w = readWord(SBS_CYCLES)) >= 0) { Serial.print(F("Cycle count    : ")); Serial.println(w); }

  int32_t dc = readWord(SBS_DESIGN_CAP), fcc = readWord(SBS_FCC);
  if (dc > 0 && fcc >= 0) {
    Serial.print(F("Capacity       : ")); Serial.print(fcc); Serial.print(F(" / "));
    Serial.print(dc); Serial.print(F(" mAh (")); Serial.print(fcc * 100 / dc); Serial.println(F("%)"));
  }
  if ((w = readWord(SBS_SOH)) >= 0)  { Serial.print(F("SOH            : ")); Serial.print(w); Serial.println('%'); }
  if ((w = readWord(SBS_RSOC)) >= 0) { Serial.print(F("Relative SOC   : ")); Serial.print(w); Serial.println('%'); }

  if ((w = readWord(SBS_VOLTAGE)) >= 0) { Serial.print(F("Pack voltage   : ")); Serial.print(w); Serial.println(F(" mV")); }
  const uint8_t regs[3] = { SBS_CELL1, SBS_CELL2, SBS_CELL3 };
  int32_t vmin = 0, vmax = 0;
  for (uint8_t i = 0; i < 3; i++) {
    int32_t v = readWord(regs[i]);
    Serial.print(F("Cell ")); Serial.print(i + 1); Serial.print(F("         : "));
    if (v < 0) { Serial.println(F("error")); continue; }
    Serial.print(v); Serial.println(F(" mV"));
    if (v > 0 && (vmin == 0 || v < vmin)) vmin = v;
    if (v > vmax) vmax = v;
  }
  if (vmin) {
    Serial.print(F("Cell spread    : ")); Serial.print(vmax - vmin); Serial.println(F(" mV"));
    if (vmin < 2000) Serial.println(F("  !! Cell < 2.0 V: possible internal damage. Supervise charging or discard it."));
  }

  if ((w = readWord(SBS_TEMP)) >= 0) {
    Serial.print(F("Temperature    : ")); Serial.print(w / 10.0 - 273.15, 1); Serial.println(F(" C"));
  }
  if ((w = readWord(SBS_CURRENT)) >= 0) { Serial.print(F("Current        : ")); Serial.print((int16_t)w); Serial.println(F(" mA")); }
  if ((w = readWord(SBS_STATUS)) >= 0)  { Serial.print(F("BatteryStatus  : ")); printHex(w, 4); Serial.println(); }

  uint32_t op;
  uint8_t sec = readSec(&op);
  if (sec) {
    Serial.print(F("OperationStatus: ")); printHex(op, 8); Serial.println();
    Serial.print(F("  Security     : ")); Serial.println(secName(sec));
    Serial.print(F("  PF active    : ")); Serial.println(op & OP_PF ? F("YES  <-- locked") : F("no"));
    Serial.print(F("  Charge       : ")); Serial.println(op & OP_XCHG ? F("disabled") : (op & OP_CHG ? F("FET ON") : F("FET OFF")));
    Serial.print(F("  Discharge    : ")); Serial.println(op & OP_XDSG ? F("disabled") : (op & OP_DSG ? F("FET ON") : F("FET OFF")));
  } else {
    Serial.println(F("OperationStatus: unreadable"));
  }

  uint32_t v32;
  if (macRead32(MAC_SAFETY_STATUS, v32)) {
    Serial.print(F("SafetyStatus   : ")); printHex(v32, 8);
    if (v32 & 0x01) Serial.print(F("  CUV (cell undervoltage)"));
    if (v32 & 0x02) Serial.print(F("  COV (cell overvoltage)"));
    Serial.println();
  }
  if (macRead32(MAC_PF_STATUS, v32)) {
    Serial.print(F("PFStatus       : ")); printHex(v32, 8);
    Serial.println(v32 ? F("  <-- permanent failure latched") : F("  OK"));
  }
  Serial.println(F("========================================"));
}

// ───────────────────────── write operations ─────────────────────────

// Sends both key halves back to back (the chip requires <4 s between them).
// The chip may NACK a word: not conclusive, the result is verified afterwards.
void sendKey(const KeyPair &k) {
  writeWord(SBS_MAC, k.w0);
  delay(2);
  writeWord(SBS_MAC, k.w1);
  delay(50);
}

bool tryKey(const KeyPair &k, uint8_t target, const __FlashStringHelper *label) {
  for (uint8_t attempt = 1; attempt <= 5; attempt++) {
    sendKey(k);
    uint8_t sec = readSec();
    Serial.print(F("  ")); Serial.print(label);
    Serial.print(F(" attempt ")); Serial.print(attempt);
    Serial.print(F(" -> ")); Serial.println(secName(sec));
    if (sec != 0 && sec <= target) return true;
    delay(300);
  }
  return false;
}

bool doUnseal() {
  Serial.println(F("\n[U] Unseal"));
  uint8_t sec = readSec();
  if (sec == 1 || sec == 2) { Serial.println(F("  Already unsealed.")); return true; }
  if (tryKey(KEY_SPARK, 2, F("Spark key 0xCCDF7EE0"))) return true;
  Serial.println(F("  Spark key failed; trying the TI default key."));
  if (tryKey(KEY_TI_UNSEAL, 2, F("TI key 0x36720414"))) return true;
  Serial.println(F("  [X] Unseal failed. Check contacts (use T) and the wake-up supply."));
  return false;
}

bool doFullAccess() {
  Serial.println(F("\n[F] Full access"));
  if (readSec() == 3 && !doUnseal()) return false;
  if (tryKey(KEY_TI_FULL, 1, F("TI full-access key 0xFFFFFFFF"))) return true;
  Serial.println(F("  Not available (not needed to clear the PF)."));
  return false;
}

bool pfActive(uint32_t *pfs = NULL) {
  uint32_t op = 0, st = 0;
  readSec(&op);
  macRead32(MAC_PF_STATUS, st);
  if (pfs) *pfs = st;
  return (op & OP_PF) || st;
}

bool doClearPF() {
  Serial.println(F("\n[P] PermanentFailDataReset (0x0029)"));
  uint8_t sec = readSec();
  if (sec != 1 && sec != 2) {
    Serial.println(F("  Chip is sealed: run U first."));
    return false;
  }
  busTimeout(TIMEOUT_SLOW_US);
  bool ok = false;
  // The community reports it sometimes needs several attempts
  for (uint8_t attempt = 1; attempt <= 5; attempt++) {
    uint8_t r = writeWord(SBS_MAC, MAC_PF_DATA_RESET);
    delay(1000);                                  // data flash write
    uint32_t pfs;
    bool still = pfActive(&pfs);
    Serial.print(F("  attempt ")); Serial.print(attempt);
    Serial.print(r == 0 ? F(" ACK") : (r == 5 ? F(" timeout") : F(" NACK")));
    Serial.print(F("  PFStatus=")); printHex(pfs, 8);
    Serial.println(still ? F("  (still set)") : F("  (cleared)"));
    if (!still) { ok = true; break; }
  }
  busTimeout(TIMEOUT_FAST_US);
  if (!ok) Serial.println(F("  The PF bit may not update until reset (R). Continuing."));
  return ok;
}

void doReset() {
  Serial.println(F("\n[R] Chip reset (0x0041)"));
  busTimeout(TIMEOUT_SLOW_US);
  writeWord(SBS_MAC, MAC_DEVICE_RESET);
  busTimeout(TIMEOUT_FAST_US);
  for (uint8_t i = 0; i < 30; i++) {              // up to ~3 s to reboot
    delay(100);
    if (present()) {
      Serial.println(F("  BMS is responding again."));
      delay(500);
      return;
    }
  }
  Serial.println(F("  BMS not responding after reset (wake-up supply disconnected?)."));
}

void doSeal() {
  Serial.println(F("\n[L] Seal (0x0030)"));
  uint8_t sec = readSec();
  if (sec == 3) { Serial.println(F("  Already sealed.")); return; }
  writeWord(SBS_MAC, MAC_SEAL);
  delay(300);
  Serial.print(F("  State: ")); Serial.println(secName(readSec()));
}

// ───────────────────────── tools ─────────────────────────

void doScan() {
  Serial.println(F("\n[S] Scanning I2C..."));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  ")); printHex(a, 2);
      Serial.println(a == BATT_ADDR ? F("  <- battery BMS") : F(""));
      found++;
    }
  }
  if (!found) Serial.println(F("  Nothing found. Wake-up supply applied? SDA/SCL swapped? Pull-ups?"));
}

void doBusTest() {
  Serial.println(F("\n[T] 300 voltage reads..."));
  uint16_t errors = 0;
  for (uint16_t i = 0; i < 300; i++) if (readWordOnce(SBS_VOLTAGE) < 0) errors++;
  Serial.print(F("  Errors: ")); Serial.println(errors);
  Serial.println(errors == 0 ? F("  Bus stable: safe to write.") :
                               F("  Bus unstable: secure the contacts before writing."));
}

// Measures idle SDA/SCL with the internal pull-ups off:
// ~5 V = external pull-up present; ~0 V = missing pull-up or line shorted to GND.
void doWiringCheck() {
  Serial.println(F("\n[W] SDA/SCL line check"));
  Wire.end();
  pinMode(A4, INPUT);
  pinMode(A5, INPUT);
  delay(10);
  const uint8_t pins[2] = { A4, A5 };
  const char *names[2] = { "SDA (A4)", "SCL (A5)" };
  for (uint8_t i = 0; i < 2; i++) {
    uint16_t mv = (uint32_t)analogRead(pins[i]) * 5000 / 1023;
    Serial.print(F("  ")); Serial.print(names[i]); Serial.print(F(": "));
    Serial.print(mv); Serial.print(F(" mV  "));
    if (mv > 4000)      Serial.println(F("OK, pull-up present"));
    else if (mv < 500)  Serial.println(F("LOW: missing pull-up or line shorted to GND"));
    else                Serial.println(F("intermediate: unpowered BMS loading the line?"));
  }
  Wire.begin();
  Wire.setClock(100000);
  busTimeout(TIMEOUT_FAST_US);
}

// One CSV line for the scripts (read-only):
// D,pack_mV,c1_mV,c2_mV,c3_mV,current_mA,temp_dC,OperationStatus,PFStatus
// Unreadable fields are -1 (OperationStatus/PFStatus as 4294967295).
void doDump() {
  int32_t pack = readWord(SBS_VOLTAGE);
  int32_t c1 = readWord(SBS_CELL1), c2 = readWord(SBS_CELL2), c3 = readWord(SBS_CELL3);
  int32_t cur = readWord(SBS_CURRENT);
  int32_t tk = readWord(SBS_TEMP);
  uint32_t op = 0xFFFFFFFFUL, pfs = 0xFFFFFFFFUL;
  if (!macRead32(MAC_OP_STATUS, op)) op = 0xFFFFFFFFUL;
  if (!macRead32(MAC_PF_STATUS, pfs)) pfs = 0xFFFFFFFFUL;
  Serial.print(F("D,"));
  Serial.print(pack); Serial.print(',');
  Serial.print(c1);   Serial.print(',');
  Serial.print(c2);   Serial.print(',');
  Serial.print(c3);   Serial.print(',');
  Serial.print(cur < 0 ? -1 : (int16_t)cur); Serial.print(',');
  Serial.print(tk < 0 ? -1 : tk - 2731);     Serial.print(',');
  Serial.print(op);   Serial.print(',');
  Serial.println(pfs);
}

char waitKey() {
  while (!Serial.available()) {}
  char c = Serial.read();
  delay(20);
  while (Serial.available()) Serial.read();       // drop CR/LF
  return c;
}

void doAuto() {
  Serial.println(F("\n[A] Automatic recovery: U -> P -> R -> L"));
  if (!present()) { Serial.println(F("  No reply at 0x0B.")); return; }

  int32_t vmin = minCellMv();
  if (vmin > 0 && vmin < 2000) {
    Serial.print(F("  !! Lowest cell = ")); Serial.print(vmin);
    Serial.println(F(" mV. Below 2 V the cell may be damaged."));
    Serial.println(F("  Continue anyway? (y/n)"));
    char c = waitKey();
    if (c != 'y' && c != 'Y') { Serial.println(F("  Cancelled.")); return; }
  }

  if (!doUnseal()) return;
  doClearPF();
  doReset();

  uint32_t pfs;
  bool still = pfActive(&pfs);
  if (still) {
    // The chip comes back sealed after reset: a second round usually does it
    Serial.println(F("\n  PF still active after reset; second round..."));
    if (doUnseal()) { doClearPF(); doReset(); }
    still = pfActive(&pfs);
  }

  doSeal();
  printInfo();
  if (still) {
    Serial.println(F("\n[!] PF still active. Check contacts (T), keep the wake-up supply on and repeat A."));
  } else {
    Serial.println(F("\n[OK] PF cleared. Put the battery on the original DJI charger and supervise the first charge."));
  }
}

void printMenu() {
  Serial.println(F("\nDJI Spark unbrick — S scan | W lines | I info | T bus test | D csv | U unseal | F full access"));
  Serial.println(F("                    P clear PF | R reset | L seal | A auto | ? menu"));
}

// ───────────────────────── Arduino ─────────────────────────

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(100000);                          // SMBus: 100 kHz max.
  busTimeout(TIMEOUT_FAST_US);
  delay(300);
  printMenu();
}

void loop() {
  if (!Serial.available()) return;
  char c = waitKey();
  switch (c) {
    case 'S': case 's': doScan();        break;
    case 'I': case 'i': case '1': printInfo(); break;
    case 'T': case 't': doBusTest();     break;
    case 'W': case 'w': doWiringCheck(); break;
    case 'D': case 'd': doDump();        break;
    case 'U': case 'u': doUnseal();      break;
    case 'F': case 'f': doFullAccess();  break;
    case 'P': case 'p': doClearPF();     break;
    case 'R': case 'r': doReset();       break;
    case 'L': case 'l': doSeal();        break;
    case 'A': case 'a': doAuto();        break;
    case '?': case 'h': case 'H': printMenu(); break;
    default: break;
  }
}
