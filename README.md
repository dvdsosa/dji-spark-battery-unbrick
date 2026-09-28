# dji-spark-battery-unbrick

Recuperación de baterías **DJI Spark** bloqueadas en *Permanent Fail* (PF) tras
pasar mucho tiempo descargadas, con un Arduino Uno/Nano y dos scripts de bash
para macOS.

*English summary at the end.*

> **Seguridad.** Una celda Li-ion que ha estado por debajo de ~2 V puede tener
> daño interno y provocar un incendio al recargarse. No recuperes baterías
> hinchadas. Haz la primera carga al aire libre o sobre superficie no
> inflamable, sin perderla de vista. Uso bajo tu responsabilidad.

## Contenido

| Ruta | Qué es |
|---|---|
| `spark_unbrick/spark_unbrick.ino` | Sketch con menú por serie (115200): lectura, unseal, borrado de PF, reset, sellado, volcado CSV |
| `scripts/reset_pf.sh` | Comprueba el bus, muestra el estado, pide confirmación y ejecuta la recuperación |
| `scripts/monitor_carga.sh` | Monitor en pantalla fija (sin scroll): tensión por celda con barras, corriente, temperatura, PF y avisos; guarda CSV en `logs/` |
| `docs/cableado_12v.svg` | Esquema con fuente de 12 V y 100 Ω (probado) |
| `docs/cableado_9v.svg` | Esquema con pila de 9 V (alternativa) |

## El problema

El BMS del Spark es un TI **BQ40Z307** con firmware DJI (serigrafiado
"BQ9003"), en la dirección SMBus `0x0B`. Si una celda baja del umbral de
*Safety Cell Undervoltage*, el chip registra un PF en su memoria flash y abre
los MOSFET de carga y descarga. La batería parece muerta y el cargador no la
acepta. El PF **no se borra solo** al subir la tensión: hay que borrarlo por
SMBus. Además, si las celdas siguen por debajo del umbral, **vuelve a saltar**
en menos de un minuto (observado: con una celda a ~2,17 V se reactivó en
<60 s; el umbral parece rondar los 2,2 V).

## Hardware

- Arduino Uno o Nano (ATmega328P, 5 V)
- 2 × 4,7 kΩ (pull-ups de SDA y SCL a 5 V)
- Fuente de 9–12 V para alimentar el BMS dormido, con **100 Ω en serie** si no
  tiene limitación de corriente (el BMS consume pocos mA; la resistencia limita
  cualquier cortocircuito accidental)

### Opción A — fuente de 12 V con 100 Ω en serie (probada en este proyecto)

Es el montaje con el que se han recuperado las baterías de este repositorio:
Arduino Uno clónico, GND del Arduino en el pin 2 y la fuente de 12 V en los
pines 3 (+, a través de 100 Ω) y 5 (−).

![Cableado con fuente de 12 V (probado)](docs/cableado_12v.svg)

### Opción B — pila de 9 V (alternativa, no probada aquí)

Montaje descrito por la comunidad: GND del Arduino en el pin 5 y la pila PP3
directamente en los pines 3 (+) y 2 (−). Una pila de 9 V no puede dar
corrientes peligrosas, por eso no lleva resistencia en serie.

![Cableado con pila de 9 V (alternativa)](docs/cableado_9v.svg)

Los pines 2 y 5 son las dos masas de la batería; compruébalo con el polímetro
(continuidad ≈ 0 Ω) antes de montar cualquiera de las dos opciones.

Conector de la batería, contactos mirando hacia ti, de izquierda a derecha:

```
  1     2     3     4     5     6
 SCL   GND   BAT+  BAT+  GND   SDA
```

| Batería | Arduino / fuente (opción A, probada) |
|---|---|
| 1 SCL | A5 (+ 4,7 kΩ a 5V) |
| 6 SDA | A4 (+ 4,7 kΩ a 5V) |
| 2 GND | GND del Arduino |
| 3 BAT+ | + de la fuente, a través de 100 Ω |
| 5 GND | − de la fuente |

El esquema de circuitschools (SDA=pin 5, SCL=pin 6) es para Mavic Air, **no**
para Spark.

## Uso

```bash
arduino-cli compile --fqbn arduino:avr:uno spark_unbrick
arduino-cli upload -p /dev/cu.usbserial-10 --fqbn arduino:avr:uno spark_unbrick
```

1. Conecta datos y fuente. Comprueba en el monitor serie (`S`, `W`, `T`, `I`)
   que el BMS responde y que el test de bus da **0 errores**.
2. `./scripts/reset_pf.sh` → confirma → espera a `PF borrado`.
3. **Inmediatamente** quita fuente y cables y pon la batería en el cargador DJI.
4. Si el cargador no la acepta o el PF vuelve (se puede ver con
   `./scripts/monitor_carga.sh` o `I`), repite.

`monitor_carga.sh [intervalo_s] [objetivo_mV] [puerto]` — por defecto 10 s,
3000 mV y `/dev/cu.usbserial-10`. Solo lee (comando `D`). Avisa con alarma si
la temperatura supera 40 °C, si el PF se reactiva o al alcanzar el objetivo.

### Menú del sketch

| Tecla | Acción | Escribe |
|---|---|---|
| `S` | Escanear I2C | no |
| `W` | Medir SDA/SCL (pull-ups, cortos) | no |
| `T` | 300 lecturas, cuenta errores | no |
| `I` | Estado completo | no |
| `D` | Una línea CSV (para el monitor) | no |
| `U` | Unseal (clave Spark, si falla TI por defecto) | sí |
| `F` | Full access (clave TI por defecto) | sí |
| `P` | `PermanentFailDataReset` (0x0029) con verificación | sí |
| `R` | `DeviceReset` (0x0041) | sí |
| `L` | Sellar (0x0030) | sí |
| `A` | U → P → R (dos rondas si hace falta) → L; pide confirmación si una celda < 2 V | sí |

## Notas de protocolo

- Clave de unseal Spark: `0xCCDF7EE0`, se escribe en `ManufacturerAccess`
  (0x00) como palabra baja `0x7EE0` y luego alta `0xCCDF`.
- Lecturas de estado: subcomando a `0x00` y respuesta en bloque desde
  `ManufacturerData` (0x23). `OperationStatus` (0x0054): `SEC` = bits 8–9
  (3 sellado, 2 unsealed, 1 full access), `PF` bit 12, `XDSG` 13, `XCHG` 14.
- Tras `DeviceReset` el chip vuelve sellado solo.

Diferencias con otros proyectos revisados: algunos leen el nivel de seguridad
del byte equivocado de la respuesta y tratan `SEC=1` como "unsealed", y envían
`0x002A`/`0x002B` como "PF clear", subcomandos que en el TRM de TI son otros
(reset del *black box* y LEDs). Aquí solo se usa `0x0029`.

## Créditos

- [o-gs/dji-firmware-tools#258](https://github.com/o-gs/dji-firmware-tools/issues/258):
  clave `0xCCDF7EE0` y secuencia `Unseal → PermanentFailDataReset → Seal` para Spark
- [Lishen99/DJI-Spark-Battery-Recovery-ESP32](https://github.com/Lishen99/DJI-Spark-Battery-Recovery-ESP32)
  y [lv70/spark-battery-nano](https://github.com/lv70/spark-battery-nano)
- [davext/unbrick-dji](https://github.com/davext/unbrick-dji)

---

## English summary

Arduino (Uno/Nano) sketch plus two macOS bash scripts to recover DJI Spark
batteries whose BQ40Z307 ("BQ9003") BMS latched Permanent Fail after deep
discharge. Unseal key `0xCCDF7EE0`, then `PermanentFailDataReset` (0x0029),
`DeviceReset`, seal. The PF re-latches within a minute if a cell is still below
the undervoltage threshold (~2.2 V), so move the pack to the DJI charger right
after clearing. `monitor_carga.sh` shows a live, fixed-screen dashboard and
logs CSV. Deeply discharged Li-ion cells are a fire risk: charge supervised,
on a non-flammable surface.

## Licencia

MIT. Ver [LICENSE](LICENSE).
