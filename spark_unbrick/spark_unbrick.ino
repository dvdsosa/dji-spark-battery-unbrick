/*
 * spark_unbrick.ino — Recuperación de baterías DJI Spark en Permanent Fail (PF)
 * ---------------------------------------------------------------------------
 * Placa: Arduino Uno / Nano (ATmega328P, 5 V, 16 MHz). Monitor serie a 115200.
 *
 * Chip BMS: TI BQ40Z307 con firmware DJI (serigrafiado "BQ9003"), SMBus 0x0B.
 * Clave de unseal Spark: 0xCCDF7EE0 (se envía palabra baja 0x7EE0 y luego
 * alta 0xCCDF). Confirmada por la comunidad en o-gs/dji-firmware-tools #258.
 *
 * CONEXIONES (conector de la batería, contactos mirando hacia ti, de izq. a dcha.)
 *
 *     1     2     3     4     5     6
 *    SCL   GND   BAT+  BAT+  GND   SDA
 *
 *    Pin 1 (SCL) -> A5  + resistencia 4.7k a 5V
 *    Pin 6 (SDA) -> A4  + resistencia 4.7k a 5V
 *    Pin 2 (GND) -> GND del Arduino
 *    Pin 3 (+) / Pin 5 (-) -> pila de 9 V SOLO para despertar el BMS.
 *                             ¡Nunca conectes BAT+ ni la pila al Arduino!
 *
 *  OJO: el esquema de circuitschools (SDA=pin5, SCL=pin6) es para Mavic Air,
 *  NO para Spark.
 *
 * MENÚ
 *   S  escanear bus               I  información/estado (solo lectura)
 *   T  test de estabilidad del bus  W  medir tensión de SDA/SCL (pull-ups)
 *   U  unseal                     F  full access (clave TI por defecto)
 *   P  borrar datos de PF (0x0029) R  reset del chip (0x0041)
 *   L  sellar (0x0030)            A  recuperación automática U→P→R→L
 *
 * Uso bajo tu responsabilidad. Una celda Li-ion que ha pasado mucho tiempo
 * muy descargada puede estar dañada: primera carga vigilada y sobre
 * superficie no inflamable; si la batería está hinchada, no la recuperes.
 */

#include <Wire.h>

#define BATT_ADDR 0x0B

// Comandos SBS estándar
#define SBS_MAC        0x00   // ManufacturerAccess (escritura de subcomandos)
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
#define SBS_MAC_DATA   0x23   // ManufacturerData (respuesta de subcomandos)
#define SBS_CELL3      0x3D
#define SBS_CELL2      0x3E
#define SBS_CELL1      0x3F
#define SBS_SOH        0x4F

// Subcomandos ManufacturerAccess (TRM bq40z50 / bq40z307)
#define MAC_DEVICE_TYPE   0x0001
#define MAC_FW_VERSION    0x0002
#define MAC_PF_DATA_RESET 0x0029
#define MAC_SEAL          0x0030
#define MAC_DEVICE_RESET  0x0041
#define MAC_SAFETY_STATUS 0x0051
#define MAC_PF_STATUS     0x0053
#define MAC_OP_STATUS     0x0054

// OperationStatus: bits relevantes
#define OP_SEC_SHIFT 8        // SEC1:SEC0 -> 3 sellado, 2 unsealed, 1 full access
#define OP_PF        (1UL << 12)
#define OP_XDSG      (1UL << 13)
#define OP_XCHG      (1UL << 14)
#define OP_DSG       (1UL << 1)
#define OP_CHG       (1UL << 2)

struct KeyPair { uint16_t w0, w1; };
const KeyPair KEY_SPARK      = { 0x7EE0, 0xCCDF };  // 0xCCDF7EE0
const KeyPair KEY_TI_UNSEAL  = { 0x0414, 0x3672 };  // 0x36720414 (TI por defecto)
const KeyPair KEY_TI_FULL    = { 0xFFFF, 0xFFFF };  // 0xFFFFFFFF (TI por defecto)

#define TIMEOUT_FAST_US  50000UL    // lecturas normales
#define TIMEOUT_SLOW_US  3000000UL  // escrituras en flash: el chip estira el reloj

// ───────────────────────── SMBus de bajo nivel ─────────────────────────

void busTimeout(uint32_t us) {
#ifdef WIRE_HAS_TIMEOUT
  Wire.setWireTimeout(us, true);
#else
  (void)us;
#endif
}

// Devuelve el código de Wire.endTransmission(): 0 = ACK
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

// Con reintentos: los contactos del conector Spark son poco fiables
int32_t readWord(uint8_t cmd) {
  for (uint8_t i = 0; i < 3; i++) {
    int32_t v = readWordOnce(cmd);
    if (v >= 0) return v;
    delay(10);
  }
  return -1;
}

// SMBus block read: [count][data...]. Devuelve nº de bytes o -1.
int8_t readBlock(uint8_t cmd, uint8_t *buf, uint8_t maxLen) {
  Wire.beginTransmission(BATT_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return -1;
  uint8_t req = maxLen + 1;
  if (req > 32) req = 32;                                // buffer de Wire en AVR
  if (Wire.requestFrom((uint8_t)BATT_ADDR, req) == 0) return -1;
  uint8_t len = Wire.read();
  if (len > req - 1) len = req - 1;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.available() ? Wire.read() : 0;
  while (Wire.available()) Wire.read();
  return len;
}

// Subcomando por ManufacturerAccess y respuesta por ManufacturerData (0x23).
// Funciona también con el chip sellado para los comandos de estado.
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

// ───────────────────────── utilidades de impresión ─────────────────────────

void printHex(uint32_t v, uint8_t digits) {
  Serial.print(F("0x"));
  for (int8_t s = (digits - 1) * 4; s >= 0; s -= 4) Serial.print((v >> s) & 0xF, HEX);
}

void printString(uint8_t cmd) {
  uint8_t b[21];
  int8_t n = readBlock(cmd, b, 20);
  if (n <= 0) { Serial.println(F("(sin respuesta)")); return; }
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

// ───────────────────────── estado ─────────────────────────

// Devuelve el nivel de seguridad (1..3) o 0 si no se pudo leer
uint8_t readSec(uint32_t *opOut = NULL) {
  uint32_t op;
  if (!macRead32(MAC_OP_STATUS, op)) return 0;
  if (opOut) *opOut = op;
  return (op >> OP_SEC_SHIFT) & 0x03;
}

// Tensión mínima de celda en mV (0 si no se pudo leer)
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
  Serial.println(F("\n======== Estado de la batería ========"));
  if (!present()) {
    Serial.println(F("No responde en 0x0B. Revisa cableado, pull-ups y la pila de 9 V."));
    return;
  }

  Serial.print(F("Fabricante     : ")); printString(SBS_MFG_NAME);
  Serial.print(F("Dispositivo    : ")); printString(SBS_DEV_NAME);

  uint8_t b[4];
  if (macRead(MAC_DEVICE_TYPE, b, 2) >= 2) {
    Serial.print(F("Device type    : "));
    printHex(b[0] | (b[1] << 8), 4);
    Serial.println();
  }

  int32_t w;
  if ((w = readWord(SBS_SERIAL)) >= 0)  { Serial.print(F("Nº serie SBS   : ")); Serial.println(w); }
  if ((w = readWord(SBS_MFG_DATE)) >= 0) {
    Serial.print(F("Fabricada      : "));
    Serial.print(1980 + ((w >> 9) & 0x7F)); Serial.print('-');
    Serial.print((w >> 5) & 0x0F);          Serial.print('-');
    Serial.println(w & 0x1F);
  }
  if ((w = readWord(SBS_CYCLES)) >= 0) { Serial.print(F("Ciclos         : ")); Serial.println(w); }

  int32_t dc = readWord(SBS_DESIGN_CAP), fcc = readWord(SBS_FCC);
  if (dc > 0 && fcc >= 0) {
    Serial.print(F("Capacidad      : ")); Serial.print(fcc); Serial.print(F(" / "));
    Serial.print(dc); Serial.print(F(" mAh (")); Serial.print(fcc * 100 / dc); Serial.println(F("%)"));
  }
  if ((w = readWord(SBS_SOH)) >= 0)  { Serial.print(F("SOH            : ")); Serial.print(w); Serial.println('%'); }
  if ((w = readWord(SBS_RSOC)) >= 0) { Serial.print(F("Carga relativa : ")); Serial.print(w); Serial.println('%'); }

  if ((w = readWord(SBS_VOLTAGE)) >= 0) { Serial.print(F("Tensión pack   : ")); Serial.print(w); Serial.println(F(" mV")); }
  const uint8_t regs[3] = { SBS_CELL1, SBS_CELL2, SBS_CELL3 };
  int32_t vmin = 0, vmax = 0;
  for (uint8_t i = 0; i < 3; i++) {
    int32_t v = readWord(regs[i]);
    Serial.print(F("Celda ")); Serial.print(i + 1); Serial.print(F("        : "));
    if (v < 0) { Serial.println(F("error")); continue; }
    Serial.print(v); Serial.println(F(" mV"));
    if (v > 0 && (vmin == 0 || v < vmin)) vmin = v;
    if (v > vmax) vmax = v;
  }
  if (vmin) {
    Serial.print(F("Desequilibrio  : ")); Serial.print(vmax - vmin); Serial.println(F(" mV"));
    if (vmin < 2000) Serial.println(F("  !! Celda < 2.0 V: posible daño interno. Carga vigilada o descártala."));
  }

  if ((w = readWord(SBS_TEMP)) >= 0) {
    Serial.print(F("Temperatura    : ")); Serial.print(w / 10.0 - 273.15, 1); Serial.println(F(" C"));
  }
  if ((w = readWord(SBS_CURRENT)) >= 0) { Serial.print(F("Corriente      : ")); Serial.print((int16_t)w); Serial.println(F(" mA")); }
  if ((w = readWord(SBS_STATUS)) >= 0)  { Serial.print(F("BatteryStatus  : ")); printHex(w, 4); Serial.println(); }

  uint32_t op;
  uint8_t sec = readSec(&op);
  if (sec) {
    Serial.print(F("OperationStatus: ")); printHex(op, 8); Serial.println();
    Serial.print(F("  Seguridad    : ")); Serial.println(secName(sec));
    Serial.print(F("  PF activo    : ")); Serial.println(op & OP_PF ? F("SÍ  <-- bloqueada") : F("no"));
    Serial.print(F("  Carga        : ")); Serial.println(op & OP_XCHG ? F("deshabilitada") : (op & OP_CHG ? F("FET ON") : F("FET OFF")));
    Serial.print(F("  Descarga     : ")); Serial.println(op & OP_XDSG ? F("deshabilitada") : (op & OP_DSG ? F("FET ON") : F("FET OFF")));
  } else {
    Serial.println(F("OperationStatus: no se pudo leer"));
  }

  uint32_t v32;
  if (macRead32(MAC_SAFETY_STATUS, v32)) {
    Serial.print(F("SafetyStatus   : ")); printHex(v32, 8);
    if (v32 & 0x01) Serial.print(F("  CUV (subtensión de celda)"));
    if (v32 & 0x02) Serial.print(F("  COV (sobretensión de celda)"));
    Serial.println();
  }
  if (macRead32(MAC_PF_STATUS, v32)) {
    Serial.print(F("PFStatus       : ")); printHex(v32, 8);
    Serial.println(v32 ? F("  <-- fallo permanente registrado") : F("  OK"));
  }
  Serial.println(F("======================================"));
}

// ───────────────────────── operaciones de escritura ─────────────────────────

// Envía las dos mitades de la clave seguidas (el chip exige <4 s entre ellas).
// El chip puede hacer NACK en alguna palabra: no es concluyente, se verifica después.
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
    Serial.print(F(" intento ")); Serial.print(attempt);
    Serial.print(F(" -> ")); Serial.println(secName(sec));
    if (sec != 0 && sec <= target) return true;
    delay(300);
  }
  return false;
}

bool doUnseal() {
  Serial.println(F("\n[U] Unseal"));
  uint8_t sec = readSec();
  if (sec == 1 || sec == 2) { Serial.println(F("  Ya estaba abierta.")); return true; }
  if (tryKey(KEY_SPARK, 2, F("clave Spark 0xCCDF7EE0"))) return true;
  Serial.println(F("  La clave Spark no funcionó; pruebo la clave TI por defecto."));
  if (tryKey(KEY_TI_UNSEAL, 2, F("clave TI 0x36720414"))) return true;
  Serial.println(F("  [X] No se pudo hacer unseal. Revisa contactos (usa T) y alimentación."));
  return false;
}

bool doFullAccess() {
  Serial.println(F("\n[F] Full access"));
  if (readSec() == 3 && !doUnseal()) return false;
  if (tryKey(KEY_TI_FULL, 1, F("clave full TI 0xFFFFFFFF"))) return true;
  Serial.println(F("  No disponible (no es necesario para borrar el PF)."));
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
    Serial.println(F("  El chip está sellado: ejecuta U antes."));
    return false;
  }
  busTimeout(TIMEOUT_SLOW_US);
  bool ok = false;
  // En la comunidad a veces hace falta repetirlo varias veces
  for (uint8_t attempt = 1; attempt <= 5; attempt++) {
    uint8_t r = writeWord(SBS_MAC, MAC_PF_DATA_RESET);
    delay(1000);                                  // escritura en data flash
    uint32_t pfs;
    bool still = pfActive(&pfs);
    Serial.print(F("  intento ")); Serial.print(attempt);
    Serial.print(r == 0 ? F(" ACK") : (r == 5 ? F(" timeout") : F(" NACK")));
    Serial.print(F("  PFStatus=")); printHex(pfs, 8);
    Serial.println(still ? F("  (sigue)") : F("  (borrado)"));
    if (!still) { ok = true; break; }
  }
  busTimeout(TIMEOUT_FAST_US);
  if (!ok) Serial.println(F("  El bit PF puede no actualizarse hasta el reset (R). Continúa."));
  return ok;
}

void doReset() {
  Serial.println(F("\n[R] Reset del chip (0x0041)"));
  busTimeout(TIMEOUT_SLOW_US);
  writeWord(SBS_MAC, MAC_DEVICE_RESET);
  busTimeout(TIMEOUT_FAST_US);
  for (uint8_t i = 0; i < 30; i++) {              // hasta ~3 s para reiniciar
    delay(100);
    if (present()) {
      Serial.println(F("  El BMS ha vuelto a responder."));
      delay(500);
      return;
    }
  }
  Serial.println(F("  El BMS no responde tras el reset (¿se soltó la pila de 9 V?)."));
}

void doSeal() {
  Serial.println(F("\n[L] Sellar (0x0030)"));
  uint8_t sec = readSec();
  if (sec == 3) { Serial.println(F("  Ya está sellada.")); return; }
  writeWord(SBS_MAC, MAC_SEAL);
  delay(300);
  Serial.print(F("  Estado: ")); Serial.println(secName(readSec()));
}

// ───────────────────────── herramientas ─────────────────────────

void doScan() {
  Serial.println(F("\n[S] Escaneando I2C..."));
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.print(F("  ")); printHex(a, 2);
      Serial.println(a == BATT_ADDR ? F("  <- BMS de la batería") : F(""));
      found++;
    }
  }
  if (!found) Serial.println(F("  Nada. ¿Pila de 9 V aplicada? ¿SDA/SCL invertidos? ¿Pull-ups?"));
}

void doBusTest() {
  Serial.println(F("\n[T] 300 lecturas de tensión..."));
  uint16_t errors = 0;
  for (uint16_t i = 0; i < 300; i++) if (readWordOnce(SBS_VOLTAGE) < 0) errors++;
  Serial.print(F("  Errores: ")); Serial.println(errors);
  Serial.println(errors == 0 ? F("  Bus estable: puedes escribir.") :
                               F("  Bus inestable: sujeta mejor los contactos antes de escribir."));
}

// Mide SDA/SCL en reposo con las pull-ups internas desactivadas:
// ~5 V = pull-up externa presente; ~0 V = sin pull-up o línea en corto a GND.
void doWiringCheck() {
  Serial.println(F("\n[W] Comprobación de líneas SDA/SCL"));
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
    if (mv > 4000)      Serial.println(F("OK, pull-up presente"));
    else if (mv < 500)  Serial.println(F("BAJA: falta pull-up o línea a GND"));
    else                Serial.println(F("intermedia: ¿BMS sin alimentar cargando la línea?"));
  }
  Wire.begin();
  Wire.setClock(100000);
  busTimeout(TIMEOUT_FAST_US);
}

// Una línea CSV para monitor_carga.sh (solo lectura):
// D,pack_mV,c1_mV,c2_mV,c3_mV,corriente_mA,temp_dC,OperationStatus,PFStatus
// Cualquier campo ilegible sale como -1 (OperationStatus/PFStatus como 4294967295).
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
  while (Serial.available()) Serial.read();       // descarta CR/LF
  return c;
}

void doAuto() {
  Serial.println(F("\n[A] Recuperación automática: U -> P -> R -> L"));
  if (!present()) { Serial.println(F("  No responde en 0x0B.")); return; }

  int32_t vmin = minCellMv();
  if (vmin > 0 && vmin < 2000) {
    Serial.print(F("  !! Celda mínima = ")); Serial.print(vmin);
    Serial.println(F(" mV. Por debajo de 2 V la celda puede estar dañada."));
    Serial.println(F("  ¿Continuar igualmente? (y/n)"));
    char c = waitKey();
    if (c != 'y' && c != 'Y') { Serial.println(F("  Cancelado.")); return; }
  }

  if (!doUnseal()) return;
  doClearPF();
  doReset();

  uint32_t pfs;
  bool still = pfActive(&pfs);
  if (still) {
    // Tras el reset el chip vuelve sellado: una segunda ronda suele bastar
    Serial.println(F("\n  PF sigue activo tras el reset; segunda ronda..."));
    if (doUnseal()) { doClearPF(); doReset(); }
    still = pfActive(&pfs);
  }

  doSeal();
  printInfo();
  if (still) {
    Serial.println(F("\n[!] El PF sigue activo. Revisa contactos (T), mantén la pila de 9 V y repite A."));
  } else {
    Serial.println(F("\n[OK] PF borrado. Pon la batería en el cargador DJI original y vigila la primera carga."));
  }
}

void printMenu() {
  Serial.println(F("\nDJI Spark unbrick — S escanear | W líneas | I info | T test bus | U unseal | F full access"));
  Serial.println(F("                    P borrar PF | R reset | L sellar | A automático | ? menú"));
}

// ───────────────────────── Arduino ─────────────────────────

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(100000);                          // SMBus: 100 kHz máx.
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
