/* =====================================================================
 *  Тренажёр Брайля для слепых — автономное устройство на Arduino
 *  ---------------------------------------------------------------------
 *  Железо:
 *    - Arduino Uno / Nano / Pro Mini (ATmega328P, 5 В)
 *    - 6 кнопок С ФИКСАЦИЕЙ PB22E06 (DPDT) — точки Брайля 1..6
 *    - 1 обычная кнопка без фиксации — «Проверка» (Enter)
 *    - 3 обычные кнопки — «Режим», «Громкость +», «Громкость −»
 *    - DFPlayer Mini (HW-247A) + динамик 8 Ом на SPK1/SPK2
 *    - microSD: FAT32, папка MP3, файлы 0001.mp3 ... 0100.mp3
 *
 *  Режим 1 «Алфавит по порядку» (работает):
 *    устройство говорит «Набери букву А» → пользователь выставляет точки
 *    фиксирующимися кнопками → жмёт «Проверка» → «Верно / Неверно» →
 *    следующая буква. 2 ошибки → устройство само даёт правильный ответ.
 *
 *  Режимы 2 и 3 — заглушки («Режим пока не готов»), кнопки уже работают.
 *
 *  Без компьютера: питание от батареи/БП, вся обратная связь — голосом.
 *  Библиотек ставить НЕ нужно, драйвер DFPlayer встроен.
 * ===================================================================== */

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <EEPROM.h>

/* ---- Не удалять этот блок ---------------------------------------------
   Arduino IDE автоматически вставляет в начало файла прототипы функций.
   Для «void initButton(Button&)» тип Button должен быть уже известен,
   иначе получается ошибка:
      error: 'Button' was not declared in this scope
      error: variable or field 'initButton' declared void
   Поэтому тип и прототипы объявляем руками прямо после #include. */
struct Button;
void    initButton(Button &b);
uint8_t updateButton(Button &b);
/* ------------------------------------------------------------------------- */

/* ============================ ПИНЫ ================================= */
// Точки Брайля. Порядок в массиве = точка 1,2,3,4,5,6
// Физически ставим их так (как в клетке Брайля):
//      1  4
//      2  5
//      3  6
const uint8_t PIN_DOT[6] = {2, 3, 4, 5, 6, 7};

const uint8_t PIN_ENTER   = 8;    // «Проверка», без фиксации
const uint8_t PIN_MODE    = 9;    // «Режим»,   без фиксации
const uint8_t PIN_VOL_UP  = A0;   // «Громкость +»
const uint8_t PIN_VOL_DN  = A1;   // «Громкость −»

const uint8_t PIN_DF_RX   = 10;   // Arduino RX  <- TX DFPlayer
const uint8_t PIN_DF_TX   = 11;   // Arduino TX  -> RX DFPlayer через 1 кОм
const uint8_t PIN_BUSY    = 12;   // BUSY DFPlayer (LOW = играет). ОЧЕНЬ желателен
const uint8_t PIN_LED     = 13;   // светодиод для отладки, можно не ставить

// Свободны под будущие кнопки прямого выбора режима: A2, A3, A4, A5

/* ======================= НАСТРОЙКИ ПОВЕДЕНИЯ ======================= */
#define USE_BUSY_PIN      1     // 1 = BUSY подключён (правильно), 0 = таймеры вслепую
#define DF_PLAY_CMD       0x12  // 0x12 = файл MP3/NNNN.mp3 по имени (правильный вариант)
                                // 0x03 = файл N по ПОРЯДКУ КОПИРОВАНИЯ в корне карты —
                                // запасной вариант, если клон не понимает папку MP3 (см. SoundTest)
#define GIVE_UP_AFTER     2     // через сколько ошибок подряд сказать правильный ответ
#define SPEAK_DOT_NUMBER  0     // 1 = при постановке точки говорить её номер (шумно, но полезно на старте)
#define INVERT_DOTS       1     // 1 = ИНВЕРТИРОВАННАЯ карта: для каждой буквы засчитываются
                                //     точки, которые в стандартной карте в эту букву НЕ входят.
                                //     Например А: стандартно точка 1, инвертированно 2,3,4,5,6.
                                //     Реализовано как побитовая инверсия в пределах 6 точек
                                //     (XOR с 0x3F) в функции letterMask().
                                // 0 = стандартная карта Брайля (А = точка 1 и т.д.)

const uint8_t  VOL_MIN        = 5;    // ниже не опускаем, чтобы не «пропал звук»
const uint8_t  VOL_MAX        = 30;   // максимум DFPlayer
const uint8_t  VOL_STEP       = 2;
const uint8_t  VOL_DEFAULT    = 20;

const uint16_t DEBOUNCE_MS    = 40;   // антидребезг
const uint16_t ENTER_LONG_MS  = 900;  // удержание «Проверка» → повторить задание
const uint16_t ENTER_HINT_MS  = 2200; // удержание дольше → подсказка (номера точек)
const uint16_t MODE_LONG_MS   = 1000; // удержание «Режим» → переключить режим
const uint16_t VOL_HOLD_MS    = 500;  // удержание громкости → автоповтор
const uint16_t VOL_REPEAT_MS  = 250;

const uint8_t  MODE_COUNT     = 3;    // 1 рабочий + 2 заглушки
const uint8_t  LETTER_COUNT   = 33;   // русский алфавит, включая Ё и Й

/* ==================== НОМЕРА ЗВУКОВЫХ ФАЙЛОВ ======================= */
// Файлы лежат в /MP3/ и называются 0001.mp3 ... 0100.mp3
#define TRK_LETTER(i)   (uint16_t)(1 + (i))   // 0001..0033 — названия букв А..Я
#define TRK_TASK        40   // «Набери букву»
#define TRK_OK          41   // «Верно»
#define TRK_BAD         42   // «Неверно»
#define TRK_ANSWER      43   // «Правильный ответ: буква»
#define TRK_DOTS        44   // «Точки»
#define TRK_TRY_AGAIN   45   // «Попробуй ещё раз»
#define TRK_RELEASE     46   // «Отпусти все кнопки»
#define TRK_MODE_1      47   // «Режим: алфавит по порядку»
#define TRK_MODE_SOON   48   // «Этот режим пока не готов»
#define TRK_VOLUME      49   // «Громкость»
#define TRK_HELLO       50   // приветствие
#define TRK_DONE        51   // «Алфавит пройден, начинаем сначала»
#define TRK_HINT        52   // «Подсказка»
#define TRK_EMPTY       53   // «Сначала набери букву на кнопках»
#define TRK_TYPED       54   // «Набрано: буква»
#define TRK_MODE_WORD   55   // «Режим»
#define TRK_NUM(n)      (uint16_t)(70 + (n))  // 0070..0100 — числа 0..30

/* ================= ТАБЛИЦА БРАЙЛЯ (русский алфавит) ================= */
// ЭТАЛОННАЯ (стандартная) карта русского Брайля. Бит 0 = точка 1, ... бит 5 = точка 6.
// Реальное поведение задаёт флаг INVERT_DOTS: при INVERT_DOTS=1 функция letterMask()
// возвращает инвертированную маску (XOR 0x3F), т.е. А засчитывается за точки 2,3,4,5,6
// вместо одной точки 1. Сама таблица не меняется — она остаётся справочной.
const uint8_t BRAILLE[LETTER_COUNT] PROGMEM = {
  0x01, //  1 А  = 1
  0x03, //  2 Б  = 1,2
  0x3A, //  3 В  = 2,4,5,6
  0x1B, //  4 Г  = 1,2,4,5
  0x19, //  5 Д  = 1,4,5
  0x11, //  6 Е  = 1,5
  0x21, //  7 Ё  = 1,6
  0x1A, //  8 Ж  = 2,4,5
  0x35, //  9 З  = 1,3,5,6
  0x0A, // 10 И  = 2,4
  0x2F, // 11 Й  = 1,2,3,4,6
  0x05, // 12 К  = 1,3
  0x07, // 13 Л  = 1,2,3
  0x0D, // 14 М  = 1,3,4
  0x1D, // 15 Н  = 1,3,4,5
  0x15, // 16 О  = 1,3,5
  0x0F, // 17 П  = 1,2,3,4
  0x17, // 18 Р  = 1,2,3,5
  0x0E, // 19 С  = 2,3,4
  0x1E, // 20 Т  = 2,3,4,5
  0x25, // 21 У  = 1,3,6
  0x0B, // 22 Ф  = 1,2,4
  0x13, // 23 Х  = 1,2,5
  0x09, // 24 Ц  = 1,4
  0x1F, // 25 Ч  = 1,2,3,4,5
  0x31, // 26 Ш  = 1,5,6
  0x2D, // 27 Щ  = 1,3,4,6
  0x37, // 28 Ъ  = 1,2,3,5,6
  0x2E, // 29 Ы  = 2,3,4,6
  0x3E, // 30 Ь  = 2,3,4,5,6
  0x2A, // 31 Э  = 2,4,6
  0x33, // 32 Ю  = 1,2,5,6
  0x2B  // 33 Я  = 1,2,4,6
};

// Возвращает маску точек для буквы i. При INVERT_DOTS=1 маска инвертируется
// побитово в пределах 6 точек: каждая точка заменяется на «не точку».
// Это единая точка применения инверсии — все вызовы (doCheck, findLetter,
// doHint, sayDotNumbers) автоматически получают инвертированные значения.
static inline uint8_t letterMask(uint8_t i) {
  uint8_t m = pgm_read_byte(&BRAILLE[i]);
#if INVERT_DOTS
  m ^= 0x3F;   // инверсия в пределах 6 точек: 0x3F = биты 0..5
#endif
  return m;
}

/* ========================== DFPlayer =============================== */
SoftwareSerial dfSerial(PIN_DF_RX, PIN_DF_TX);

// Команда без запроса подтверждения (feedback = 0) — так проще и стабильнее
void dfCmd(uint8_t cmd, uint16_t param) {
  uint8_t b[10];
  b[0] = 0x7E; b[1] = 0xFF; b[2] = 0x06; b[3] = cmd; b[4] = 0x00;
  b[5] = (uint8_t)(param >> 8); b[6] = (uint8_t)(param & 0xFF);
  uint16_t sum = 0;
  for (uint8_t i = 1; i < 7; i++) sum += b[i];
  sum = (uint16_t)(0 - sum);
  b[7] = (uint8_t)(sum >> 8); b[8] = (uint8_t)(sum & 0xFF); b[9] = 0xEF;
  dfSerial.write(b, 10);
  delay(30);                       // модулю нужна пауза между командами
}

void dfSetVolume(uint8_t v) { dfCmd(0x06, v); }
void dfPlay(uint16_t idx)   { dfCmd(DF_PLAY_CMD, idx); }
void dfStop()               { dfCmd(0x16, 0); }

/* ============ ОЧЕРЕДЬ ФРАЗ (неблокирующая, по BUSY) ================= */
const uint8_t QMAX = 12;
uint16_t q[QMAX];
uint8_t  qLen = 0, qPos = 0;
bool     spkActive = false;
bool     spkSawBusy = false;          // видели ли уже LOW на BUSY (файл реально начал играть)
uint32_t spkStart = 0, spkHigh = 0;

void speechStartNext() {
  if (qPos >= qLen) { spkActive = false; qLen = qPos = 0; return; }
  dfPlay(q[qPos++]);
  spkActive = true;
  spkSawBusy = false;
  spkStart = millis();
  spkHigh  = millis();
}

// say() добавляет файл в очередь; первая фраза стартует сразу
void say(uint16_t track) {
  if (qLen < QMAX) q[qLen++] = track;
  if (!spkActive) speechStartNext();
}

void speechReset() { qLen = qPos = 0; spkActive = false; dfStop(); }

bool speechIdle() { return !spkActive && qLen == 0; }

void speechTask() {
  if (!spkActive) return;
  uint32_t now = millis();

#if USE_BUSY_PIN
  if (digitalRead(PIN_BUSY) == LOW) {           // LOW = играет
    spkSawBusy = true;
    spkHigh = now;
    if (now - spkStart > 15000UL) speechStartNext();  // страховка от зависания
    return;
  }
  // BUSY = HIGH: либо файл ещё не начался, либо уже закончился
  if (!spkSawBusy && now - spkStart < 1200UL) return;  // ждём старта (медленная карта)
  if (now - spkHigh < 150) return;                     // пауза между файлами
  speechStartNext();
#else
  if (now - spkStart < 2000UL) return;          // вслепую: 2 с на файл
  speechStartNext();
#endif
}

/* ============================ КНОПКИ =============================== */
struct Button {
  uint8_t  pin;
  bool     state;        // стабильное состояние: true = нажата
  bool     raw;
  uint32_t tRaw;
  uint32_t tPress;
  bool     longDone;
  bool     hintDone;
  uint32_t tRepeat;
};

Button bEnter = {PIN_ENTER, false, false, 0, 0, false, false, 0};
Button bMode  = {PIN_MODE,  false, false, 0, 0, false, false, 0};
Button bUp    = {PIN_VOL_UP,false, false, 0, 0, false, false, 0};
Button bDn    = {PIN_VOL_DN,false, false, 0, 0, false, false, 0};

bool     dotState[6] = {false,false,false,false,false,false};
bool     dotRaw[6];
uint32_t dotTRaw[6];

// события, которые loop() разберёт сам
bool evEnterShort = false, evEnterLong = false, evEnterHint = false;
bool evModeShort  = false, evModeLong  = false;
bool evVolUp = false, evVolDn = false;
int8_t evDotChanged = -1;

void initButton(Button &b) {
  pinMode(b.pin, INPUT_PULLUP);
  b.raw = b.state = (digitalRead(b.pin) == LOW);
  b.tRaw = millis();
}

// возвращает bitmask: 1 = нажата только что, 2 = отпущена только что
uint8_t updateButton(Button &b) {
  uint8_t ev = 0;
  bool r = (digitalRead(b.pin) == LOW);
  uint32_t now = millis();
  if (r != b.raw) { b.raw = r; b.tRaw = now; }
  if (now - b.tRaw >= DEBOUNCE_MS && b.state != b.raw) {
    b.state = b.raw;
    if (b.state) { ev |= 1; b.tPress = now; b.longDone = false; b.hintDone = false; b.tRepeat = now; }
    else         { ev |= 2; }
  }
  return ev;
}

void readDotsTask() {
  uint32_t now = millis();
  for (uint8_t i = 0; i < 6; i++) {
    bool r = (digitalRead(PIN_DOT[i]) == LOW);
    if (r != dotRaw[i]) { dotRaw[i] = r; dotTRaw[i] = now; }
    if (now - dotTRaw[i] >= DEBOUNCE_MS && dotState[i] != dotRaw[i]) {
      dotState[i] = dotRaw[i];
      evDotChanged = i;
    }
  }
}

uint8_t dotsMask() {
  uint8_t m = 0;
  for (uint8_t i = 0; i < 6; i++) if (dotState[i]) m |= (1 << i);
  return m;
}

bool allDotsReleased() { return dotsMask() == 0; }

/* ===================== СОСТОЯНИЕ УСТРОЙСТВА ======================== */
enum State { ST_INPUT, ST_RELEASE, ST_CLEAR, ST_STUB };
State   state = ST_INPUT;
uint8_t mode = 1;             // 1 = алфавит по порядку
uint8_t letterIdx = 0;        // 0 = А
uint8_t attempts = 0;
uint8_t volume = VOL_DEFAULT;

uint32_t tSettingsDirty = 0;
bool     settingsDirty = false;

const uint8_t EE_MAGIC = 0xB7;
void loadSettings() {
  if (EEPROM.read(0) == EE_MAGIC) {
    volume    = constrain(EEPROM.read(1), VOL_MIN, VOL_MAX);
    letterIdx = EEPROM.read(2) % LETTER_COUNT;
    mode      = constrain(EEPROM.read(3), 1, MODE_COUNT);
  } else {
    EEPROM.update(0, EE_MAGIC);
    volume = VOL_DEFAULT; letterIdx = 0; mode = 1;
    settingsDirty = true; tSettingsDirty = millis();
  }
}
void markDirty() { settingsDirty = true; tSettingsDirty = millis(); }
void saveTask() {                       // пишем EEPROM не чаще, чем через 4 с покоя
  if (!settingsDirty || millis() - tSettingsDirty < 4000UL) return;
  EEPROM.update(1, volume);
  EEPROM.update(2, letterIdx);
  EEPROM.update(3, mode);
  settingsDirty = false;
}

/* ========================= ЛОГИКА РЕЖИМА 1 ========================= */
int8_t findLetter(uint8_t mask) {
  if (!mask) return -1;
  for (uint8_t i = 0; i < LETTER_COUNT; i++) if (letterMask(i) == mask) return i;
  return -1;
}

void sayDotNumbers(uint8_t mask) {
  say(TRK_DOTS);
  for (uint8_t i = 0; i < 6; i++) if (mask & (1 << i)) say(TRK_NUM(i + 1));
}

void askLetter() {
  say(TRK_TASK);
  say(TRK_LETTER(letterIdx));
  attempts = 0;
  state = ST_INPUT;
}

void nextLetter() {
  letterIdx++;
  if (letterIdx >= LETTER_COUNT) {
    letterIdx = 0;
    speechReset();
    say(TRK_DONE);
  }
  markDirty();
  askLetter();
}

void doCheck() {
  uint8_t m = dotsMask();
  uint8_t target = letterMask(letterIdx);

  if (m == 0) {                       // ничего не набрано
    speechReset();
    say(TRK_EMPTY);
    say(TRK_TASK); say(TRK_LETTER(letterIdx));
    return;
  }

  if (m == target) {                  // ВЕРНО
    speechReset();
    say(TRK_OK);
    say(TRK_LETTER(letterIdx));       // закрепляем: «Верно. А.»
    say(TRK_RELEASE);
    attempts = 0;
    state = ST_RELEASE;
    return;
  }

  attempts++;
  speechReset();
  say(TRK_BAD);

  if (attempts >= GIVE_UP_AFTER) {    // сдаёмся и учим
    say(TRK_ANSWER);
    say(TRK_LETTER(letterIdx));
    sayDotNumbers(target);
    say(TRK_RELEASE);
    state = ST_RELEASE;
  } else {                            // есть ещё попытка
    int8_t typed = findLetter(m);
    if (typed >= 0) { say(TRK_TYPED); say(TRK_LETTER(typed)); }
    say(TRK_TRY_AGAIN);
    say(TRK_TASK); say(TRK_LETTER(letterIdx));
  }
}

void doRepeat() {
  speechReset();
  say(TRK_TASK); say(TRK_LETTER(letterIdx));
}

void doHint() {
  speechReset();
  say(TRK_HINT);
  sayDotNumbers(letterMask(letterIdx));
}

void announceMode() {
  speechReset();
  say(TRK_MODE_WORD);
  say(TRK_NUM(mode));
  if (mode == 1) say(TRK_MODE_1);
  else say(TRK_MODE_SOON);
}

void switchMode() {
  mode = (mode % MODE_COUNT) + 1;
  markDirty();
  announceMode();
  if (mode == 1) {
    if (dotsMask() != 0) { say(TRK_RELEASE); state = ST_CLEAR; }
    else askLetter();
  } else state = ST_STUB;
}

void changeVolume(int8_t dir) {
  int16_t v = (int16_t)volume + dir * VOL_STEP;
  v = constrain(v, VOL_MIN, VOL_MAX);
  bool changed = (v != volume);
  volume = (uint8_t)v;
  dfSetVolume(volume);
  markDirty();
  speechReset();
  if (!changed) say(TRK_VOLUME);                 // упёрлись в предел — поясняем
  say(TRK_NUM(volume));                          // коротко: «двадцать»
  if (mode == 1) {                               // и возвращаем задание, чтобы не потерялось
    if (state == ST_INPUT) { say(TRK_TASK); say(TRK_LETTER(letterIdx)); }
    else if (state == ST_RELEASE || state == ST_CLEAR) say(TRK_RELEASE);
  }
}

/* ============================== SETUP ============================== */
void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BUSY, INPUT);          // BUSY — обычный логический выход модуля

  for (uint8_t i = 0; i < 6; i++) {
    pinMode(PIN_DOT[i], INPUT_PULLUP);
    dotRaw[i] = dotState[i] = (digitalRead(PIN_DOT[i]) == LOW);
    dotTRaw[i] = millis();
  }
  initButton(bEnter); initButton(bMode); initButton(bUp); initButton(bDn);

  loadSettings();

  dfSerial.begin(9600);
  delay(1500);                       // модулю нужно время на старт и чтение карты
  dfCmd(0x09, 0x02);                 // источник = TF-карта
  delay(200);
  dfSetVolume(volume);
  delay(200);

  speechReset();
  say(TRK_HELLO);
  if (mode == 1) {
    say(TRK_MODE_WORD); say(TRK_NUM(1)); say(TRK_MODE_1);
    if (dotsMask() != 0) { say(TRK_RELEASE); state = ST_CLEAR; }   // точки остались нажатыми
    else { say(TRK_TASK); say(TRK_LETTER(letterIdx)); state = ST_INPUT; }
  } else {
    say(TRK_MODE_WORD); say(TRK_NUM(mode)); say(TRK_MODE_SOON); state = ST_STUB;
  }
}

/* =============================== LOOP ============================== */
void loop() {
  speechTask();
  readDotsTask();
  saveTask();

  /* --- «Проверка»: короткое = проверить, длинное = повтор / подсказка --- */
  uint8_t eE = updateButton(bEnter);   // на нажатие не реагируем: ждём клик или удержание
  if (bEnter.state && !bEnter.longDone && millis() - bEnter.tPress >= ENTER_LONG_MS) {
    bEnter.longDone = true; evEnterLong = true;
  }
  if (bEnter.state && bEnter.longDone && !bEnter.hintDone && millis() - bEnter.tPress >= ENTER_HINT_MS) {
    bEnter.hintDone = true; evEnterHint = true;
  }
  if ((eE & 2) && !bEnter.longDone) evEnterShort = true;

  /* --- «Режим»: короткое = назвать текущий, длинное = переключить --- */
  uint8_t eM = updateButton(bMode);
  if (eM & 1) evModeShort = true;                       // отзовёмся сразу на нажатие
  if (bMode.state && !bMode.longDone && millis() - bMode.tPress >= MODE_LONG_MS) {
    bMode.longDone = true; evModeLong = true; evModeShort = false;
  }

  /* --- Громкость: клик + автоповтор при удержании --- */
  uint8_t eU = updateButton(bUp);
  uint8_t eD = updateButton(bDn);
  if (eU & 1) evVolUp = true;
  if (eD & 1) evVolDn = true;
  if (bUp.state && millis() - bUp.tPress > VOL_HOLD_MS && millis() - bUp.tRepeat > VOL_REPEAT_MS) {
    bUp.tRepeat = millis(); evVolUp = true;
  }
  if (bDn.state && millis() - bDn.tPress > VOL_HOLD_MS && millis() - bDn.tRepeat > VOL_REPEAT_MS) {
    bDn.tRepeat = millis(); evVolDn = true;
  }

  /* ---------------------- обработка событий ---------------------- */
  if (evVolUp) { evVolUp = false; changeVolume(+1); }
  if (evVolDn) { evVolDn = false; changeVolume(-1); }

  if (evModeLong)  { evModeLong = false; evModeShort = false; switchMode(); }
  else if (evModeShort) { evModeShort = false; announceMode(); }

  if (mode == 1) {
    if (evEnterHint)      { evEnterHint = false; evEnterLong = false; doHint(); }
    else if (evEnterLong) { evEnterLong = false; doRepeat(); }

    if (evEnterShort) {
      evEnterShort = false;
      if (state == ST_INPUT) doCheck();
      else { speechReset(); say(TRK_RELEASE); }
    }

    // после правильного ответа / разбора ошибки ждём, пока снимут все точки
    if (state == ST_RELEASE && allDotsReleased() && speechIdle()) nextLetter();
    // старт с зажатыми точками: ждём освобождения и повторяем ТО ЖЕ задание
    if (state == ST_CLEAR && allDotsReleased() && speechIdle()) askLetter();

#if SPEAK_DOT_NUMBER
    if (evDotChanged >= 0 && dotState[evDotChanged] && state == ST_INPUT) {
      speechReset(); say(TRK_NUM(evDotChanged + 1));
    }
#endif
  } else {
    // заглушки режимов 2/3: кнопка «Проверка» просто напоминает, где мы
    if (evEnterShort || evEnterLong || evEnterHint) {
      evEnterShort = evEnterLong = evEnterHint = false;
      announceMode();
    }
  }
  evDotChanged = -1;

  digitalWrite(PIN_LED, spkActive ? HIGH : LOW);   // мигает, пока говорит
}
