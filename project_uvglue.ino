// ============================================================================
//   АВТОМАТИЗИРАН РОБОТ ЗА ЛЕПИЛО С ИМПУЛСНО НАНАСЯНЕ И UV (ПРАВ ЛИНЕЕН КОД)
// ============================================================================

#include <Arduino.h>

// ----------------------------------------------------------------------------
// 1. ПИНОВЕ И ДРАЙВЕРИ
// ----------------------------------------------------------------------------
#define X_ENA_PIN  10  
#define X_DIR_PIN   9  
#define X_STEP_PIN 12  

#define Y_ENA_PIN   3  
#define Y_DIR_PIN   4  
#define Y_STEP_PIN  6  

#define GLUE_DISPENSER_PIN 7  // Реле Nordson Ultimus (Active LOW)
#define UV_LAMP_PIN        8  // Реле UV Лампа (импулсно)

#define X_MIN_PIN A2  // изключвател по Y FORWARD / X MIN
#define X_MAX_PIN A3  // изключвател по X FORWARD
#define Y_MIN_PIN A0  // изключвател по Y REVERSED
#define Y_MAX_PIN A1  // изключвател по X REVERSED / Y MAX

#define START_BTN_PIN A5  

// ============================================================================
// 2. НАСТРОЙКИ ЗА ГЕОМЕТРИЯ, ДВИЖЕНИЕ И ИМПУЛСИ НА ЛЕПИЛОТО
// ============================================================================
const float STEPS_PER_MM_X = 4007; 
const float STEPS_PER_MM_Y = 4007; 

int minDelayUs = 50;    
int maxDelayUs = 150;   

const float HOME_OFFSET_X_MM      = 38.5;    // Начален offset по X след HOME
const float HOME_OFFSET_Y_MM      = 3.0;   // Начален offset по Y след HOME
const float RECT_WIDTH_MM         = 8.6;  // Дължина на правоъгълника по X
const float RECT_HEIGHT_MM        = 5.64;  // Височина на правоъгълника
const float TRANSITION_DIST_MM    = 26.6;  // Преход между двата правоъгълника
const float UV_OFFSET_X_MM        = 37.6;  // Изместване НАЗАД към UV лампата
const int   UV_TIME_SEC           = 6     // Време за изпичане (секунди)

// --- НАСТРОЙКИ ЗА ИМПУЛСНО НАНАСЯНЕ НА ЛЕПИЛО (В МИЛИСЕКУНДИ) ---
const unsigned long GLUE_ON_TIME_MS  = 250;  // Време с ПУСНАТО лепило (0.25 сек)
const unsigned long GLUE_OFF_TIME_MS = 800;  // Време със СПРЯНО лепило (0.80 сек)

long currentXSteps = 0;
long currentYSteps = 0;

bool isRunning          = false;        
bool stopRequested      = false;      
bool isPaused           = false;        
bool isDispensingGlue   = false;   
bool isUVCuringActive   = false;   
bool isHoming           = false; // Защита: флаг за калибриране

// Прототипи
bool returnToHomePosition();
bool checkGlobalCommands();
bool moveStepsControlled(char axis, bool dir, long steps);
bool moveStepsWithGluePulsing(bool dir, long steps);
bool runUVCuring(int seconds);
void runSingleExecutionPattern();
void sendRelayPulse();
void handlePauseLogic();

// ============================================================================
// 3. SETUP ФУНКЦИЯ
// ============================================================================
void setup() {
  Serial.begin(115200); 

  digitalWrite(UV_LAMP_PIN, HIGH); 
  pinMode(UV_LAMP_PIN, OUTPUT);

  digitalWrite(GLUE_DISPENSER_PIN, HIGH); // Изключено реле (Active LOW)
  pinMode(GLUE_DISPENSER_PIN, OUTPUT);

  pinMode(X_ENA_PIN, OUTPUT); pinMode(X_DIR_PIN, OUTPUT); pinMode(X_STEP_PIN, OUTPUT);
  pinMode(Y_ENA_PIN, OUTPUT); pinMode(Y_DIR_PIN, OUTPUT); pinMode(Y_STEP_PIN, OUTPUT);

  digitalWrite(X_ENA_PIN, LOW); 
  digitalWrite(Y_ENA_PIN, LOW);

  pinMode(X_MIN_PIN, INPUT_PULLUP); pinMode(X_MAX_PIN, INPUT_PULLUP);
  pinMode(Y_MIN_PIN, INPUT_PULLUP); pinMode(Y_MAX_PIN, INPUT_PULLUP);
  pinMode(START_BTN_PIN, INPUT_PULLUP);

  Serial.println(F("\n=================================================="));
  Serial.println(F(" РОБОТ ЗА ЛЕПИЛО | ПАУЗА С КРАТКО И HOME С ДЪЛГО НАТИСКАНЕ"));
  Serial.println(F("=================================================="));

  returnToHomePosition();
}

// ============================================================================
// 4. MAIN LOOP
// ============================================================================
void loop() {
  // Ако роботът НЕ работи, кратко натискане стартира цикъла
  if (digitalRead(START_BTN_PIN) == LOW && !isRunning) {
    unsigned long pressStartTime = millis();
    bool heldLongEnough = false;

    while (digitalRead(START_BTN_PIN) == LOW) {
      if (millis() - pressStartTime >= 2000) {
        heldLongEnough = true;
        break;
      }
      delay(10);
    }

    while (digitalRead(START_BTN_PIN) == LOW) { delay(10); } // Изчакваме пускане

    if (heldLongEnough) {
      Serial.println(F("\n🏠 [ПРИНУДИТЕЛЕН HOME] Задържан бутон за 2 секунди..."));
      returnToHomePosition();
    } else {
      runSingleExecutionPattern();
    }
  }
}

void sendRelayPulse() {
  digitalWrite(UV_LAMP_PIN, LOW);  
  delay(100);                      
  digitalWrite(UV_LAMP_PIN, HIGH); 
}

bool checkGlobalCommands() {
  // -------------------------------------------------------------------------
  // 1. ПРОВЕРКА НА БУТОНА ПО ВРЕМЕ НА РАБОТА 
  // (КРАТКО = ПАУЗА НА МЯСТО, ДЪЛГО 2с = HOME)
  // -------------------------------------------------------------------------
  if (digitalRead(START_BTN_PIN) == LOW) {
    unsigned long pressStartTime = millis();
    bool heldLongEnough = false;

    while (digitalRead(START_BTN_PIN) == LOW) {
      if (millis() - pressStartTime >= 2000) {
        heldLongEnough = true;
        break;
      }
      delay(10);
    }

    while (digitalRead(START_BTN_PIN) == LOW) { delay(10); } // Изчакваме пускане

    if (heldLongEnough) {
      // Задържан 2 секунди -> Авариен стоп и връщане в Home
      stopRequested = true;
      digitalWrite(GLUE_DISPENSER_PIN, HIGH); 
      if (isUVCuringActive) {
        sendRelayPulse();
        isUVCuringActive = false;
      }
      Serial.println(F("\n🏠 [ПРИНУДИТЕЛЕН HOME] Бутонът е задържан 2 секунди! Прекъсване и прибиране..."));
      return true;
    } else {
      // Кратко натискане -> Само Пауза на място (без връщане в Home)
      isPaused = true;
      handlePauseLogic();
      if (stopRequested) return true;
    }
  }

  // -------------------------------------------------------------------------
  // 2. АВАРИЙНА ПРОВЕРКА НА КРАЙНИТЕ ИЗКЛЮЧВАТЕЛИ A1 И A2
  // -------------------------------------------------------------------------
  if (!isHoming) {
    if (digitalRead(A1) == LOW || digitalRead(A2) == LOW) {
      stopRequested = true;
      digitalWrite(GLUE_DISPENSER_PIN, HIGH); 
      if (isUVCuringActive) {
        sendRelayPulse(); 
        isUVCuringActive = false;
      }
      
      Serial.print(F("\n🛑 [АВАРИЕН СТОП - КРАЕН ИЗКЛЮЧВАТЕЛ] Задействан е: "));
      if (digitalRead(A1) == LOW) Serial.print(F("A1 "));
      if (digitalRead(A2) == LOW) Serial.print(F("A2 "));
      Serial.println(F("| Движението е прекратено!"));
      
      return true;
    }
  }

  // -------------------------------------------------------------------------
  // 3. ПРОВЕРКА ЗА СЕРИЙНИ КОМАНДИ (SPACE ЗА СТОП/HOME, P ЗА ПАУЗА)
  // -------------------------------------------------------------------------
  if (Serial.available() > 0) {
    char ch = Serial.read();
    if (ch == ' ') { 
      stopRequested = true;
      digitalWrite(GLUE_DISPENSER_PIN, HIGH);
      if (isUVCuringActive) sendRelayPulse(); 
      Serial.println(F("\n🛑 [АВАРИЕН СТОП] Прекъснато от потребителя!"));
      return true;
    } else if (ch == 'p' || ch == 'P') { 
      isPaused = true;
      handlePauseLogic();
      if (stopRequested) return true;
    }
  }

  return stopRequested;
}

void handlePauseLogic() {
  Serial.println(F("\n⏸️ [ПАУЗА] Роботът е спрян на място. Натиснете бутона кратко за продължаване или задръжте 2с за Home."));
  digitalWrite(GLUE_DISPENSER_PIN, HIGH);
  if (isUVCuringActive) sendRelayPulse(); 

  while (isPaused && !stopRequested) {
    if (digitalRead(START_BTN_PIN) == LOW) {
      unsigned long pressStartTime = millis();
      bool heldLongEnough = false;
      while (digitalRead(START_BTN_PIN) == LOW) {още
        if (millis() - pressStartTime >= 2000) { heldLongEnough = true; break; }
        delay(10);
      }
      
      while (digitalRead(START_BTN_PIN) == LOW) { delay(10); } // Изчакваме пускане

      if (heldLongEnough) {
        stopRequested = true;
        isPaused = false;
        Serial.println(F("\n🏠 [ПРИНУДИТЕЛЕН HOME] От пауза към Home..."));
        return;
      } else {
        // Кратко натискане по време на пауза -> Продължаване от същото място
        isPaused = false;
      }
    }

    if (Serial.available() > 0) {
      char ch = Serial.read();
      if (ch == 'p' || ch == 'P') isPaused = false;
      else if (ch == ' ') { stopRequested = true; isPaused = false; }
    }
    delay(20);
  }

  if (stopRequested) return;

  Serial.println(F("▶️ [ПРОДЪЛЖАВАНЕ] Работата се възобновява от мястото на спиране..."));
  if (isDispensingGlue) digitalWrite(GLUE_DISPENSER_PIN, LOW);
  if (isUVCuringActive) sendRelayPulse(); 
}

bool moveStepsControlled(char axis, bool dir, long steps) {
  if (steps <= 0 || stopRequested) return false;

  if (axis == 'X') digitalWrite(X_DIR_PIN, dir ? HIGH : LOW);
  else if (axis == 'Y') digitalWrite(Y_DIR_PIN, dir ? HIGH : LOW);
  delayMicroseconds(20); 

  int currentMinDelay = dir ? minDelayUs : 30; 
  int currentMaxDelay = dir ? maxDelayUs : 80;

  long accelSteps = steps / 4;
  if (accelSteps > 500) accelSteps = 500;

  for (long i = 0; i < steps; i++) {
    if (checkGlobalCommands()) return false; 

    int currentDelay = currentMinDelay;
    if (accelSteps > 0) {
      if (i < accelSteps) {
        currentDelay = currentMaxDelay - ((currentMaxDelay - currentMinDelay) * i / accelSteps);
      } else if (i > steps - accelSteps) {
        currentDelay = currentMinDelay + ((currentMaxDelay - currentMinDelay) * (i - (steps - accelSteps)) / accelSteps);
      }
    }

    if (axis == 'X') {
      digitalWrite(X_STEP_PIN, HIGH); delayMicroseconds(currentDelay);
      digitalWrite(X_STEP_PIN, LOW);  delayMicroseconds(currentDelay);
      currentXSteps += (dir ? 1 : -1); 
    } else {
      digitalWrite(Y_STEP_PIN, HIGH); delayMicroseconds(currentDelay);
      digitalWrite(Y_STEP_PIN, LOW);  delayMicroseconds(currentDelay);
      currentYSteps += (dir ? 1 : -1);
    }
  }
  return true;
}

bool moveStepsWithGluePulsing(bool dir, long steps) {
  if (steps <= 0 || stopRequested) return false;

  digitalWrite(X_DIR_PIN, dir ? HIGH : LOW);
  delayMicroseconds(20); 

  int currentMinDelay = dir ? minDelayUs : 30; 
  int currentMaxDelay = dir ? maxDelayUs : 80;

  long accelSteps = steps / 4;
  if (accelSteps > 500) accelSteps = 500;

  digitalWrite(GLUE_DISPENSER_PIN, LOW); 
  isDispensingGlue = true;
  unsigned long lastToggleTime = millis();

  for (long i = 0; i < steps; i++) {
    if (checkGlobalCommands()) {
      digitalWrite(GLUE_DISPENSER_PIN, HIGH);
      isDispensingGlue = false;
      return false; 
    }

    unsigned long now = millis();
    if (isDispensingGlue && (now - lastToggleTime >= GLUE_ON_TIME_MS)) {
      digitalWrite(GLUE_DISPENSER_PIN, HIGH);
      isDispensingGlue = false;
      lastToggleTime = now;
    } else if (!isDispensingGlue && (now - lastToggleTime >= GLUE_OFF_TIME_MS)) {
      digitalWrite(GLUE_DISPENSER_PIN, LOW);
      isDispensingGlue = true;
      lastToggleTime = now;
    }

    int currentDelay = currentMinDelay;
    if (accelSteps > 0) {
      if (i < accelSteps) {
        currentDelay = currentMaxDelay - ((currentMaxDelay - currentMinDelay) * i / accelSteps);
      } else if (i > steps - accelSteps) {
        currentDelay = currentMinDelay + ((currentMaxDelay - currentMinDelay) * (i - (steps - accelSteps)) / accelSteps);
      }
    }

    digitalWrite(X_STEP_PIN, HIGH); delayMicroseconds(currentDelay);
    digitalWrite(X_STEP_PIN, LOW);  delayMicroseconds(currentDelay);
    currentXSteps += (dir ? 1 : -1); 
  }

  digitalWrite(GLUE_DISPENSER_PIN, HIGH);
  isDispensingGlue = false;

  return true;
}

// БЪРЗА HOME ФУНКЦИЯ (25us)
bool returnToHomePosition() {
  Serial.println(F("📍 Бързо калибриране на HOME position (25us)..."));
  stopRequested = false;
  isHoming = true; 

  // 1. Калибриране по X
  digitalWrite(X_DIR_PIN, LOW); 
  delayMicroseconds(20);
  
  while (digitalRead(X_MIN_PIN) == HIGH) { 
    digitalWrite(X_STEP_PIN, HIGH); delayMicroseconds(25);
    digitalWrite(X_STEP_PIN, LOW);  delayMicroseconds(25);

    if (Serial.available() > 0 && Serial.read() == ' ') {
      Serial.println(F("🛑 Авариен стоп по време на HOME по X!"));
      isHoming = false;
      return false;
    }
  }
  delay(50);
  if (!moveStepsControlled('X', true, HOME_OFFSET_X_MM * STEPS_PER_MM_X)) {
    isHoming = false;
    return false;
  }

  // 2. Калибриране по Y
  digitalWrite(Y_DIR_PIN, LOW); 
  delayMicroseconds(20);

  while (digitalRead(Y_MIN_PIN) == HIGH) { 
    digitalWrite(Y_STEP_PIN, HIGH); delayMicroseconds(25);
    digitalWrite(Y_STEP_PIN, LOW);  delayMicroseconds(25);

    if (Serial.available() > 0 && Serial.read() == ' ') {
      Serial.println(F("🛑 Авариен стоп по време на HOME по Y!"));
      isHoming = false;
      return false;
    }
  }
  delay(50);
  if (!moveStepsControlled('Y', true, HOME_OFFSET_Y_MM * STEPS_PER_MM_Y)) {
    isHoming = false;
    return false;
  }

  currentXSteps = 0; 
  currentYSteps = 0;
  isHoming = false; 
  Serial.println(F("✅ HOME позиция установена успешно!\n"));
  return true;
}

bool runUVCuring(int seconds) {
  Serial.print(F("   ☀️ UV изпичане ("));
  Serial.print(seconds);
  Serial.println(F(" сек)..."));

  isUVCuringActive = true;
  sendRelayPulse(); 

  for (int i = 0; i < seconds * 10; i++) {
    if (checkGlobalCommands()) {
      if (stopRequested) {
        if (isUVCuringActive) sendRelayPulse(); 
        isUVCuringActive = false;
        return false;
      }
    }
    delay(100);
  }

  sendRelayPulse(); 
  isUVCuringActive = false;
  return true;
}

void runSingleExecutionPattern() {
  isRunning = true;
  stopRequested = false;
  isPaused = false;

  long xStepsLine   = RECT_WIDTH_MM * STEPS_PER_MM_X;
  long xStepsUV     = UV_OFFSET_X_MM * STEPS_PER_MM_X;

  Serial.println(F("\n🚀 [СТАРТ] Работен цикъл..."));

  // === ПЪРВИ ПРАВОЪГЪЛНИК ===
  Serial.println(F("=== ПЪРВИ ПРАВОЪГЪЛНИК ==="));

  for (int pass = 1; pass <= 2; pass++) {
    Serial.print(F("   --- Пасаж ")); Serial.print(pass); Serial.println(F(" от 2 ---"));
    
    if (!moveStepsWithGluePulsing(true, xStepsLine)) goto abortSeq;
    if (!moveStepsWithGluePulsing(false, xStepsLine)) goto abortSeq;
    delay(50);

    if (!moveStepsControlled('X', false, xStepsUV)) goto abortSeq;
    if (!runUVCuring(UV_TIME_SEC)) goto abortSeq;
    
    if (!moveStepsControlled('X', true, xStepsUV)) goto abortSeq;
  }

  // === ПРЕХОД КЪМ ВТОРИЯ ПРАВОЪГЪЛНИК ===
  Serial.println(F("\n  -> Преход напред към втория правоъгълник..."));
  if (!moveStepsControlled('X', true, TRANSITION_DIST_MM * STEPS_PER_MM_X)) goto abortSeq;

  // === ВТОРИ ПРАВОЪГЪЛНИК ===
  Serial.println(F("=== ВТОРИ ПРАВОЪГЪЛНИК ==="));

  for (int pass = 1; pass <= 2; pass++) {
    Serial.print(F("   --- Пасаж ")); Serial.print(pass); Serial.println(F(" от 2 ---"));
    
    if (!moveStepsWithGluePulsing(true, xStepsLine)) goto abortSeq;
    if (!moveStepsWithGluePulsing(false, xStepsLine)) goto abortSeq;
    delay(50);

    if (!moveStepsControlled('X', false, xStepsUV)) goto abortSeq;
    if (!runUVCuring(UV_TIME_SEC)) goto abortSeq;

    if (pass == 1) {
      if (!moveStepsControlled('X', true, xStepsUV)) goto abortSeq;
    }
  }

  // === ФИНАЛ ===
  if (!returnToHomePosition()) goto abortSeq;

  Serial.println(F("\n✅ РАБОТНИЯТ ЦИКЪЛ ЗАВЪРШИ УСПЕШНО!"));
  isRunning = false;
  return;

abortSeq:
  digitalWrite(GLUE_DISPENSER_PIN, HIGH);
  if (isUVCuringActive) {
    sendRelayPulse(); 
    isUVCuringActive = false;
  }
  
  Serial.println(F("🏠 Връщане в Home поради прекъсване..."));
  returnToHomePosition();

  isRunning = false;
  Serial.println(F("⚠️ Процесът беше прекратен!"));
}