#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <DHT.h>
#include <LittleFS.h>
#include <time.h>
#include "esp_sntp.h"

// ====== НАСТРОЙКИ — поменяйте под себя ======

#define FW_VERSION 15          // версия ЭТОЙ прошивки

const char* WIFI_SSID = "CPE-A3AE";
//const char* WIFI_PASS = "neverever2332";
const char* WIFI_PASS = "1234567890";

const char* VERSION_URL  = "https://raw.githubusercontent.com/zakirovandrey/parkent/main/firmware/version.txt";
const char* FIRMWARE_URL = "https://raw.githubusercontent.com/zakirovandrey/parkent/main/firmware/firmware.bin";

// При первом запуске (пустая память) плата просто запоминает, какой файл
// сейчас лежит на сервере, и не перепрошивается. Поставьте false, если
// хотите, чтобы она сразу подтянула то, что на GitHub.
const bool SKIP_FIRST_UPDATE = true;

const char* BOT_TOKEN = "8635862750:AAFtNBZxm5Q9S2GW93PJi619r4RivaF3aos";

// Все перечисленные чаты равноправны: любой может включать и выключать насос.
// Пока OPEN_ACCESS = true, бот слушает вообще всех — это режим настройки,
// нужный чтобы узнать свои chat_id. Обязательно поставьте false, когда
// впишете реальные номера.
const bool OPEN_ACCESS = true;
const long ALLOWED_CHATS[] = { 128749596, 222222222, 333333333 };
const int  ALLOWED_COUNT   = sizeof(ALLOWED_CHATS) / sizeof(ALLOWED_CHATS[0]);

// Одно реле на насос, каждое управляется само по себе.
// Пины 26/25/33/32 у ESP32-WROOM-32 свободны: не участвуют в загрузке,
// не заняты флешем и умеют работать выходом.
const int   RELAY_PINS[]    = { 26, 25, 33, 32 };
const char* RELAY_NAMES[]   = { "Насос 1", "Насос 2", "Насос 3", "Насос 4" };
const int   RELAY_COUNT     = sizeof(RELAY_PINS) / sizeof(RELAY_PINS[0]);
const bool  RELAY_ACTIVE_LOW = true;

const unsigned long PUMP_DEFAULT_MIN = 15;
const unsigned long PUMP_MAX_MIN     = 120;
const bool          RESTORE_ON_BOOT  = false;

const unsigned long CHECK_INTERVAL_MS = 300000;
const unsigned long WIFI_TIMEOUT_MS   = 20000;
const unsigned long ERROR_RETRY_MS    = 15000;
const int           POLL_TIMEOUT_S    = 20;

const int LED_PIN = 2;

const int DHT_PIN  = 27;
#define   DHT_TYPE DHT11

// --- Телеметрия ---
// Датчик опрашивает отдельная задача FreeRTOS (telemetryTask), в сеть она
// не ходит. Частый опрос нужен статистике: DHT11 отдаёт целые числа,
// и усреднение ~30 отсчётов за бакет даёт эффективное разрешение ~0.2 °C.
const uint32_t TM_SAMPLE_MS     = 60000;   // период опроса датчика
const int      BUCKET_MIN      = 30;       // минут в бакете; только делители часа: 20 или 30
const uint16_t BUCKET_CAPACITY = 2880;     // глубина кольца: 60 дней получасовых бакетов

// Группы Telegram для графиков и алертов. Обязательно строками: у каналов
// и новых групп id не влезает в 32-битный long этой платы. Бот должен быть
// участником каждой группы. Если Telegram мигрирует группу в супергруппу,
// id сменится (в Serial будет ошибка sendPhoto) — впишите новый.
const char* CH_DAILY   = "-4451281939";   // суточный график, ежедневно 07:00
const char* CH_WEEKLY  = "-4339059313";   // недельный, вс 20:00 (этап 4)
const char* CH_MONTHLY = "-4320741154";   // месячный, 1-го 09:00 (этап 4)
const char* CH_ALERTS  = "-4253212238";   // алерты (этап 4)

// Диагностика: показывать chat_id прямо в панели. Поставьте false, когда
// разберётесь с номерами чатов.
const bool SHOW_CHAT_ID = true;

// =====================================================================
//  СВЕТОДИОД
// =====================================================================

enum LedMode { LED_BLINK, LED_BREATHE, LED_SOLID };

struct LedEffect {
  LedMode         mode;
  const uint16_t* steps;
  uint8_t         len;
  uint16_t        period;
};

#define BLINK_FX(p)    { LED_BLINK,   p,       sizeof(p) / sizeof(p[0]), 0  }
#define BREATHE_FX(ms) { LED_BREATHE, nullptr, 0,                        ms }

const uint16_t PAT_CONNECTING[] = {100, 100};
const uint16_t PAT_ONLINE[]     = {60, 1940};
const uint16_t PAT_ERROR[]      = {100, 150, 100, 1000};

const LedEffect FX_CONNECTING = BLINK_FX(PAT_CONNECTING);
const LedEffect FX_ONLINE     = BLINK_FX(PAT_ONLINE);
const LedEffect FX_ERROR      = BLINK_FX(PAT_ERROR);
const LedEffect FX_CHECKING   = BREATHE_FX(2000);
const LedEffect FX_UPDATING   = BREATHE_FX(700);
const LedEffect FX_PUMP_ON    = { LED_SOLID, nullptr, 0, 0 };

const LedEffect* fx      = &FX_CONNECTING;
unsigned long    fxStart = 0;
uint8_t          fxStep  = 0;

Preferences prefs;

void ledSet(uint8_t duty) {
  ledcWrite(LED_PIN, duty);
}

void ledBegin() {
  ledcAttach(LED_PIN, 5000, 8);       // пин, 5 кГц, 8 бит (0..255)
  ledSet(0);
}

void setLedEffect(const LedEffect* effect) {
  if (fx == effect) return;
  fx      = effect;
  fxStart = millis();
  fxStep  = 0;
  if (effect->mode == LED_BLINK || effect->mode == LED_SOLID) ledSet(255);
}

void updateLed() {
  switch (fx->mode) {
    case LED_SOLID:
      ledSet(255);
      break;

    case LED_BLINK:
      if (millis() - fxStart >= fx->steps[fxStep]) {
        fxStart = millis();
        fxStep  = (fxStep + 1) % fx->len;
        ledSet((fxStep % 2 == 0) ? 255 : 0);
      }
      break;

    case LED_BREATHE: {
      uint32_t t     = (millis() - fxStart) % fx->period;
      float    phase = (float)t / (float)fx->period;
      float    b     = (1.0f - cosf(2.0f * PI * phase)) * 0.5f;
      ledSet((uint8_t)(b * b * 255.0f));
      break;
    }
  }
}

void ledTask(void* param) {
  for (;;) {
    updateLed();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =====================================================================
//  ДАТЧИК DHT11
// =====================================================================

DHT dht(DHT_PIN, DHT_TYPE);

float         sensorTemp = NAN;
float         sensorHum  = NAN;
unsigned long sensorTime = 0;   // millis последнего УДАЧНОГО чтения

// Абстракция над железом: сейчас DHT11, позже AHT20+BMP280 или SHT31 на I2C —
// заменять придётся только эту функцию. Возвращает false, если датчик
// не ответил или отдал мусор. Сбой контрольной суммы при исправной схеме —
// обычное дело, поэтому две попытки. RH около нуля — тоже отказ: диапазон
// DHT11 начинается с 20 %, а на RH = 0 взорвётся ln() в точке росы.
// Вызывается только из telemetryTask, библиотека DHT не потокобезопасна.
bool readSensor(float& t, float& rh) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt) vTaskDelay(pdMS_TO_TICKS(2200));   // библиотека кэширует результат 2 секунды

    float h  = dht.readHumidity();
    float tc = dht.readTemperature();

    if (!isnan(h) && !isnan(tc) &&
        h > 0.5f && h <= 100.0f && tc > -45.0f && tc < 85.0f) {
      t  = tc;
      rh = h;
      return true;
    }
  }
  return false;
}

String sensorLines() {
  if (sensorTime == 0) return "Датчик: нет данных";

  String s = "Температура: " + String(sensorTemp, 0) + " C\n";
  s += "Влажность: " + String(sensorHum, 0) + " %";

  unsigned long ageMin = (millis() - sensorTime) / 60000;
  if (ageMin >= 5) s += "\n(данные " + String(ageMin) + " мин назад)";
  return s;
}

// =====================================================================
//  ТЕЛЕМЕТРИЯ: часы, бакеты, кольцо на LittleFS
// =====================================================================
//
// Опрос датчика и запись на флеш живут в telemetryTask на ядре 1:
// DHT11 читается битбэнгом с запретом прерываний, а на ядре 0 — стек WiFi.
// В сеть задача не ходит вообще — вся сетевая работа остаётся в loop(),
// чтобы не появилось второй одновременной TLS-сессии.

static_assert(60 % BUCKET_MIN == 0, "BUCKET_MIN должен делить час");

const char* TM_BUCKETS_PATH = "/tm/buckets.bin";
const char* TM_STATE_PATH   = "/tm/state.bin";
const char* TM_DAILY_PATH   = "/tm/daily.bin";

// Часы считаем валидными, если время правдоподобно. RTC переживает мягкую
// перезагрузку, поэтому после OTA бакеты идут сразу; после пропажи питания —
// только когда NTP синхронизируется заново.
const time_t TM_TIME_VALID = 1700000000;

// Получасовой агрегат. Храним только измеренное: точка росы и VPD считаются
// при отрисовке — дешевле по флешу, и формулы можно поправить задним числом.
// TM_NOVAL во всех полях — ни одного валидного отсчёта за бакет.
struct __attribute__((packed)) Bucket {
  uint32_t ts;       // начало бакета, UTC unix
  int16_t  t_min;    // °C × 100
  int16_t  t_max;
  int16_t  t_avg;
  int16_t  rh_min;   // % × 100
  int16_t  rh_max;
  int16_t  rh_avg;
  uint8_t  n;        // валидных отсчётов
  uint8_t  n_fail;   // неудачных чтений
};
static_assert(sizeof(Bucket) == 18, "Bucket должен занимать 18 байт");

// Суточная свёртка, дописывается в /tm/daily.bin заданием планировщика
// в 00:05. Точку росы и VPD по бакетам не храним — считаем при отрисовке.
struct __attribute__((packed)) DayRec {
  uint32_t day;       // локальная полночь, unix
  int16_t  t_min;     // °C × 100
  int16_t  t_max;
  int16_t  t_avg;
  uint16_t t_min_at;  // минут от локальной полуночи (начало бакета)
  uint16_t t_max_at;
  int16_t  rh_min;    // % × 100
  int16_t  rh_avg;
  uint16_t vpd_hsum;  // Σ(VPD × час) × 100, кПа·ч
  uint16_t gdd10;     // градусо-дни, база 10 °C, × 100
};
static_assert(sizeof(DayRec) == 22, "DayRec должен занимать 22 байта");

// Явные прототипы: автогенератор Arduino вставляет свои выше объявлений
// структур, и Bucket/DayRec в них ещё не видны.
void tmWriteBucketLocked(const Bucket& b);
int  tmReadLastBuckets(Bucket* out, int want);
int  tmReadBucketsRange(uint32_t from, uint32_t to, Bucket* out, int maxN);
bool tmReadOldestBucket(Bucket& out);
int  tmReadLastDays(DayRec* out, int want);

const int16_t TM_NOVAL = INT16_MIN;

// Голова кольца и время последних заданий планировщика — в отдельном файле,
// чтобы buckets.bin только перезаписывался по месту и никогда не рос.
const uint32_t TM_STATE_MAGIC = 0x314D5354;   // "TSM1"

struct __attribute__((packed)) TmState {
  uint32_t magic;
  uint16_t bucketHead;   // куда писать следующий бакет
  uint16_t bucketUsed;   // сколько записей кольца валидны
  uint32_t jobLast[8];   // unix последних успешных запусков заданий (этапы 2–4)
};

// Копящийся бакет. Живёт в RAM: при потере питания пропадает не больше
// получаса. Закрытые бакеты уходят на флеш сразу, без буферизации.
struct TmAccum {
  uint32_t bucketStart = 0;   // 0 — бакет не начат
  float    tMin = 0, tMax = 0, tSum = 0;
  float    rhMin = 0, rhMax = 0, rhSum = 0;
  uint16_t n = 0, nFail = 0;
};

TmState tmState = {};
TmAccum tmAcc;
bool    tmFsOk = false;

// Мьютекс закрывает tmState, tmAcc и файлы кольца: задача пишет,
// команды бота читают из loop().
SemaphoreHandle_t tmMutex = nullptr;

volatile uint32_t tmTotalOk      = 0;   // счётчики чтений с загрузки
volatile uint32_t tmTotalFail    = 0;
volatile uint32_t tmLastSampleMs = 0;   // millis конца последнего цикла опроса
volatile bool     tmTimeSynced   = false;   // часы валидны (NTP или RTC после перезагрузки)
volatile bool     tmNtpSynced    = false;   // NTP реально ответил с этой загрузки

// ts последнего записанного бакета: кольцо держим строго возрастающим,
// чтобы шаг часов (SNTP после долгого офлайна) не породил записей задним
// числом. Читается/пишется под tmMutex.
uint32_t tmLastWrittenTs = 0;

// Вызывается из контекста SNTP при каждом успешном ответе сервера времени.
void tmOnNtpSync(struct timeval* tv) {
  tmNtpSynced = true;
}

// В файлах всегда UTC unix; локальное время — только для отображения.
void tmFmtLocal(time_t ts, const char* fmt, char* buf, size_t len) {
  struct tm tmv;
  localtime_r(&ts, &tmv);
  strftime(buf, len, fmt, &tmv);
}

void tmSaveStateLocked() {
  File f = LittleFS.open(TM_STATE_PATH, "w");
  if (!f) { Serial.println("TM: не могу записать state.bin"); return; }
  f.write((const uint8_t*)&tmState, sizeof(tmState));
  f.close();
}

bool tmCreateBucketsFile() {
  File f = LittleFS.open(TM_BUCKETS_PATH, "w");
  if (!f) return false;

  uint8_t zeros[16 * sizeof(Bucket)] = {};
  size_t  left = (size_t)BUCKET_CAPACITY * sizeof(Bucket);
  while (left) {
    size_t chunk = left < sizeof(zeros) ? left : sizeof(zeros);
    if (f.write(zeros, chunk) != chunk) { f.close(); return false; }
    left -= chunk;
  }
  f.close();
  return true;
}

// Монтирует LittleFS и чинит хранилище, если оно битое. Кольцо создаётся
// сразу полной длины и дальше только перезаписывается по месту.
void tmFsInit() {
  if (!LittleFS.begin(true)) {   // true: форматировать при самом первом запуске
    Serial.println("TM: LittleFS не смонтировался — бакеты будут только в Serial");
    return;
  }
  LittleFS.mkdir("/tm");

  const size_t need = (size_t)BUCKET_CAPACITY * sizeof(Bucket);
  File   f    = LittleFS.open(TM_BUCKETS_PATH, "r");
  size_t have = f ? f.size() : 0;
  if (f) f.close();

  bool fresh = (have != need);
  if (fresh) {
    if (have) Serial.printf("TM: buckets.bin битый (%u байт вместо %u), пересоздаю\n",
                            (unsigned)have, (unsigned)need);
    if (!tmCreateBucketsFile()) {
      Serial.println("TM: не удалось создать buckets.bin");
      return;
    }
  }

  bool stateOk = false;
  f = LittleFS.open(TM_STATE_PATH, "r");
  if (f && f.size() == sizeof(TmState) &&
      f.read((uint8_t*)&tmState, sizeof(tmState)) == sizeof(tmState) &&
      tmState.magic == TM_STATE_MAGIC &&
      tmState.bucketHead < BUCKET_CAPACITY &&
      tmState.bucketUsed <= BUCKET_CAPACITY)
    stateOk = true;
  if (f) f.close();

  if (fresh || !stateOk) {
    if (!fresh) Serial.println("TM: state.bin битый, начинаю кольцо заново");
    tmState = {};
    tmState.magic = TM_STATE_MAGIC;
    tmSaveStateLocked();
  }

  // daily.bin — append-only; хвост неполной записи (обрыв питания на дозаписи)
  // усекаем до целых записей, не теряя накопленную историю.
  LittleFS.remove("/tm/daily.tmp");   // огрызок незавершённой прошлой починки
  f = LittleFS.open(TM_DAILY_PATH, "r");
  if (f) {
    size_t sz   = f.size();
    size_t keep = sz - sz % sizeof(DayRec);
    if (keep != sz) {
      Serial.printf("TM: daily.bin битый (%u байт), усекаю до %u\n",
                    (unsigned)sz, (unsigned)keep);
      File t = LittleFS.open("/tm/daily.tmp", "w");
      uint8_t buf[6 * sizeof(DayRec)];
      size_t  left = keep;
      bool    ok   = (bool)t;
      while (ok && left) {
        size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
        ok = f.read(buf, chunk) == chunk && t.write(buf, chunk) == chunk;
        left -= chunk;
      }
      f.close();
      if (t) t.close();
      if (ok) {
        // rename поверх существующего файла у LittleFS атомарный:
        // обрыв питания не оставит нас вовсе без daily.bin
        LittleFS.rename("/tm/daily.tmp", TM_DAILY_PATH);
      } else {
        LittleFS.remove("/tm/daily.tmp");
        Serial.println("TM: усечь daily.bin не удалось, оставил как есть");
      }
    } else {
      f.close();
    }
  }

  // Восстанавливаем ts последней записи — от него продолжится монотонность.
  if (tmState.bucketUsed) {
    f = LittleFS.open(TM_BUCKETS_PATH, "r");
    if (f) {
      uint16_t last = (tmState.bucketHead + BUCKET_CAPACITY - 1) % BUCKET_CAPACITY;
      Bucket   b;
      if (f.seek((uint32_t)last * sizeof(Bucket)) &&
          f.read((uint8_t*)&b, sizeof(b)) == sizeof(b))
        tmLastWrittenTs = b.ts;
      f.close();
    }
  }

  tmFsOk = true;
  Serial.printf("TM: хранилище готово: %u/%u бакетов, голова %u, FS %u/%u КБ\n",
                tmState.bucketUsed, BUCKET_CAPACITY, tmState.bucketHead,
                (unsigned)(LittleFS.usedBytes() / 1024),
                (unsigned)(LittleFS.totalBytes() / 1024));
}

// Пишет закрытый бакет в голову кольца. Вызывать под мьютексом.
void tmWriteBucketLocked(const Bucket& b) {
  if (!tmFsOk) return;

  File f = LittleFS.open(TM_BUCKETS_PATH, "r+");   // r+: запись по месту, без усечения
  if (!f) { Serial.println("TM: buckets.bin не открылся"); return; }
  bool ok = f.seek((uint32_t)tmState.bucketHead * sizeof(Bucket)) &&
            f.write((const uint8_t*)&b, sizeof(b)) == sizeof(b);
  f.close();
  if (!ok) { Serial.println("TM: запись бакета не удалась"); return; }

  // Сначала данные, потом голова: если питание пропадёт между ними,
  // следующий бакет просто перепишет тот же слот.
  tmState.bucketHead = (tmState.bucketHead + 1) % BUCKET_CAPACITY;
  if (tmState.bucketUsed < BUCKET_CAPACITY) tmState.bucketUsed++;
  tmLastWrittenTs = b.ts;
  tmSaveStateLocked();
}

int16_t tmScale100(float v) { return (int16_t)lroundf(v * 100.0f); }

// Сворачивает tmAcc в запись и отправляет на флеш. Вызывать под мьютексом.
void tmCloseBucketLocked() {
  Bucket b = {};
  b.ts = tmAcc.bucketStart;
  if (tmAcc.n) {
    b.t_min  = tmScale100(tmAcc.tMin);
    b.t_max  = tmScale100(tmAcc.tMax);
    b.t_avg  = tmScale100(tmAcc.tSum / tmAcc.n);
    b.rh_min = tmScale100(tmAcc.rhMin);
    b.rh_max = tmScale100(tmAcc.rhMax);
    b.rh_avg = tmScale100(tmAcc.rhSum / tmAcc.n);
  } else {
    b.t_min = b.t_max = b.t_avg = b.rh_min = b.rh_max = b.rh_avg = TM_NOVAL;
  }
  b.n      = tmAcc.n     > 255 ? 255 : tmAcc.n;
  b.n_fail = tmAcc.nFail > 255 ? 255 : tmAcc.nFail;

  tmWriteBucketLocked(b);

  char when[16];
  tmFmtLocal((time_t)b.ts, "%d.%m %H:%M", when, sizeof(when));
  if (tmAcc.n)
    Serial.printf("TM: бакет %s закрыт: T %.1f/%.1f/%.1f, RH %.0f/%.0f/%.0f, n=%u, отказов %u\n",
                  when, tmAcc.tMin, tmAcc.tSum / tmAcc.n, tmAcc.tMax,
                  tmAcc.rhMin, tmAcc.rhSum / tmAcc.n, tmAcc.rhMax, b.n, b.n_fail);
  else
    Serial.printf("TM: бакет %s закрыт пустым, отказов %u\n", when, b.n_fail);
}

// Один цикл опроса: чтение датчика, обновление панели, учёт в бакете.
void tmSampleOnce() {
  float t = NAN, rh = NAN;
  bool  ok = readSensor(t, rh);

  if (ok) {
    sensorTemp = t;             // на этих глобальных живёт текст панели
    sensorHum  = rh;
    sensorTime = millis();
    tmTotalOk  = tmTotalOk + 1;
    Serial.printf("TM: %.1f C, %.1f %%\n", t, rh);
  } else {
    tmTotalFail = tmTotalFail + 1;
    Serial.println("TM: датчик не ответил");
  }
  tmLastSampleMs = millis();

  time_t now = time(nullptr);
  if (now < TM_TIME_VALID) return;   // без стенных часов бакеты не выровнять

  if (!tmTimeSynced) {
    tmTimeSynced = true;
    char buf[24];
    tmFmtLocal(now, "%d.%m.%Y %H:%M:%S", buf, sizeof(buf));
    Serial.printf("TM: часы валидны: %s\n", buf);
  }

  // Выравнивание по стенным часам. Пояс +5 — целое число часов, а BUCKET_MIN
  // делит час, поэтому границы бакетов в UTC и в локальном времени совпадают.
  uint32_t bStart = (uint32_t)(now - now % (BUCKET_MIN * 60));

  xSemaphoreTake(tmMutex, portMAX_DELAY);
  if (tmAcc.bucketStart) {
    if (bStart < tmAcc.bucketStart) {
      // Часы шагнули назад через границу (SNTP после долгого офлайна).
      // Метки копившихся отсчётов стали недостоверны — выбрасываем.
      Serial.println("TM: часы шагнули назад, копившийся бакет отброшен");
      tmAcc = TmAccum();
    } else if (bStart > tmAcc.bucketStart) {
      tmCloseBucketLocked();
      tmAcc = TmAccum();
    }
  }
  if (!tmAcc.bucketStart) {
    // Кольцо строго возрастает по ts: бакет не старше уже записанного.
    if (bStart <= tmLastWrittenTs) {
      xSemaphoreGive(tmMutex);
      return;   // ждём, пока часы дойдут до незанятой границы
    }
    tmAcc.bucketStart = bStart;
  }

  if (ok) {
    if (!tmAcc.n) {
      tmAcc.tMin  = tmAcc.tMax  = t;
      tmAcc.rhMin = tmAcc.rhMax = rh;
    } else {
      if (t  < tmAcc.tMin)  tmAcc.tMin  = t;
      if (t  > tmAcc.tMax)  tmAcc.tMax  = t;
      if (rh < tmAcc.rhMin) tmAcc.rhMin = rh;
      if (rh > tmAcc.rhMax) tmAcc.rhMax = rh;
    }
    tmAcc.tSum  += t;
    tmAcc.rhSum += rh;
    tmAcc.n++;
  } else {
    tmAcc.nFail++;
  }
  xSemaphoreGive(tmMutex);
}

void telemetryTask(void* param) {
  // Страховка удалённой прошивки: если прошлый запуск кончился паникой или
  // вотчдогом, придерживаем опрос — loop() успеет дойти до checkForUpdate()
  // и забрать исправленную прошивку раньше, чем упадём снова.
  esp_reset_reason_t why = esp_reset_reason();
  if (why == ESP_RST_PANIC || why == ESP_RST_TASK_WDT ||
      why == ESP_RST_INT_WDT || why == ESP_RST_WDT) {
    Serial.println("TM: прошлая загрузка упала — опрос отложен на 3 минуты");
    vTaskDelay(pdMS_TO_TICKS(180000));
  }

  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    tmSampleOnce();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(TM_SAMPLE_MS));
  }
}

void tmInit() {
  tmMutex = xSemaphoreCreateMutex();
  tmFsInit();
  xTaskCreatePinnedToCore(telemetryTask, "telemetry", 4096, NULL, 1, NULL, 1);
}

// Последние want бакетов в out, от старых к новым. Возвращает сколько прочитал.
int tmReadLastBuckets(Bucket* out, int want) {
  if (!tmFsOk) return 0;

  xSemaphoreTake(tmMutex, portMAX_DELAY);
  int  n = (int)tmState.bucketUsed < want ? tmState.bucketUsed : want;
  File f = LittleFS.open(TM_BUCKETS_PATH, "r");
  if (!f) n = 0;
  for (int k = 0; k < n; k++) {
    int idx = (tmState.bucketHead + BUCKET_CAPACITY - n + k) % BUCKET_CAPACITY;
    if (!f.seek((uint32_t)idx * sizeof(Bucket)) ||
        f.read((uint8_t*)&out[k], sizeof(Bucket)) != sizeof(Bucket)) {
      n = k;
      break;
    }
  }
  if (f) f.close();
  xSemaphoreGive(tmMutex);
  return n;
}

// Бакеты с ts в [from, to), от старых к новым. Проход по всему кольцу:
// 52 КБ последовательного чтения раз в сутки — дешевле, чем держать
// индекс. Кольцо строго возрастает по ts, поэтому дальше to не читаем.
int tmReadBucketsRange(uint32_t from, uint32_t to, Bucket* out, int maxN) {
  if (!tmFsOk) return 0;

  xSemaphoreTake(tmMutex, portMAX_DELAY);
  int  n = 0;
  File f = LittleFS.open(TM_BUCKETS_PATH, "r");
  if (f) {
    for (int k = 0; k < tmState.bucketUsed && n < maxN; k++) {
      int idx = (tmState.bucketHead + BUCKET_CAPACITY - tmState.bucketUsed + k)
                % BUCKET_CAPACITY;
      Bucket b;
      if (!f.seek((uint32_t)idx * sizeof(Bucket)) ||
          f.read((uint8_t*)&b, sizeof(b)) != sizeof(b)) break;
      if (b.ts >= to) break;
      if (b.ts >= from) out[n++] = b;
    }
    f.close();
  }
  xSemaphoreGive(tmMutex);
  return n;
}

// Самый старый бакет кольца.
bool tmReadOldestBucket(Bucket& out) {
  if (!tmFsOk) return false;

  xSemaphoreTake(tmMutex, portMAX_DELAY);
  bool ok = false;
  if (tmState.bucketUsed) {
    File f = LittleFS.open(TM_BUCKETS_PATH, "r");
    if (f) {
      int idx = (tmState.bucketHead + BUCKET_CAPACITY - tmState.bucketUsed)
                % BUCKET_CAPACITY;
      ok = f.seek((uint32_t)idx * sizeof(Bucket)) &&
           f.read((uint8_t*)&out, sizeof(Bucket)) == sizeof(Bucket);
      f.close();
    }
  }
  xSemaphoreGive(tmMutex);
  return ok;
}

// Последние want суточных записей из daily.bin, от старых к новым.
int tmReadLastDays(DayRec* out, int want) {
  if (!tmFsOk) return 0;

  xSemaphoreTake(tmMutex, portMAX_DELAY);
  int  n = 0;
  File f = LittleFS.open(TM_DAILY_PATH, "r");
  if (f) {
    int total = f.size() / sizeof(DayRec);
    n = total < want ? total : want;
    if (n > 0 &&
        (!f.seek((uint32_t)(total - n) * sizeof(DayRec)) ||
         f.read((uint8_t*)out, n * sizeof(DayRec)) != n * sizeof(DayRec)))
      n = 0;
    f.close();
  }
  xSemaphoreGive(tmMutex);
  return n;
}

// =====================================================================
//  ТЕЛЕМЕТРИЯ: формулы (в файлах только измеренное, производное — здесь)
// =====================================================================

// Давление насыщенного пара, кПа
float tmEs(float t) { return 0.6108f * expf(17.27f * t / (t + 237.3f)); }

// Дефицит давления пара, кПа
float tmVpd(float t, float rh) { return tmEs(t) * (1.0f - rh / 100.0f); }

// Точка росы, °C. RH зажимаем снизу: DHT11 при отказе отдаёт нули,
// и ln(0) на этом взрывается.
float tmDewpoint(float t, float rh) {
  if (rh < 1.0f) rh = 1.0f;
  float g = 17.27f * t / (237.7f + t) + logf(rh / 100.0f);
  return 237.7f * g / (17.27f - g);
}

// =====================================================================
//  ТЕЛЕМЕТРИЯ: суточная свёртка и суточный график
// =====================================================================

// Локальная полночь суток, в которые попадает ts.
time_t tmLocalMidnightOf(time_t ts) {
  struct tm lt;
  localtime_r(&ts, &lt);
  lt.tm_hour = 0;
  lt.tm_min  = 0;
  lt.tm_sec  = 0;
  return mktime(&lt);
}

time_t tmLocalMidnight() { return tmLocalMidnightOf(time(nullptr)); }

// Свёртка одних суток [from, from+86400) в DayRec с дозаписью в daily.bin.
// true = записал; false = данных за сутки нет или запись не удалась.
bool tmRollupOneDay(uint32_t from) {
  uint32_t to = from + 86400;

  static Bucket day[48];
  int got = tmReadBucketsRange(from, to, day, 48);

  float    tMin = 0, tMax = 0, rhMin = 0;
  uint32_t tMinAt = 0, tMaxAt = 0, nSum = 0;
  float    tSumW = 0, rhSumW = 0, vpdH = 0;
  bool     any = false;

  for (int i = 0; i < got; i++) {
    if (day[i].n == 0 || day[i].t_min == TM_NOVAL) continue;
    float bTmin = day[i].t_min / 100.0f, bTmax = day[i].t_max / 100.0f;
    float bTavg = day[i].t_avg / 100.0f;
    float bRmin = day[i].rh_min / 100.0f, bRavg = day[i].rh_avg / 100.0f;

    if (!any || bTmin < tMin)  { tMin = bTmin; tMinAt = (day[i].ts - from) / 60; }
    if (!any || bTmax > tMax)  { tMax = bTmax; tMaxAt = (day[i].ts - from) / 60; }
    if (!any || bRmin < rhMin) rhMin = bRmin;
    tSumW  += bTavg * day[i].n;
    rhSumW += bRavg * day[i].n;
    nSum   += day[i].n;
    vpdH   += tmVpd(bTavg, bRavg) * (BUCKET_MIN / 60.0f);
    any = true;
  }
  if (!any || !tmFsOk) return false;

  DayRec d = {};
  d.day      = from;
  d.t_min    = tmScale100(tMin);
  d.t_max    = tmScale100(tMax);
  d.t_avg    = tmScale100(tSumW / nSum);
  d.t_min_at = tMinAt;
  d.t_max_at = tMaxAt;
  d.rh_min   = tmScale100(rhMin);
  d.rh_avg   = tmScale100(rhSumW / nSum);
  float vh = vpdH * 100.0f;
  d.vpd_hsum = vh < 0 ? 0 : (vh > 65535.0f ? 65535 : (uint16_t)vh);
  float g = ((tMin + tMax) / 2.0f - 10.0f) * 100.0f;
  d.gdd10    = g < 0 ? 0 : (g > 65535.0f ? 65535 : (uint16_t)g);

  xSemaphoreTake(tmMutex, portMAX_DELAY);
  File f  = LittleFS.open(TM_DAILY_PATH, "a");
  bool ok = f && f.write((const uint8_t*)&d, sizeof(d)) == sizeof(d);
  if (f) f.close();
  xSemaphoreGive(tmMutex);

  char when[12];
  tmFmtLocal((time_t)from, "%d.%m", when, sizeof(when));
  Serial.printf("TM: свёртка за %s: T %.1f…%.1f, RH мин %.0f, GDD %.2f — %s\n",
                when, tMin, tMax, rhMin, d.gdd10 / 100.0f,
                ok ? "записана" : "ОШИБКА записи");
  return ok;
}

// Задание 00:05: досворачивает все дни от хвоста daily.bin до вчера
// включительно. После суток без питания дыры не образуется: пропущенные
// дни добираются из кольца (в нём 60 дней). Дни, когда плата была
// обесточена целиком, записей не получают, но прогрессу не мешают —
// следующий запуск продолжает с хвоста файла.
bool tmJobRollup() {
  if (!tmFsOk) return true;

  uint32_t today = (uint32_t)tmLocalMidnight();
  uint32_t next;

  DayRec tail;
  if (tmReadLastDays(&tail, 1) == 1) {
    next = tail.day + 86400;
  } else {
    Bucket oldest;
    if (!tmReadOldestBucket(oldest)) {
      Serial.println("TM: свёртка — кольцо пусто, нечего сворачивать");
      return true;
    }
    next = (uint32_t)tmLocalMidnightOf((time_t)oldest.ts);
  }

  int written = 0, guard = 0;
  for (; next < today && guard < 62; next += 86400, guard++)
    if (tmRollupOneDay(next)) written++;

  if (written) Serial.printf("TM: свёртка дописала дней: %d\n", written);
  return true;
}

// POST в QuickChart. Одна попытка, без ретраев: текст без графика лучше,
// чем цикл долбёжки. Своя короткоживущая TLS-сессия, последовательно
// с телеграмной — задачи в это не вмешиваются.
bool tmQuickChartCreate(JsonDocument& req, String& outUrl) {
  WiFiClientSecure client;
  client.setInsecure();          // осознанно, как везде в проекте

  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, "https://quickchart.io/chart/create")) return false;
  http.addHeader("Content-Type", "application/json");

  String body;
  serializeJson(req, body);
  int    code = http.POST(body);
  String resp = (code == HTTP_CODE_OK) ? http.getString() : String();
  http.end();

  if (code != HTTP_CODE_OK) {
    Serial.printf("TM: quickchart вернул %d\n", code);
    return false;
  }
  JsonDocument r;
  if (deserializeJson(r, resp) || !r["success"].as<bool>()) return false;
  outUrl = r["url"].as<const char*>();
  return outUrl.length() > 0;
}

// Суточный график: последние 48 закрытых бакетов. Температура и точка росы
// на левой оси с заливкой между ними (полоса схлопнулась — выпала роса),
// влажность бледной линией на правой. Возврат: 0 — данных нет;
// 1 — есть только текстовая сводка (QuickChart не дал ссылку); 2 — есть всё.
int tmBuildDailyChart(String& outUrl, String& caption) {
  static Bucket b[48];
  int got = tmReadLastBuckets(b, 48);
  if (got < 2) return 0;

  // --- сводка для подписи ---
  float    tMin = 0, tMax = 0, rhMin = 0, rhMax = 0;
  uint32_t tMinTs = 0, tMaxTs = 0, nSum = 0;
  float    tSumW = 0, rhSumW = 0;
  bool     any = false;

  for (int i = 0; i < got; i++) {
    if (b[i].n == 0 || b[i].t_min == TM_NOVAL) continue;
    float bTmin = b[i].t_min / 100.0f, bTmax = b[i].t_max / 100.0f;
    if (!any || bTmin < tMin) { tMin = bTmin; tMinTs = b[i].ts; }
    if (!any || bTmax > tMax) { tMax = bTmax; tMaxTs = b[i].ts; }
    if (!any || b[i].rh_min / 100.0f < rhMin) rhMin = b[i].rh_min / 100.0f;
    if (!any || b[i].rh_max / 100.0f > rhMax) rhMax = b[i].rh_max / 100.0f;
    tSumW  += b[i].t_avg / 100.0f * b[i].n;
    rhSumW += b[i].rh_avg / 100.0f * b[i].n;
    nSum   += b[i].n;
    any = true;
  }
  if (!any) return 0;

  char w1[16], w2[16], w3[16], w4[16];
  tmFmtLocal((time_t)b[0].ts, "%d.%m %H:%M", w1, sizeof(w1));
  tmFmtLocal((time_t)b[got - 1].ts + BUCKET_MIN * 60, "%d.%m %H:%M", w2, sizeof(w2));
  tmFmtLocal((time_t)tMinTs, "%H:%M", w3, sizeof(w3));
  tmFmtLocal((time_t)tMaxTs, "%H:%M", w4, sizeof(w4));

  caption  = String(w1) + " — " + w2 + "\n";
  caption += "T " + String(tMin, 1) + "…" + String(tMax, 1) + " °C (мин " + w3 +
             ", макс " + w4 + "), средняя " + String(tSumW / nSum, 1) +
             ", амплитуда " + String(tMax - tMin, 1) + "\n";
  caption += "RH " + String(rhMin, 0) + "…" + String(rhMax, 0) +
             " %, средняя " + String(rhSumW / nSum, 0) + " %";

  // --- конфиг Chart.js (v2, как ждёт QuickChart по умолчанию) ---
  JsonDocument req;
  req["width"]           = 900;
  req["height"]          = 450;
  req["backgroundColor"] = "white";

  JsonObject chart = req["chart"].to<JsonObject>();
  chart["type"] = "line";
  JsonObject data   = chart["data"].to<JsonObject>();
  JsonArray  labels = data["labels"].to<JsonArray>();
  JsonArray  ds     = data["datasets"].to<JsonArray>();

  JsonObject dTd = ds.add<JsonObject>();      // [0] точка росы
  dTd["label"]       = "Точка росы";
  dTd["borderColor"] = "#3b78c3";
  dTd["fill"]        = false;
  dTd["yAxisID"]     = "t";
  dTd["pointRadius"] = 0;
  dTd["borderWidth"] = 2;
  JsonArray aTd = dTd["data"].to<JsonArray>();

  JsonObject dT = ds.add<JsonObject>();       // [1] температура, заливка к росе
  dT["label"]           = "Температура";
  dT["borderColor"]     = "#d43d2a";
  dT["backgroundColor"] = "rgba(212,61,42,0.12)";
  dT["fill"]            = "-1";
  dT["yAxisID"]         = "t";
  dT["pointRadius"]     = 0;
  dT["borderWidth"]     = 2;
  JsonArray aT = dT["data"].to<JsonArray>();

  JsonObject dRh = ds.add<JsonObject>();      // [2] влажность, правая ось
  dRh["label"]       = "Влажность";
  dRh["borderColor"] = "rgba(110,145,190,0.55)";
  dRh["fill"]        = false;
  dRh["yAxisID"]     = "rh";
  dRh["pointRadius"] = 0;
  dRh["borderWidth"] = 1;
  JsonArray aRh = dRh["data"].to<JsonArray>();

  for (int i = 0; i < got; i++) {
    char lb[8];
    tmFmtLocal((time_t)b[i].ts, "%H:%M", lb, sizeof(lb));
    labels.add(lb);                           // char[] копируется, не указатель

    if (b[i].n == 0 || b[i].t_min == TM_NOVAL) {
      aTd.add(nullptr);                       // дыра в данных — разрыв линии
      aT.add(nullptr);
      aRh.add(nullptr);
    } else {
      float t  = b[i].t_avg / 100.0f;
      float rh = b[i].rh_avg / 100.0f;
      aTd.add(lroundf(tmDewpoint(t, rh) * 10) / 10.0f);
      aT.add(lroundf(t * 10) / 10.0f);
      aRh.add(lroundf(rh));
    }
  }

  JsonObject opt = chart["options"].to<JsonObject>();
  opt["title"]["display"]  = true;
  opt["title"]["text"]     = "Температура, точка росы и влажность — 24 ч";
  opt["legend"]["position"] = "bottom";

  JsonArray  ya = opt["scales"]["yAxes"].to<JsonArray>();
  JsonObject y1 = ya.add<JsonObject>();
  y1["id"]                        = "t";
  y1["position"]                  = "left";
  y1["scaleLabel"]["display"]     = true;
  y1["scaleLabel"]["labelString"] = "°C";
  JsonObject y2 = ya.add<JsonObject>();
  y2["id"]                          = "rh";
  y2["position"]                    = "right";
  y2["ticks"]["min"]                = 0;
  y2["ticks"]["max"]                = 100;
  y2["gridLines"]["drawOnChartArea"] = false;
  y2["scaleLabel"]["display"]       = true;
  y2["scaleLabel"]["labelString"]   = "% RH";
  JsonArray  xa = opt["scales"]["xAxes"].to<JsonArray>();
  JsonObject x1 = xa.add<JsonObject>();
  x1["ticks"]["maxTicksLimit"] = 13;
  x1["ticks"]["maxRotation"]   = 0;

  return tmQuickChartCreate(req, outUrl) ? 2 : 1;
}

// =====================================================================
//  РЕЛЕ
// =====================================================================

// Всё состояние насосов — по индексу реле, 0..RELAY_COUNT-1.
bool          pumpOn[RELAY_COUNT]      = { false };
unsigned long pumpStart[RELAY_COUNT]   = { 0 };
unsigned long pumpLimitMs[RELAY_COUNT] = { 0 };

void relayWrite(int i, bool on) {
  digitalWrite(RELAY_PINS[i], (on != RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

// Пин сначала выставляем, потом делаем выходом: иначе реле щёлкает при старте.
void relayBegin() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    relayWrite(i, false);
    pinMode(RELAY_PINS[i], OUTPUT);
    relayWrite(i, false);
  }
}

bool anyPumpOn() {
  for (int i = 0; i < RELAY_COUNT; i++)
    if (pumpOn[i]) return true;
  return false;
}

// Своя ячейка в памяти на каждое реле
String pumpKey(int i) { return "pump" + String(i); }

// =====================================================================
//  СОСТОЯНИЯ
// =====================================================================

enum State { ST_CONNECTING, ST_ONLINE, ST_ERROR };

State         state      = ST_CONNECTING;
unsigned long stateStart = 0;
unsigned long lastCheck  = 0;

void refreshAllPanels();      // объявлено заранее, определено ниже

void applyOnlineLed() {
  setLedEffect(anyPumpOn() ? &FX_PUMP_ON : &FX_ONLINE);
}

void setPump(int i, bool on, unsigned long minutes) {
  pumpOn[i] = on;
  relayWrite(i, on);
  prefs.putBool(pumpKey(i).c_str(), on);

  if (on) {
    pumpStart[i]   = millis();
    pumpLimitMs[i] = minutes * 60000UL;
    Serial.printf("%s ВКЛЮЧЁН на %lu мин\n", RELAY_NAMES[i], minutes);
  } else {
    pumpLimitMs[i] = 0;
    Serial.printf("%s выключен\n", RELAY_NAMES[i]);
  }

  if (state == ST_ONLINE) applyOnlineLed();
}

void allPumpsOff() {
  for (int i = 0; i < RELAY_COUNT; i++)
    if (pumpOn[i]) setPump(i, false, 0);
}

void enterConnecting() {
  Serial.printf("Подключаюсь к WiFi \"%s\"...\n", WIFI_SSID);
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  state      = ST_CONNECTING;
  stateStart = millis();
  setLedEffect(&FX_CONNECTING);
}

void enterOnline() {
  Serial.print("Подключено. IP-адрес: ");
  Serial.println(WiFi.localIP());

  // NTP: Asia/Tashkent, UTC+5, перехода на летнее время нет. Вызывается
  // и после переподключения WiFi — время пересинхронизируется заново.
  configTime(5 * 3600, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  state      = ST_ONLINE;
  stateStart = millis();
  lastCheck  = millis() - CHECK_INTERVAL_MS;
  applyOnlineLed();
}

void enterError(const char* reason) {
  Serial.printf("ОШИБКА: %s\n", reason);
  state      = ST_ERROR;
  stateStart = millis();
  setLedEffect(&FX_ERROR);
}

// =====================================================================
//  TELEGRAM: базовый вызов API
// =====================================================================

WiFiClientSecure tgClient;
long             lastUpdateId = 0;

// У каждого чата своя панель с кнопками. Храним их все, чтобы при любом
// изменении состояния перерисовать кнопки сразу везде.
struct Panel { long chatId; long msgId; };
const int MAX_PANELS = 4;
Panel  panels[MAX_PANELS] = {};
String lastActor = "";        // кто последним нажимал кнопку
long   reportChat = 0;        // кому доложить о результате проверки прошивки
bool   forceUpdate = false;   // перепрошиться, даже если файл не менялся

void rememberPanel(long chatId, long msgId) {
  for (int i = 0; i < MAX_PANELS; i++)
    if (panels[i].chatId == chatId) { panels[i].msgId = msgId; return; }
  for (int i = 0; i < MAX_PANELS; i++)
    if (panels[i].chatId == 0) { panels[i] = { chatId, msgId }; return; }
  panels[0] = { chatId, msgId };
}

bool isAllowed(long chatId) {
  if (OPEN_ACCESS) return true;
  for (int i = 0; i < ALLOWED_COUNT; i++)
    if (ALLOWED_CHATS[i] == chatId) return true;
  return false;
}

bool tgApi(const char* method, JsonDocument& doc, JsonDocument* out = nullptr,
           uint16_t timeoutMs = 6000) {
  String body;
  serializeJson(doc, body);

  HTTPClient http;
  http.setTimeout(timeoutMs);   // по умолчанию коротко: мёртвая панель не должна тормозить остальные
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/" + method;
  if (!http.begin(tgClient, url)) return false;

  http.addHeader("Content-Type", "application/json");
  int    code    = http.POST(body);
  String payload = (out && code > 0) ? http.getString() : String();
  http.end();

  if (code != HTTP_CODE_OK) {
    Serial.printf("%s вернул %d\n", method, code);
    return false;
  }
  if (out) deserializeJson(*out, payload);
  return true;
}

// =====================================================================
//  TELEGRAM: панель с кнопками
// =====================================================================

String pumpStatusLine(int i) {
  String s = String(RELAY_NAMES[i]) + ": ";
  if (!pumpOn[i]) return s + "выключен";
  unsigned long left = (pumpLimitMs[i] - (millis() - pumpStart[i])) / 60000UL + 1;
  return s + "РАБОТАЕТ, осталось ~" + String(left) + " мин";
}

// Секунды аптайма в тексте нужны, чтобы Telegram не отклонял
// редактирование с ошибкой "message is not modified"
String panelText(long chatId) {
  String s = "Управление насосами\n\n";
  s += sensorLines() + "\n\n";
  for (int i = 0; i < RELAY_COUNT; i++) s += pumpStatusLine(i) + "\n";
  s += "\nПрошивка v" + String(FW_VERSION);
  s += ", сигнал " + String(WiFi.RSSI()) + " dBm\n";
  s += "Аптайм: " + String(millis() / 1000) + " c";
  if (lastActor.length()) s += "\nПоследняя команда: " + lastActor;
  if (SHOW_CHAT_ID)       s += "\nchat_id: " + String(chatId);
  return s;
}

// По строке кнопок на каждое реле. Кнопки зависят от того,
// работает именно этот насос или нет — остальные три не при чём.
void buildKeyboard(JsonDocument& doc) {
  JsonArray rows = doc["reply_markup"]["inline_keyboard"].to<JsonArray>();

  for (int i = 0; i < RELAY_COUNT; i++) {
    JsonArray row = rows.add<JsonArray>();
    String    n   = String(i + 1);

    if (pumpOn[i]) {
      JsonObject b = row.add<JsonObject>();
      b["text"]          = String(RELAY_NAMES[i]) + ": выключить";
      b["callback_data"] = "off:" + n;
      b["style"]         = "danger";          // красная
    } else {
      JsonObject b1 = row.add<JsonObject>();
      b1["text"]          = String(RELAY_NAMES[i]) + ": 15 мин";
      b1["callback_data"] = "on:" + n + ":15";
      b1["style"]         = "success";        // зелёная
      JsonObject b2 = row.add<JsonObject>();
      b2["text"]          = "30 мин";
      b2["callback_data"] = "on:" + n + ":30";
      b2["style"]         = "success";
    }
  }

  if (anyPumpOn()) {
    JsonArray  rowAll = rows.add<JsonArray>();
    JsonObject b      = rowAll.add<JsonObject>();
    b["text"]          = "Выключить все";
    b["callback_data"] = "off";
    b["style"]         = "danger";
  }

  JsonArray row2 = rows.add<JsonArray>();
  JsonObject b3 = row2.add<JsonObject>();
  b3["text"]          = "Обновить статус";
  b3["callback_data"] = "refresh";
  JsonObject b4 = row2.add<JsonObject>();
  b4["text"]          = "Проверить прошивку";
  b4["callback_data"] = "update";
}

void tgSendPanel(long chatId) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"]    = panelText(chatId);
  buildKeyboard(doc);

  JsonDocument res;
  if (tgApi("sendMessage", doc, &res))
    rememberPanel(chatId, res["result"]["message_id"].as<long>());
}

// Перерисовывает одну панель. Если Telegram отказал (сообщение удалили,
// чат недоступен) — забываем этот слот, чтобы не ждать таймаут каждый раз.
void refreshPanelAt(int i) {
  if (panels[i].chatId == 0) return;

  JsonDocument doc;
  doc["chat_id"]    = panels[i].chatId;
  doc["message_id"] = panels[i].msgId;
  doc["text"]       = panelText(panels[i].chatId);
  buildKeyboard(doc);

  unsigned long t0 = millis();
  bool ok = tgApi("editMessageText", doc);
  Serial.printf("Панель чата %ld: %s за %lu мс\n",
                panels[i].chatId, ok ? "обновлена" : "ОШИБКА", millis() - t0);

  if (!ok) panels[i].chatId = 0;
}

// Обновляет панель конкретного чата — того, кто нажал кнопку.
// Вызывается первой, чтобы человек увидел результат сразу.
void refreshPanelFor(long chatId) {
  for (int i = 0; i < MAX_PANELS; i++)
    if (panels[i].chatId == chatId) { refreshPanelAt(i); return; }
}

void refreshAllPanelsExcept(long skipChatId) {
  for (int i = 0; i < MAX_PANELS; i++)
    if (panels[i].chatId != skipChatId) refreshPanelAt(i);
}

void refreshAllPanels() {
  refreshAllPanelsExcept(0);
}

// Гасит "часики" на нажатой кнопке. Telegram ждёт этого ответа.
void tgAnswerCallback(const char* id, const String& text) {
  JsonDocument doc;
  doc["callback_query_id"] = id;
  doc["text"]              = text;
  tgApi("answerCallbackQuery", doc);
}

void tgSend(long chatId, const String& text) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"]    = text;
  tgApi("sendMessage", doc);
}

// Отправка в группу/канал: chat_id строкой, long их id не вмещает.
bool tgSendTo(const char* chatId, const String& text) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"]    = text;
  return tgApi("sendMessage", doc);
}

// sendPhoto принимает в photo обычный URL и скачивает картинку сам —
// через плату изображение не проходит. Лимит caption — 1024 символа.
// Таймаут длинный: Telegram синхронно тянет картинку с QuickChart,
// и первый рендер может занять больше 6 секунд.
bool tgSendPhoto(const char* chatId, const String& photoUrl, const String& caption) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["photo"]   = photoUrl;
  doc["caption"] = caption;
  return tgApi("sendPhoto", doc, nullptr, 20000);
}

bool tgSendPhoto(long chatId, const String& photoUrl, const String& caption) {
  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["photo"]   = photoUrl;
  doc["caption"] = caption;
  return tgApi("sendPhoto", doc, nullptr, 20000);
}

// Сообщает результат проверки прошивки тому, кто её запросил.
// Если проверка была плановой, никому ничего не пишем.
void reportResult(const String& text) {
  Serial.println(text);
  if (reportChat) {
    tgSend(reportChat, text);
    reportChat = 0;
  }
}

// =====================================================================
//  ТЕЛЕМЕТРИЯ: планировщик
// =====================================================================
//
// Проверяется из loop() между проходами telegramPoll(): гранулярность
// ≤30 секунд, для минутных заданий достаточно. В state.bin лежит время
// последнего успешного запуска каждого задания — после перезагрузки
// в 07:01 задание на 07:00 не выполнится повторно, а пропущенное — догонит.

enum {                       // индексы в tmState.jobLast
  JOB_ROLLUP      = 0,       // суточная свёртка, 00:05
  JOB_DAILY_CHART = 1,       // суточный график, 07:00
  JOB_PUSH        = 2,       // выгрузка на GitHub, 00:10 (этап 3)
  JOB_WEEKLY      = 3,       // недельный график (этап 4)
  JOB_MONTHLY     = 4,       // месячный график (этап 4)
};

// Суточный график в группу. При отказе QuickChart — текстовая сводка:
// текст без графика лучше, чем тишина.
bool tmJobDailyChart() {
  String url, caption;
  int r = tmBuildDailyChart(url, caption);
  if (r == 0) {
    Serial.println("TM: график — данных ещё нет, пропускаю");
    return true;
  }
  if (r == 2 && tgSendPhoto(CH_DAILY, url, caption)) return true;
  Serial.println(r == 2 ? "TM: sendPhoto не прошёл, шлю текстом"
                        : "TM: QuickChart недоступен, шлю текстом");
  return tgSendTo(CH_DAILY, caption);
}

struct TmJob {
  uint8_t  slot;             // индекс в tmState.jobLast
  int8_t   hh, mm;           // локальное время срабатывания, ежедневно
  bool     network;          // тяжёлая сетевая работа: только онлайн и без насосов
  uint32_t window;           // окно догона, с; 0 — догонять всегда.
                             // Протухший график в группе никому не нужен,
                             // а догон за минуты до следующего срока даёт дубль.
  bool     (*run)();         // true = успех
  const char* name;
};

const TmJob TM_JOBS[] = {
  { JOB_ROLLUP,      0,  5, false, 0,        tmJobRollup,     "свёртка" },
  { JOB_DAILY_CHART, 7,  0, true,  2 * 3600, tmJobDailyChart, "график"  },
};
const int TM_JOB_COUNT = sizeof(TM_JOBS) / sizeof(TM_JOBS[0]);

// Ретраи живут в RAM: после перезагрузки задание просто снова due.
uint32_t tmJobRetryAtMs[8] = {0};
uint8_t  tmJobTries[8]     = {0};

void tmSchedulerRun() {
  if (!tmTimeSynced) return;    // без часов расписание бессмысленно

  static uint32_t lastCheckMs = 0;   // в состояниях с delay(50) loop крутится часто
  if (lastCheckMs && millis() - lastCheckMs < 5000) return;
  lastCheckMs = millis();

  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);

  for (int j = 0; j < TM_JOB_COUNT; j++) {
    const TmJob& job = TM_JOBS[j];

    // Последняя запланированная точка: сегодня hh:mm, либо вчера.
    struct tm st = lt;
    st.tm_hour = job.hh;
    st.tm_min  = job.mm;
    st.tm_sec  = 0;
    time_t sched = mktime(&st);
    if (sched > now) sched -= 86400;

    xSemaphoreTake(tmMutex, portMAX_DELAY);
    uint32_t last = tmState.jobLast[job.slot];
    xSemaphoreGive(tmMutex);
    if (last >= (uint32_t)sched) continue;          // уже выполнялось

    if (job.window && (uint32_t)(now - sched) > job.window) {
      Serial.printf("TM: задание «%s» пропущено: время ушло\n", job.name);
      tmJobTries[job.slot] = 0;
      xSemaphoreTake(tmMutex, portMAX_DELAY);
      tmState.jobLast[job.slot] = (uint32_t)now;
      tmSaveStateLocked();
      xSemaphoreGive(tmMutex);
      continue;
    }

    if (job.network) {
      if (state != ST_ONLINE) continue;             // отложим до сети
      if (anyPumpOn()) continue;                    // насос важнее графиков
    }
    if (tmJobTries[job.slot] &&
        (int32_t)(millis() - tmJobRetryAtMs[job.slot]) < 0) continue;

    Serial.printf("TM: задание «%s» (попытка %u)\n", job.name, tmJobTries[job.slot] + 1);
    bool ok = job.run();

    if (ok || tmJobTries[job.slot] >= 4) {          // 1 попытка + 4 ретрая
      if (!ok) Serial.printf("TM: задание «%s» пропущено после 5 попыток\n", job.name);
      tmJobTries[job.slot] = 0;
      xSemaphoreTake(tmMutex, portMAX_DELAY);
      tmState.jobLast[job.slot] = (uint32_t)now;
      tmSaveStateLocked();
      xSemaphoreGive(tmMutex);
    } else {
      tmJobTries[job.slot]++;
      tmJobRetryAtMs[job.slot] = millis() + 15UL * 60 * 1000;
      Serial.printf("TM: задание «%s» не удалось, повтор через 15 мин\n", job.name);
    }
  }
}

// =====================================================================
//  TELEGRAM: обработка действий
// =====================================================================

// Режет строку по двоеточиям и пробелам: "on:2:30" -> ["on", "2", "30"]
int splitAction(const String& src, String* parts, int maxParts) {
  int n = 0, start = 0;
  for (int i = 0; i <= (int)src.length() && n < maxParts; i++) {
    bool end = (i == (int)src.length()) || src[i] == ':' || src[i] == ' ';
    if (!end) continue;
    if (i > start) parts[n++] = src.substring(start, i);
    start = i + 1;
  }
  return n;
}

// Общая точка входа: и для кнопок, и для текстовых команд.
// Реле указывается номером 1..4 — либо прямо в слове ("on2", "off3"),
// либо отдельной частью ("on:2:30"). Без номера работает первое реле,
// а голое "off" глушит сразу все.
// Возвращает короткую подсказку для всплывашки над кнопкой.
String doAction(String action, long chatId) {
  action.toLowerCase();

  String p[4];
  int    n = splitAction(action, p, 4);
  if (n == 0) return "";

  String cmd   = p[0];
  int    relay = 0;                        // 0 = номер не указан

  // "on2" / "off3" — отрываем номер от слова
  const char* base = cmd.startsWith("off") ? "off"
                   : cmd.startsWith("on")  ? "on"  : nullptr;
  if (base) {
    String tail = cmd.substring(strlen(base));
    if (tail.length() && isDigit(tail[0])) {
      relay = tail.toInt();
      cmd   = base;
    }
  }

  if (cmd == "on") {
    unsigned long minutes = PUMP_DEFAULT_MIN;
    if (relay == 0 && n >= 3) {            // "on:2:30" — номер и время
      relay   = p[1].toInt();
      minutes = p[2].toInt();
    } else if (n >= 2) {                   // "on2:30" или старое "on:30"
      minutes = p[1].toInt();
    }
    if (relay == 0) relay = 1;
    if (relay > RELAY_COUNT) return "Нет реле с таким номером";
    if (minutes < 1)            minutes = PUMP_DEFAULT_MIN;
    if (minutes > PUMP_MAX_MIN) minutes = PUMP_MAX_MIN;

    setPump(relay - 1, true, minutes);
    return String(RELAY_NAMES[relay - 1]) + " включён на " + String(minutes) + " мин";

  } else if (cmd == "off") {
    if (relay == 0 && n >= 2) relay = p[1].toInt();
    if (relay == 0) {                      // просто "/off" — выключаем всё
      allPumpsOff();
      return "Все насосы выключены";
    }
    if (relay > RELAY_COUNT) return "Нет реле с таким номером";

    setPump(relay - 1, false, 0);
    return String(RELAY_NAMES[relay - 1]) + " выключен";

  } else if (cmd == "update" || cmd == "reflash") {
    if (anyPumpOn()) return "Сначала выключите насосы";
    forceUpdate = (cmd == "reflash");
    reportChat  = chatId;
    lastCheck   = millis() - CHECK_INTERVAL_MS;   // запустить проверку немедленно
    return forceUpdate ? "Перепрошиваюсь принудительно" : "Проверяю прошивку";

  } else if (cmd == "refresh") {
    return "Обновлено";
  }

  return "";
}

// =====================================================================
//  TELEGRAM: отладочные команды телеметрии
// =====================================================================

// Сотые доли -> строка с одним знаком после запятой.
String tmVal(int16_t v) {
  if (v == TM_NOVAL) return "—";
  return String(v / 100.0f, 1);
}

String tmClockLine() {
  if (!tmTimeSynced) return "Часы: НЕ синхронизированы, бакеты не формируются";
  char buf[24];
  tmFmtLocal(time(nullptr), "%d.%m.%Y %H:%M:%S", buf, sizeof(buf));
  return String("Часы: ") + buf + " (UTC+5, " +
         (tmNtpSynced ? "NTP подтверждён" : "RTC, NTP ещё не отвечал") + ")";
}

void tmCmdDump(long chatId, int want) {
  if (want < 1)  want = 1;
  if (want > 24) want = 24;

  Bucket buf[24];
  int got = tmReadLastBuckets(buf, want);

  String s;
  if (!got) {
    s = tmTimeSynced
        ? "Кольцо пусто: первый бакет ещё не закрылся (граница каждые "
          + String(BUCKET_MIN) + " мин)"
        : "Кольцо пусто: нет синхронизации NTP, бакеты не формируются";
  } else {
    s = "Последние бакеты, T и RH как min/avg/max, время локальное:\n";
    char when[16];
    for (int i = 0; i < got; i++) {
      tmFmtLocal((time_t)buf[i].ts, "%d.%m %H:%M", when, sizeof(when));
      s += String(when);
      s += "  T " + tmVal(buf[i].t_min) + "/" + tmVal(buf[i].t_avg) + "/" + tmVal(buf[i].t_max);
      s += "  RH " + tmVal(buf[i].rh_min) + "/" + tmVal(buf[i].rh_avg) + "/" + tmVal(buf[i].rh_max);
      s += "  n=" + String(buf[i].n) + " f=" + String(buf[i].n_fail) + "\n";
    }
  }

  xSemaphoreTake(tmMutex, portMAX_DELAY);
  uint32_t accStart = tmAcc.bucketStart;
  uint16_t accN = tmAcc.n, accFail = tmAcc.nFail;
  xSemaphoreGive(tmMutex);

  if (accStart) {
    char when[16];
    tmFmtLocal((time_t)accStart, "%H:%M", when, sizeof(when));
    s += "\nКопится: с " + String(when) + ", отсчётов " + String(accN)
       + ", отказов " + String(accFail);
  }
  tgSend(chatId, s);
}

void tmCmdStatus(long chatId) {
  String s = "Телеметрия (этап 1)\n";
  s += tmClockLine() + "\n";
  s += String("Хранилище: ") + (tmFsOk ? "ок" : "ОШИБКА, данные только в Serial") + "\n";

  if (tmLastSampleMs)
    s += "Задача: последний опрос " + String((millis() - tmLastSampleMs) / 1000) + " с назад\n";
  else
    s += "Задача: опросов ещё не было\n";

  uint32_t okC = tmTotalOk, failC = tmTotalFail;
  s += "Чтений с загрузки: " + String(okC) + " ок, " + String(failC) + " отказов";
  if (okC + failC) s += " (" + String(100.0f * failC / (okC + failC), 0) + "%)";
  s += "\n";

  xSemaphoreTake(tmMutex, portMAX_DELAY);
  uint16_t used = tmState.bucketUsed, head = tmState.bucketHead;
  uint32_t accStart = tmAcc.bucketStart;
  uint16_t accN = tmAcc.n, accFail = tmAcc.nFail;
  uint32_t jRollup = tmState.jobLast[JOB_ROLLUP];
  uint32_t jChart  = tmState.jobLast[JOB_DAILY_CHART];
  xSemaphoreGive(tmMutex);

  char when[20];
  s += "Свёртка 00:05: ";
  if (jRollup) { tmFmtLocal((time_t)jRollup, "%d.%m %H:%M", when, sizeof(when)); s += when; }
  else s += "ещё не было";
  s += "\nГрафик 07:00: ";
  if (jChart) { tmFmtLocal((time_t)jChart, "%d.%m %H:%M", when, sizeof(when)); s += when; }
  else s += "ещё не было";
  s += "\n";

  if (accStart) {
    char when[16];
    tmFmtLocal((time_t)accStart, "%H:%M", when, sizeof(when));
    s += "Текущий бакет: с " + String(when) + ", отсчётов " + String(accN)
       + ", отказов " + String(accFail) + "\n";
  } else {
    s += "Текущий бакет: не начат\n";
  }

  s += "Кольцо: " + String(used) + "/" + String(BUCKET_CAPACITY)
     + " бакетов, голова " + String(head) + "\n";
  if (tmFsOk)
    s += "LittleFS: " + String((unsigned)(LittleFS.usedBytes() / 1024)) + "/"
       + String((unsigned)(LittleFS.totalBytes() / 1024)) + " КБ\n";
  s += "Куча: свободно " + String(ESP.getFreeHeap() / 1024) + " КБ";
  tgSend(chatId, s);
}

void tmCmdTime(long chatId) {
  String s = tmClockLine() + "\n";
  s += "UTC unix: " + String((unsigned long)time(nullptr)) + "\n";
  s += "Аптайм: " + String(millis() / 1000) + " с";
  tgSend(chatId, s);
}

// Ручной запуск графика. «/tm_chart daily» — в чат запросившего,
// «/tm_chart daily ch» — в группу CH_DAILY (проверить права бота).
void tmCmdChart(long chatId, const String& kind, bool toChannel) {
  if (kind != "daily") {
    tgSend(chatId, kind == "weekly" || kind == "monthly"
                       ? "Недельный и месячный графики — этап 4"
                       : "Так умею: /tm_chart daily [ch]");
    return;
  }
  if (anyPumpOn()) {           // как /update: тяжёлая сеть подождёт полива
    tgSend(chatId, "Насос работает — сначала выключите насосы");
    return;
  }
  tgSend(chatId, "Строю суточный график…");

  String url, caption;
  int r = tmBuildDailyChart(url, caption);
  if (r == 0) {
    tgSend(chatId, "Данных ещё нет: ни одного закрытого бакета");
    return;
  }
  if (r == 2) {
    bool ok = toChannel ? tgSendPhoto(CH_DAILY, url, caption)
                        : tgSendPhoto(chatId, url, caption);
    if (ok) {
      if (toChannel) tgSend(chatId, "График ушёл в группу");
      return;
    }
    tgSend(chatId, "sendPhoto не прошёл (бот не в группе?), сводка текстом:\n" + caption);
    return;
  }
  tgSend(chatId, "QuickChart недоступен, сводка текстом:\n" + caption);
}

void tmCmdDays(long chatId, int want) {
  if (want < 1)  want = 1;
  if (want > 30) want = 30;

  DayRec d[30];
  int got = tmReadLastDays(d, want);
  if (!got) {
    tgSend(chatId, "daily.bin пуст: первая свёртка — в 00:05");
    return;
  }
  String s = "Суточные записи (" + String(got) + "):\n";
  char when[12], atMin[8], atMax[8];
  for (int i = 0; i < got; i++) {
    tmFmtLocal((time_t)d[i].day, "%d.%m", when, sizeof(when));
    snprintf(atMin, sizeof(atMin), "%02u:%02u", d[i].t_min_at / 60, d[i].t_min_at % 60);
    snprintf(atMax, sizeof(atMax), "%02u:%02u", d[i].t_max_at / 60, d[i].t_max_at % 60);
    s += String(when) + "  T " + String(d[i].t_min / 100.0f, 1) + "…" +
         String(d[i].t_max / 100.0f, 1) + " (ср " + String(d[i].t_avg / 100.0f, 1) +
         ", мин " + atMin + ", макс " + atMax + ")  RH мин " +
         String(d[i].rh_min / 100.0f, 0) + " ср " + String(d[i].rh_avg / 100.0f, 0) +
         "  VPD·ч " + String(d[i].vpd_hsum / 100.0f, 1) +
         "  GDD " + String(d[i].gdd10 / 100.0f, 1) + "\n";
  }
  tgSend(chatId, s);
}

void handleTmCommand(long chatId, const String& text) {
  String p[3];
  int n = splitAction(text, p, 3);

  if      (p[0] == "/tm_dump")   tmCmdDump(chatId, n >= 2 ? p[1].toInt() : 8);
  else if (p[0] == "/tm_status") tmCmdStatus(chatId);
  else if (p[0] == "/tm_time")   tmCmdTime(chatId);
  else if (p[0] == "/tm_chart")  tmCmdChart(chatId, n >= 2 ? p[1] : String("daily"),
                                            n >= 3 && p[2] == "ch");
  else if (p[0] == "/tm_days")   tmCmdDays(chatId, n >= 2 ? p[1].toInt() : 7);
  else tgSend(chatId,
              "Команды телеметрии:\n"
              "/tm_status — состояние сбора\n"
              "/tm_dump N — последние N бакетов (1–24)\n"
              "/tm_chart daily [ch] — суточный график сюда или в группу\n"
              "/tm_days N — суточные свёртки\n"
              "/tm_time — часы NTP");
}

String helpText() {
  String s = "Кнопки под панелью управляют каждым реле отдельно.\n\n";
  s += "То же самое текстом:\n";
  s += "/on2 20 — включить насос 2 на 20 мин\n";
  s += "/on3 — включить насос 3 на " + String(PUMP_DEFAULT_MIN) + " мин\n";
  s += "/off2 — выключить насос 2\n";
  s += "/off — выключить все\n";
  s += "/update — проверить прошивку\n\n";
  s += "Номер реле — от 1 до " + String(RELAY_COUNT);
  s += ", время — до " + String(PUMP_MAX_MIN) + " мин.";
  s += "\n\nТелеметрия:\n";
  s += "/tm_status — состояние сбора\n";
  s += "/tm_dump N — последние N бакетов\n";
  s += "/tm_chart daily — суточный график\n";
  s += "/tm_days N — суточные свёртки\n";
  s += "/tm_time — часы NTP";
  return s;
}

void handleMessage(long chatId, String text) {
  text.trim();
  int at = text.indexOf('@');
  if (at > 0) text = text.substring(0, at);

  Serial.printf("Сообщение от %ld: %s\n", chatId, text.c_str());

  if (text.startsWith("/tm")) { handleTmCommand(chatId, text); return; }

  if (text.startsWith("/help")) tgSend(chatId, helpText());

  if (text.startsWith("/start") || text.startsWith("/help") || text == "/menu") {
    tgSendPanel(chatId);
    return;
  }

  String action = text.startsWith("/") ? text.substring(1) : text;

  if (doAction(action, chatId).length()) refreshAllPanels();
  else tgSendPanel(chatId);    // непонятную команду просто отвечаем панелью
}

void handleCallback(JsonObject cb) {
  const char* cbId   = cb["id"];
  long        chatId = cb["message"]["chat"]["id"].as<long>();
  long        msgId  = cb["message"]["message_id"].as<long>();
  const char* data   = cb["data"];

  if (!isAllowed(chatId)) {
    tgAnswerCallback(cbId, "Нет доступа");
    return;
  }
  if (data == nullptr) return;

  const char* who = cb["from"]["first_name"];
  lastActor = who ? String(who) : String("");

  Serial.printf("Нажата кнопка %s (чат %ld, %s)\n", data, chatId, lastActor.c_str());

  rememberPanel(chatId, msgId);

  String note = doAction(String(data), chatId);
  tgAnswerCallback(cbId, note);
  refreshPanelFor(chatId);            // сначала тому, кто нажал
  refreshAllPanelsExcept(chatId);     // потом остальным
}

void telegramPoll() {
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN +
               "/getUpdates?timeout=" + POLL_TIMEOUT_S +
               "&offset=" + lastUpdateId +
               "&allowed_updates=%5B%22message%22%2C%22callback_query%22%5D";

  HTTPClient http;
  http.setTimeout((POLL_TIMEOUT_S + 10) * 1000);
  http.setReuse(true);
  if (!http.begin(tgClient, url)) return;

  int    code = http.GET();
  String payload;
  if (code == HTTP_CODE_OK) payload = http.getString();
  http.end();

  if (code != HTTP_CODE_OK) {
    if (code > 0) Serial.printf("getUpdates вернул %d\n", code);
    delay(2000);
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("Не смог разобрать ответ Telegram");
    return;
  }

  for (JsonObject upd : doc["result"].as<JsonArray>()) {
    lastUpdateId = upd["update_id"].as<long>() + 1;

    JsonObject cb = upd["callback_query"];
    if (!cb.isNull()) {
      handleCallback(cb);
      continue;
    }

    JsonObject msg = upd["message"];
    if (msg.isNull()) continue;

    long        chatId = msg["chat"]["id"].as<long>();
    const char* txt    = msg["text"];
    if (txt == nullptr) continue;

    if (!isAllowed(chatId)) {
      Serial.printf("Игнорирую чужой чат: %ld\n", chatId);
      continue;
    }

    const char* who = msg["from"]["first_name"];
    lastActor = who ? String(who) : String("");

    handleMessage(chatId, String(txt));
  }
}

// =====================================================================
//  ОБНОВЛЕНИЕ
// =====================================================================

// CDN GitHub кэширует ответы по полному URL (обычно max-age=300).
// Меняющийся параметр делает каждый запрос уникальным, и кэш обходится.
String firmwareUrl() {
  return String(FIRMWARE_URL) + "?cb=" + String(millis());
}

// Вместо version.txt спрашиваем у сервера метаданные самого firmware.bin.
// Запрос HEAD скачивает только заголовки, тело не передаётся — это дёшево.
// ETag у GitHub меняется при любом изменении содержимого файла.
String getRemoteSignature() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, firmwareUrl())) return "";

  const char* keys[] = { "ETag", "Last-Modified", "Content-Length" };
  http.collectHeaders(keys, 3);

  int    code = http.sendRequest("HEAD");
  String sig  = "";

  // Некоторые прокси и CDN отвечают на HEAD не так, как на GET.
  // Если HEAD не прошёл — повторяем обычным GET, тело просто не читаем.
  if (code != HTTP_CODE_OK) {
    Serial.printf("HEAD вернул %d, пробую GET\n", code);
    code = http.GET();
  }

  if (code == HTTP_CODE_OK) {
    sig = http.header("ETag");
    if (!sig.length()) sig = http.header("Last-Modified");
    sig += "|" + http.header("Content-Length");
  } else {
    Serial.printf("Сервер вернул %d\n", code);
  }

  http.end();
  return (sig == "|") ? String("") : sig;
}

void doUpdate(const String& sig) {
  Serial.println(">>> Начинаю обновление. Не отключайте питание!");
  for (int i = 0; i < MAX_PANELS; i++)
    if (panels[i].chatId) tgSend(panels[i].chatId, "Качаю новую прошивку, скоро перезагружусь");
  setLedEffect(&FX_UPDATING);

  // Запоминаем подпись ДО прошивки: после успеха плата перезагрузится
  // и сюда уже не вернётся.
  String prevSig = prefs.getString("fwsig", "");
  prefs.putString("fwsig", sig);

  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(client, firmwareUrl());

  if (ret == HTTP_UPDATE_FAILED) {
    Serial.printf("Ошибка обновления (%d): %s\n",
                  httpUpdate.getLastError(),
                  httpUpdate.getLastErrorString().c_str());
    prefs.putString("fwsig", prevSig);   // откатываем, чтобы попробовать снова
    enterError("не удалось скачать прошивку");
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    applyOnlineLed();
  }
}

void checkForUpdate() {
  if (anyPumpOn()) {
    Serial.println("Насос работает — обновление отложено");
    return;
  }

  Serial.println("--- Проверяю прошивку на сервере ---");
  setLedEffect(&FX_CHECKING);

  String sig = getRemoteSignature();
  if (!sig.length()) {
    forceUpdate = false;
    reportResult("Не смог получить данные о прошивке с сервера");
    enterError("сервер прошивок не отвечает");
    return;
  }

  String known = prefs.getString("fwsig", "");
  Serial.printf("Известная: %s\nНа сервере: %s\n", known.c_str(), sig.c_str());

  if (known.length() == 0 && SKIP_FIRST_UPDATE && !forceUpdate) {
    prefs.putString("fwsig", sig);
    reportResult("Первый запуск — запомнил файл на сервере, прошивку не менял");
    applyOnlineLed();
    return;
  }

  if (sig != known || forceUpdate) {
    bool wasForced = forceUpdate;
    forceUpdate = false;
    reportResult(wasForced ? "Качаю прошивку заново"
                           : "Найдена новая прошивка, качаю");
    doUpdate(sig);
  } else {
    forceUpdate = false;
    reportResult("Файл на сервере не менялся, обновление не требуется");
    applyOnlineLed();
  }
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================

void setup() {
  relayBegin();
  Serial.begin(115200);
  delay(2000);

  ledBegin();
  xTaskCreatePinnedToCore(ledTask, "led", 2048, NULL, 1, NULL, 1);

  dht.begin();

  // Пояс выставляем сразу, не дожидаясь configTime: RTC переживает мягкую
  // перезагрузку, и бакеты могут пойти ещё до синхронизации NTP.
  setenv("TZ", "<+05>-5", 1);   // Asia/Tashkent, UTC+5, летнего времени нет
  tzset();
  sntp_set_time_sync_notification_cb(tmOnNtpSync);

  tmInit();   // LittleFS, кольцо бакетов и задача опроса датчика

  prefs.begin("cfg", false);
  prefs.remove("pump");            // ключ от старой версии с одним реле

  for (int i = 0; i < RELAY_COUNT; i++) {
    String key   = pumpKey(i);
    bool   wasOn = prefs.getBool(key.c_str(), false);
    if (RESTORE_ON_BOOT && wasOn) {
      setPump(i, true, PUMP_DEFAULT_MIN);
    } else {
      if (wasOn) Serial.printf("%s был включён до перезагрузки — оставляю выключённым\n",
                               RELAY_NAMES[i]);
      prefs.putBool(key.c_str(), false);
    }
  }

  tgClient.setInsecure();

  Serial.println();
  Serial.println("=================================");
  Serial.printf("Запуск. Версия прошивки: %d\n", FW_VERSION);
  Serial.printf("Причина перезагрузки: %d\n", esp_reset_reason());
  Serial.println("=================================");

  enterConnecting();
}

// У каждого реле свой отсчёт; панели перерисовываем один раз за проход.
void checkPumpTimeout() {
  bool changed = false;

  for (int i = 0; i < RELAY_COUNT; i++) {
    if (pumpOn[i] && millis() - pumpStart[i] >= pumpLimitMs[i]) {
      Serial.printf("%s: время вышло, выключаю\n", RELAY_NAMES[i]);
      lastActor = "таймер";
      setPump(i, false, 0);
      changed = true;
    }
  }

  if (changed) refreshAllPanels();   // кнопки во всех чатах сами переключатся
}

void loop() {
  checkPumpTimeout();

  switch (state) {

    case ST_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        enterOnline();
      } else if (millis() - stateStart > WIFI_TIMEOUT_MS) {
        enterError("не удалось подключиться к WiFi");
      }
      delay(50);
      break;

    case ST_ONLINE:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi отвалился, переподключаюсь");
        enterConnecting();
        break;
      }

      if (millis() - lastCheck >= CHECK_INTERVAL_MS) {
        lastCheck = millis();
        checkForUpdate();
      }

      telegramPoll();
      break;

    case ST_ERROR:
      if (millis() - stateStart > ERROR_RETRY_MS) {
        Serial.println("Повторная попытка");
        enterConnecting();
      }
      delay(50);
      break;
  }

  // Сразу после telegramPoll() в онлайне; несетевые задания (свёртка)
  // выполняются и без сети.
  tmSchedulerRun();
}