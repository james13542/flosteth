#include <Arduino.h>
#include <WiFi.h>

// 74HC595 pins (ESP32 -> 74HC595)
static const int PIN_SR_DATA  = 32;  // DS
static const int PIN_SR_CLOCK = 25;  // SHCP
static const int PIN_SR_LATCH = 33;   // STCP

// Digit select pins (ESP32 -> digit commons)
static const int PIN_DIGIT_1 = 26;   // leftmost or whatever you wired
static const int PIN_DIGIT_2 = 27;
static const int PIN_DIGIT_3 = 14;
static const int PIN_DIGIT_4 = 13;

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

void multiplexNumber(int value, uint32_t duration_ms) {
  int d[4];
  d[0] = (value / 1000) % 10;
  d[1] = (value / 100)  % 10;
  d[2] = (value / 10)   % 10;
  d[3] = value % 10;

  uint32_t start = millis();
  while (millis() - start < duration_ms) {
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
void renderBufferOnce(int duration_ms) {
  uint32_t start = millis();
  while (millis() - start < duration_ms) {
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


void setup() {
  WiFi.mode(WIFI_OFF);
  btStop();
  //setCpuFrequencyMhz(20);
  Serial.begin(115200);


  pinMode(PIN_SR_DATA, OUTPUT);
  pinMode(PIN_SR_CLOCK, OUTPUT);
  pinMode(PIN_SR_LATCH, OUTPUT);

  for (int i = 0; i < 4; i++) pinMode(DIGIT_PINS[i], OUTPUT);

  pinMode(23, OUTPUT);
  pinMode(35, INPUT);

  allDigitsOff();
  srWriteByte(COMMON_ANODE_SEGMENTS ? 0xFF : 0x00);
}

void loop() {
  /*
  for (int n = 0; n <= 9999; n++) {
NEW SKETCH

    multiplexNumber(n, 120); // show each number for ~120ms
  }*/
  // Display 'A' on digit 2 for 500 ms
  //showOnDigit(2, ALPHA_LOGICAL_CC['A' - 'A'], 500);
  int value = analogRead(2);
  Serial.println("Analog  Value: ");
  Serial.println(value);
    
  if (value > 900) {
    Serial.println("on");
    setWord4("Flos"); 
    renderBufferOnce(1000);
    delay(100);
    setWord4("Teth"); 
    renderBufferOnce(1000);
    srWriteByte(0x00);
    delay(2000);  
  } else {
    Serial.println("off");
    allDigitsOff();
    srWriteByte(0x00);
    delay(2000);

  }

}
