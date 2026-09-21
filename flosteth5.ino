#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

// 74HC595 pins (ESP32 -> 74HC595)
static const int PIN_SR_DATA  = 32;  // DS
static const int PIN_SR_CLOCK = 25;  // SHCP
static const int PIN_SR_LATCH = 33;   // STCP

// Digit select pins (ESP32 -> digit commons)
static const int PIN_DIGIT_1 = 26;   // leftmost or whatever you wired
static const int PIN_DIGIT_2 = 27;
static const int PIN_DIGIT_3 = 14;
static const int PIN_DIGIT_4 = 13;

// Front buttons (active LOW with internal pull-ups)
static const int PIN_COUNT_BUTTON = 18;  // right button: show floss count
static const int PIN_FLOSS_BUTTON = 19;  // left button: record flossing / start timer

// Persistent floss counter (ESP32 NVS)
static const char* NVS_NAMESPACE = "flosteth";
static const char* NVS_COUNT_KEY = "floss_count";
static const char* NVS_BOOT_ARMED_KEY = "boot_armed";
static const uint32_t MAX_FLOSS_COUNT = 9999;  // four-digit display limit
static const uint32_t QUICK_RESET_WINDOW_MS = 5000;

Preferences preferences;
static uint32_t counter = 0;
static bool nvsReady = false;
static bool quickResetWindowActive = false;
static bool counterResetThisBoot = false;
static uint32_t quickResetWindowStart = 0;

void serviceQuickResetWindow();
void delayWithServices(uint32_t duration_ms);

static const bool COMMON_ANODE_SEGMENTS = false; // your tests: HIGH = segment ON
static const bool SHIFT_LSBFIRST = true;         // keep unless your wiring order differs

// Flip this if digits don’t enable correctly.
// false => LOW = digit ON, true => HIGH = digit ON
static const bool DIGIT_ON_IS_HIGH = false;

static const uint16_t DIGIT_ON_US = 300;
static const int DIGIT_PINS[4] = { PIN_DIGIT_1, PIN_DIGIT_2, PIN_DIGIT_3, PIN_DIGIT_4 };

// Your discovered mapping: Q0..Q7 -> (a,b,c,d,e,f,g,dp indices)
static int qToSegIndex[8] = { 4, 3, 7, 2, 6, 1, 5, 0 };

// Logical segment indices: 0=a,1=b,2=c,3=d,4=e,5=f,6=g,7=dp
static const uint8_t DIGIT_LOGICAL_CC[10] = {
  0b00111111, // 0
  0b00000110, // 1
  0b01011011, // 2
  0b01001111, // 3
  0b01100110, // 4
  0b01101101, // 5
  0b01111101, // 6
  0b00000111, // 7
  0b01111111, // 8
  0b01101111  // 9
};

// Logical segment masks for letters A–Z
// bit0=a, bit1=b, bit2=c, bit3=d, bit4=e, bit5=f, bit6=g, bit7=dp
static const uint8_t ALPHA_LOGICAL_CC[26] = {
  0b01110111, // A  a b c e f g
  0b01111100, // b  c d e f g
  0b00111001, // C  a d e f
  0b01011110, // d  b c d e g
  0b01111001, // E  a d e f g
  0b01110001, // F  a e f g
  0b00111101, // G  a c d e f
  0b01110110, // H  b c e f g
  0b00000110, // I  b c
  0b00011110, // J  b c d e
  0b01110110, // K* ≈ H
  0b00111000, // L  d e f
  0b00010101, // M* ≈ n (c e g)
  0b01010100, // n  c e g
  0b00111111, // O  a b c d e f
  0b01110011, // P  a b e f g
  0b01100111, // Q* ≈ q (a b c f g)
  0b01010000, // r  e g
  0b01101101, // S  a c d f g
  0b01111000, // t  d e f g
  0b00111110, // U  b c d e f
  0b00011100, // v  c d e
  0b00101010, // W* ≈ u (b d f)
  0b01110110, // X* ≈ H
  0b01101110, // Y  b c d f g
  0b01011011  // Z  a b d e g
};

static inline uint8_t applySegmentPolarity(uint8_t seg_cc_bits) {
  return COMMON_ANODE_SEGMENTS ? (uint8_t)~seg_cc_bits : seg_cc_bits;
}

static inline void srWriteByte(uint8_t value) {
  digitalWrite(PIN_SR_LATCH, LOW);
  if (SHIFT_LSBFIRST) shiftOut(PIN_SR_DATA, PIN_SR_CLOCK, LSBFIRST, value);
  else                shiftOut(PIN_SR_DATA, PIN_SR_CLOCK, MSBFIRST, value);
  digitalWrite(PIN_SR_LATCH, HIGH);
}

static inline void allDigitsOff() {
  for (int i = 0; i < 4; i++) digitalWrite(DIGIT_PINS[i], DIGIT_ON_IS_HIGH ? LOW : HIGH);
}

static inline void digitOn(int idx) {
  digitalWrite(DIGIT_PINS[idx], DIGIT_ON_IS_HIGH ? HIGH : LOW);
}

uint8_t logicalSegMask_to_QMask(uint8_t logicalMask) {
  uint8_t qMask = 0;
  for (int q = 0; q < 8; q++) {
    int seg = qToSegIndex[q];
    if (logicalMask & (1u << seg)) qMask |= (1u << q);
  }
  return qMask;
}

void multiplexNumber(uint32_t value, uint32_t duration_ms) {
  int d[4];
  d[0] = (value / 1000) % 10;
  d[1] = (value / 100)  % 10;
  d[2] = (value / 10)   % 10;
  d[3] = value % 10;

  uint32_t start = millis();
  while (millis() - start < duration_ms) {
    serviceQuickResetWindow();
    for (int i = 0; i < 4; i++) {
      allDigitsOff();
      uint8_t qMaskCC = logicalSegMask_to_QMask(DIGIT_LOGICAL_CC[d[i]]);
      srWriteByte(applySegmentPolarity(qMaskCC));
      digitOn(i);
      delayMicroseconds(DIGIT_ON_US);
    }
  }
  allDigitsOff();
}

// 4 digits worth of logical CC masks (bit0=a ... bit7=dp)
volatile uint8_t dispBuf[4] = {0, 0, 0, 0};  // left->right

// Call this to show current display
void renderBufferOnce(uint32_t duration_ms) {
  uint32_t start = millis();
  while (millis() - start < duration_ms) {
    serviceQuickResetWindow();
    for (int i = 0; i < 4; i++) {
      allDigitsOff();
      uint8_t qMaskCC = logicalSegMask_to_QMask(dispBuf[i]);
      srWriteByte(applySegmentPolarity(qMaskCC));
      digitOn(i);
      delayMicroseconds(DIGIT_ON_US);
    }
  }
  allDigitsOff();
}



uint8_t charToSeg(char ch) {
  if (ch >= '0' && ch <= '9') return DIGIT_LOGICAL_CC[ch - '0'];

  ch = toupper((unsigned char)ch);
  if (ch >= 'A' && ch <= 'Z') return ALPHA_LOGICAL_CC[ch - 'A'];

  if (ch == '-') return 0b01000000; // g segment only
  if (ch == ' ') return 0x00; 

  return 0x00; // unsupported chars blank
}

void setWord4(const char* s4) {
  for (int i = 0; i < 4; i++) {
    dispBuf[i] = charToSeg(s4[i]);
  }
}

void beginCounterStorage() {
  nvsReady = preferences.begin(NVS_NAMESPACE, false);
  if (!nvsReady) {
    Serial.println("NVS unavailable; floss count will be RAM-only.");
    counter = 0;
    return;
  }

  counter = preferences.getUInt(NVS_COUNT_KEY, 0);

  // Keep the stored value representable on the four-digit display.
  if (counter > MAX_FLOSS_COUNT) {
    counter = MAX_FLOSS_COUNT;
    preferences.putUInt(NVS_COUNT_KEY, counter);
  }

  // A marker left armed by the previous boot means power returned within the
  // quick-reset window. Reset the persistent counter on this second boot.
  bool previousBootWasArmed = preferences.getUChar(NVS_BOOT_ARMED_KEY, 0) != 0;
  if (previousBootWasArmed) {
    counter = 0;
    counterResetThisBoot = true;

    if (preferences.putUInt(NVS_COUNT_KEY, counter) != sizeof(counter)) {
      Serial.println("Failed to reset floss count in NVS.");
    }
    if (preferences.putUChar(NVS_BOOT_ARMED_KEY, 0) != sizeof(uint8_t)) {
      Serial.println("Failed to clear quick-reset marker in NVS.");
    }

    Serial.println("Quick double power-cycle detected; floss count reset to 0.");
  } else {
    // Arm this boot. Stable operation for QUICK_RESET_WINDOW_MS clears it.
    if (preferences.putUChar(NVS_BOOT_ARMED_KEY, 1) == sizeof(uint8_t)) {
      quickResetWindowStart = millis();
      quickResetWindowActive = true;
    } else {
      Serial.println("Failed to arm quick-reset marker in NVS.");
    }
  }

  Serial.printf("Loaded floss count from NVS: %lu\n", (unsigned long)counter);
}

void serviceQuickResetWindow() {
  if (!nvsReady || !quickResetWindowActive) return;
  if (millis() - quickResetWindowStart < QUICK_RESET_WINDOW_MS) return;

  if (preferences.putUChar(NVS_BOOT_ARMED_KEY, 0) != sizeof(uint8_t)) {
    Serial.println("Failed to disarm quick-reset marker in NVS.");
  } else {
    Serial.println("Quick power-cycle reset window closed.");
  }

  quickResetWindowActive = false;
}

void delayWithServices(uint32_t duration_ms) {
  uint32_t start = millis();
  while (millis() - start < duration_ms) {
    serviceQuickResetWindow();
    delay(10);
  }
}

void incrementFlossCounter() {
  if (counter >= MAX_FLOSS_COUNT) {
    Serial.println("Floss count is already at the display maximum (9999).");
    return;
  }

  counter++;

  if (nvsReady) {
    size_t bytesWritten = preferences.putUInt(NVS_COUNT_KEY, counter);
    if (bytesWritten != sizeof(counter)) {
      Serial.println("Failed to save floss count to NVS.");
    }
  }

  Serial.printf("Floss count: %lu\n", (unsigned long)counter);
}


void setup() {
  WiFi.mode(WIFI_OFF);
  btStop();
  setCpuFrequencyMhz(20);
  Serial.begin(115200);

  beginCounterStorage();


  pinMode(PIN_SR_DATA, OUTPUT);
  pinMode(PIN_SR_CLOCK, OUTPUT);
  pinMode(PIN_SR_LATCH, OUTPUT);

  for (int i = 0; i < 4; i++) pinMode(DIGIT_PINS[i], OUTPUT);


  pinMode(PIN_COUNT_BUTTON, INPUT_PULLUP);
  pinMode(PIN_FLOSS_BUTTON, INPUT_PULLUP);
  allDigitsOff();
  srWriteByte(COMMON_ANODE_SEGMENTS ? 0xFF : 0x00);

  if (counterResetThisBoot) {
    setWord4("CLr ");
    renderBufferOnce(1500);
    srWriteByte(0x00);
    delayWithServices(100);
    showCount(0);
  }
}

void showCount(uint32_t count) {
  setWord4("PUSH"); 
  renderBufferOnce(1000);
  srWriteByte(0x00);
  delayWithServices(100);
  setWord4("HITS"); 
  renderBufferOnce(1000);
  srWriteByte(0x00);
  delayWithServices(100);
  multiplexNumber(count, 2000);
  srWriteByte(0x00);
  delayWithServices(2000);
}

void loop() {
  /*
  floss teeth repeat otherwise:
  pin 18 button for showing persistent floss count from NVS
  pin 19 button for recording flossing and starting the 18-hour delay
  */
  unsigned long start = 0;
  if (digitalRead(PIN_COUNT_BUTTON) != LOW && digitalRead(PIN_FLOSS_BUTTON) != LOW) {
    Serial.println("on");
    setWord4("Flos"); 
    renderBufferOnce(1000);
    delayWithServices(100);
    setWord4("Teth"); 
    renderBufferOnce(1000);
    srWriteByte(0x00);
    delayWithServices(2000);
  } else if (digitalRead(PIN_COUNT_BUTTON) == LOW) {
    showCount(counter);
  } else if (digitalRead(PIN_FLOSS_BUTTON) == LOW) {
    incrementFlossCounter();
    setWord4("SEE "); 
    renderBufferOnce(1000);
    srWriteByte(0x00);
    delayWithServices(100);
    setWord4("YOU "); 
    renderBufferOnce(1000);
    srWriteByte(0x00);
    delayWithServices(100);
    setWord4("NEXT"); 
    renderBufferOnce(1000);
    srWriteByte(0x00);
    delayWithServices(100);
    setWord4("DAY "); 
    renderBufferOnce(1000);
    srWriteByte(0x00);
    delayWithServices(100);
    allDigitsOff();
    srWriteByte(0x00);
    start = millis();
    while (millis() - start < 64800000) {//64800000
      serviceQuickResetWindow();
      if (digitalRead(PIN_COUNT_BUTTON) == LOW){
        showCount(counter);
      }
      allDigitsOff();
      srWriteByte(0x00);
    }
    
  } else {
    Serial.println("off");
    allDigitsOff();
    srWriteByte(0x00);
    delayWithServices(2000);
  }

}
