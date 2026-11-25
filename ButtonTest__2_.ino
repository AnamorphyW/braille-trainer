/* =====================================================================
 *  ТЕСТ ЖЕЛЕЗА для тренажёра Брайля — прошивать ПЕРЕД основным скетчем
 *  ---------------------------------------------------------------------
 *  Проверяет, что всё припаяно правильно, и подсказывает голосом:
 *    - нажал/отпустил точку 1..6  → говорит «один»…«шесть»
 *    - «Проверка»                 → «семь»
 *    - «Режим»                    → «восемь»
 *    - «Громкость +»              → «девять»
 *    - «Громкость −»              → «десять»
 *  При включении говорит «Тренажёр Брайля готов» (0050) и текущую громкость.
 *  Кнопки «Громкость +/−» реально меняют громкость — так проверяется и звук.
 *
 *  Ничего не получилось? Смотри README, раздел «Если тишина».
 *  Дополнительно всё дублируется в Serial Monitor на 9600 (необязательно).
 *
 *  Пины — ровно те же, что в BrailleTrainer.ino.
 * ===================================================================== */

#include <Arduino.h>
#include <SoftwareSerial.h>

const uint8_t PIN_DOT[6] = {2, 3, 4, 5, 6, 7};
const uint8_t PIN_ENTER  = 8;
const uint8_t PIN_MODE   = 9;
const uint8_t PIN_VOL_UP = A0;
const uint8_t PIN_VOL_DN = A1;
const uint8_t PIN_DF_RX  = 10;   // <- TX плеера
const uint8_t PIN_DF_TX  = 11;   // -> RX плеера через 1 кОм
const uint8_t PIN_BUSY   = 12;
const uint8_t PIN_LED    = 13;

// Если кнопка «наоборот» (говорит при отпускании, а не при нажатии) —
// поставь 1, либо просто перекинь провод на другую крайнюю ножку кнопки.
#define INVERT_BUTTONS 0

SoftwareSerial df(PIN_DF_RX, PIN_DF_TX);

void dfCmd(uint8_t cmd, uint16_t param) {
  uint8_t b[10] = {0x7E, 0xFF, 0x06, cmd, 0x00,
                   (uint8_t)(param >> 8), (uint8_t)(param & 0xFF), 0, 0, 0xEF};
  uint16_t sum = 0;
  for (uint8_t i = 1; i < 7; i++) sum += b[i];
  sum = (uint16_t)(0 - sum);
  b[7] = sum >> 8; b[8] = sum & 0xFF;
  df.write(b, 10);
  delay(30);
}

// говорить число n файлом 00(70+n).mp3
void sayNum(uint8_t n) { dfCmd(0x12, 70 + n); }

uint8_t volume = 20;

struct Btn { uint8_t pin; bool st; bool raw; uint32_t t; const char *name; uint8_t voice; };
Btn btn[10] = {
  {2,  false, false, 0, "точка 1",     1},
  {3,  false, false, 0, "точка 2",     2},
  {4,  false, false, 0, "точка 3",     3},
  {5,  false, false, 0, "точка 4",     4},
  {6,  false, false, 0, "точка 5",     5},
  {7,  false, false, 0, "точка 6",     6},
  {8,  false, false, 0, "ПРОВЕРКА",    7},
  {9,  false, false, 0, "РЕЖИМ",       8},
  {A0, false, false, 0, "ГРОМКОСТЬ +", 9},
  {A1, false, false, 0, "ГРОМКОСТЬ -", 10},
};

bool pressed(uint8_t pin) {
  bool p = (digitalRead(pin) == LOW);
#if INVERT_BUTTONS
  p = !p;
#endif
  return p;
}

void setup() {
  Serial.begin(9600);
  Serial.println(F("=== TEST: nazhimaj knopki, slushaj nomera ==="));
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUSY, INPUT);
  for (uint8_t i = 0; i < 10; i++) {
    pinMode(btn[i].pin, INPUT_PULLUP);
    btn[i].raw = btn[i].st = pressed(btn[i].pin);
    btn[i].t = millis();
  }
  df.begin(9600);
  delay(1500);
  dfCmd(0x09, 0x02);          // источник — TF-карта
  delay(200);
  dfCmd(0x06, volume);        // громкость
  delay(200);
  dfCmd(0x12, 50);            // «Тренажёр Брайля готов»
  delay(2000);
  sayNum(volume);             // «двадцать» — проверка громкости
}

void loop() {
  uint32_t now = millis();
  digitalWrite(PIN_LED, digitalRead(PIN_BUSY) == LOW ? HIGH : LOW);

  for (uint8_t i = 0; i < 10; i++) {
    bool r = pressed(btn[i].pin);
    if (r != btn[i].raw) { btn[i].raw = r; btn[i].t = now; }
    if (now - btn[i].t < 40 || btn[i].st == btn[i].raw) continue;

    btn[i].st = btn[i].raw;
    Serial.print(btn[i].name);
    Serial.println(btn[i].st ? F("  нажата") : F("  отпущена"));

    if (i == 8 && btn[i].st) {                 // громкость +
      if (volume < 30) volume += 2;
      dfCmd(0x06, volume); delay(40); sayNum(volume);
      continue;
    }
    if (i == 9 && btn[i].st) {                 // громкость −
      if (volume > 5) volume = (volume - 2 < 5) ? 5 : volume - 2;
      dfCmd(0x06, volume); delay(40); sayNum(volume);
      continue;
    }
    // у остальных говорим номер и при нажатии, и при отпускании —
    // так видно, что кнопка с фиксацией честно щёлкает в обе стороны
    sayNum(btn[i].voice);
  }
}
