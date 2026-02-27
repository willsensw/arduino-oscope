// DIY ARDUINO OSCILLOSCOPE - FIRMWARE V3.6 (By WillsenSW)

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ==========================================
// 1. PIN DEFINITIONS
// ==========================================
#define TFT_CS     10
#define TFT_RST    7
#define TFT_DC     8
#define ADC_CS     9 

#define ENC_A      2
#define ENC_B      3
#define ENC_BTN    4   
#define BTN_HOLD   5   
#define BTN_MENU   6  

#define PIN_CAL    A0 

// ==========================================
// 2. CONFIGURATION
// ==========================================
#define COLOR_BG   ST7735_BLACK
#define COLOR_GRID 0x9492 
#define COLOR_CH1  ST7735_YELLOW
#define COLOR_CH2  ST7735_CYAN
#define COLOR_TXT  ST7735_WHITE
#define COLOR_ACC  ST7735_MAGENTA 
#define COLOR_SEL  ST7735_GREEN 

#define SCREEN_W   160
#define SCREEN_H   128
#define TOP_H      16    
#define BOT_H      16    
#define GRID_DIV   25    
#define Y_CENTER   64    
#define SMP_COUNT  160   

#define V_REF      5.23  
#define ADC_RES    1023.0

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// Buffers (8-bit for RAM savings)
uint8_t ch1_curr[SMP_COUNT];
uint8_t ch2_curr[SMP_COUNT];
uint8_t ch1_prev[SMP_COUNT];
uint8_t ch2_prev[SMP_COUNT];

// --- APP MODES ---
enum AppMode { MODE_SCOPE, MODE_MENU };
volatile AppMode currentMode = MODE_SCOPE;

// --- SCOPE SETTINGS ---
int scopeFocus = 0; 
const int timeDelays[] = {0, 5, 10, 20, 50, 100, 200, 500, 1000}; 
const char* timeNames[] = {"4us", "10us", "20us", "50us", "100us", "200us", "500us", "1ms", "2ms"};
const float timePerSampleUs[] = {4.5, 13, 22, 38, 70, 120, 230, 550, 1100}; 
const int timeMax = 8;
int timeIdx = 3; 

const int probeVals[] = {1, 10};
const char* probeNames[] = {"1X ", "10X"};
int probeIdx = 0; 

const char* freqNames[] = {"F:CH1", "F:CH2"};
int freqSource = 0; 

// --- MENU SETTINGS ---
bool showCH1 = true;
bool showCH2 = true;
int menuSelectIdx = 0; 
bool menuNeedsFullDraw = true;

// --- FLAGS & TIMERS ---
bool holdMode = false;
bool uiDirty = true; 
unsigned long lastMeasureTime = 0;
float measV1 = 0.0;
float measV2 = 0.0;
float measFreq = 0.0;

// --- DECOUPLED INPUT VARIABLES ---
volatile int8_t encoderDelta = 0; 
volatile int8_t enc_count = 0;    
volatile unsigned long lastEncTime = 0; 

bool lastEncBtnState = HIGH;      
unsigned long lastBtnDebounceTime = 0;
unsigned long lastMenuBtnDebounce = 0;
unsigned long lastHoldBtnDebounce = 0;

// ==========================================
// 3. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  tft.initR(INITR_BLACKTAB); 
  tft.setRotation(1);
  tft.fillScreen(COLOR_BG);
  
  pinMode(ADC_CS, OUTPUT);
  digitalWrite(ADC_CS, HIGH);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_BTN, INPUT_PULLUP);
  pinMode(BTN_HOLD, INPUT_PULLUP);
  pinMode(BTN_MENU, INPUT_PULLUP); 
  pinMode(PIN_CAL, OUTPUT);

  playBootAnimation();

  tone(PIN_CAL, 1000); 

  attachInterrupt(digitalPinToInterrupt(ENC_A), readEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), readEncoderISR, CHANGE);

  SPI.begin();
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));

  for(int i=0; i<SMP_COUNT; i++) {
    ch1_prev[i] = 255; 
    ch2_prev[i] = 255;
  }

  drawGridAndBars();
  updateTopUI();
  updateBottomUI();
}

// ==========================================
// 4. MAIN LOOP
// ==========================================
void loop() {
  handleInputs(); 

  if (currentMode == MODE_MENU) {
    if (menuNeedsFullDraw) {
       drawMenuPage();
       menuNeedsFullDraw = false;
       uiDirty = false;
    }
    else if (uiDirty) {
      drawMenuItems(); 
      uiDirty = false;
    }
    delay(30); 
    return;
  }

  // --- SCOPE MODE LOGIC ---
  if (uiDirty) {
    updateTopUI();
    updateBottomUI(); 
    uiDirty = false;
  }

  if (holdMode) return;

  unsigned long timeout = micros() + 20000;
  int triggerLevel = 512; 
  while (readADC(0) > triggerLevel && micros() < timeout) if(encoderDelta != 0 || digitalRead(ENC_BTN)==LOW) return; 
  while (readADC(0) < triggerLevel && micros() < timeout) if(encoderDelta != 0 || digitalRead(ENC_BTN)==LOW) return;

  int delayTime = timeDelays[timeIdx];
  int vMin1 = 1023, vMax1 = 0;
  int vMin2 = 1023, vMax2 = 0;

  for (int x = 0; x < SMP_COUNT; x++) {
    if (encoderDelta != 0 || currentMode == MODE_MENU) return; 

    int val1 = readADC(0);
    int val2 = readADC(1);

    if (showCH1) {
       if (val1 < vMin1) vMin1 = val1;
       if (val1 > vMax1) vMax1 = val1;
       ch1_curr[x] = adcToScreenY(val1); 
    } else ch1_curr[x] = 255; 

    if (showCH2) {
       if (val2 < vMin2) vMin2 = val2;
       if (val2 > vMax2) vMax2 = val2;
       ch2_curr[x] = adcToScreenY(val2);
    } else ch2_curr[x] = 255; 

    if (delayTime > 0) delayMicroseconds(delayTime);
  }

  if (millis() - lastMeasureTime > 300) {
    if(showCH1) measV1 = ((vMax1 * V_REF) / ADC_RES) * probeVals[probeIdx];
    else measV1 = 0;
    
    if(showCH2) measV2 = ((vMax2 * V_REF) / ADC_RES) * probeVals[probeIdx];
    else measV2 = 0;

    calculateFreq();
    updateBottomUI();
    lastMeasureTime = millis();
  }

  // --- DRAWING WAVEFORMS ---
  for (int x = 1; x < SMP_COUNT; x++) {
    if (encoderDelta != 0) return; 

    if (ch1_prev[x-1] != 255 && ch1_prev[x] != 255) 
       tft.drawLine(x - 1, ch1_prev[x-1], x, ch1_prev[x], COLOR_BG);
    if (ch2_prev[x-1] != 255 && ch2_prev[x] != 255) 
       tft.drawLine(x - 1, ch2_prev[x-1], x, ch2_prev[x], COLOR_BG);

    restoreGridForX(x - 1);
    restoreGridForX(x);

    uint8_t y1_new = ch1_curr[x];
    uint8_t y2_new = ch2_curr[x];
    uint8_t y1_old = ch1_curr[x-1];
    uint8_t y2_old = ch2_curr[x-1];

    if (showCH1 && y1_old != 255 && y1_new != 255) 
       tft.drawLine(x - 1, y1_old, x, y1_new, COLOR_CH1);
    if (showCH2 && y2_old != 255 && y2_new != 255) 
       tft.drawLine(x - 1, y2_old, x, y2_new, COLOR_CH2);

    ch1_prev[x-1] = y1_old; 
    ch2_prev[x-1] = y2_old;
    if (x == SMP_COUNT - 1) {
       ch1_prev[x] = y1_new;
       ch2_prev[x] = y2_new;
    }
  }
}

// ==========================================
// 5. INPUT HANDLING (ANTI-CROSSTALK LOGIC)
// ==========================================
void handleInputs() {
  unsigned long now = millis();

  // ----------------------------------------
  // ACTION 1: ROTATE RIGHT 
  // ----------------------------------------
  if (encoderDelta > 0) {
    encoderDelta = 0; 
    if (currentMode == MODE_SCOPE) {
       if (scopeFocus == 0)      { timeIdx++; if(timeIdx > timeMax) timeIdx = timeMax; }
       else if (scopeFocus == 1) { probeIdx = 1; }
       else if (scopeFocus == 2) { freqSource = 1; }
    } else {
       menuSelectIdx++;
       if (menuSelectIdx > 2) menuSelectIdx = 0;
    }
    uiDirty = true;
  }
  
  // ----------------------------------------
  // ACTION 2: ROTATE LEFT
  // ----------------------------------------
  else if (encoderDelta < 0) {
    encoderDelta = 0; 
    if (currentMode == MODE_SCOPE) {
       if (scopeFocus == 0)      { timeIdx--; if(timeIdx < 0) timeIdx = 0; }
       else if (scopeFocus == 1) { probeIdx = 0; }
       else if (scopeFocus == 2) { freqSource = 0; }
    } else {
       menuSelectIdx--;
       if (menuSelectIdx < 0) menuSelectIdx = 2;
    }
    uiDirty = true;
  }

  // ----------------------------------------
  // ACTION 3: ENCODER BUTTON (With Vibration Lockout)
  // ----------------------------------------
  bool currentEncBtnState = digitalRead(ENC_BTN);
  
  if (currentEncBtnState == LOW && lastEncBtnState == HIGH && (now - lastBtnDebounceTime > 50)) {
     if (now - lastEncTime > 250) { 
         lastBtnDebounceTime = now;
         
         if (currentMode == MODE_SCOPE) {
            scopeFocus++;
            if (scopeFocus > 2) scopeFocus = 0; 
         } else {
            executeMenuAction();
         }
         uiDirty = true;
     }
  }
  lastEncBtnState = currentEncBtnState;

  // ----------------------------------------
  // ACTION 4: MENU BUTTON
  // ----------------------------------------
  if (digitalRead(BTN_MENU) == LOW && (now - lastMenuBtnDebounce > 250)) {
    lastMenuBtnDebounce = now;
    if (currentMode == MODE_SCOPE) {
      currentMode = MODE_MENU; 
      menuNeedsFullDraw = true;
    } else {
      currentMode = MODE_SCOPE; 
      tft.fillScreen(COLOR_BG); 
      drawGridAndBars();        
    }
    uiDirty = true;
  }

  // ----------------------------------------
  // ACTION 5: HOLD BUTTON
  // ----------------------------------------
  if (currentMode == MODE_SCOPE && digitalRead(BTN_HOLD) == LOW && (now - lastHoldBtnDebounce > 250)) {
    lastHoldBtnDebounce = now;
    holdMode = !holdMode;
    uiDirty = true;
  }
}

// ==========================================
// 6. HARDWARE INTERRUPT (Strict Rate Limit)
// ==========================================
void readEncoderISR() {
  static const int8_t enc_states[] = {0,-1,1,0,1,0,0,-1,-1,0,0,1,0,1,-1,0};
  static uint8_t old_AB = 0;
  
  old_AB <<= 2; 
  old_AB |= ((PIND & 0x0C) >> 2); 
  int8_t val = enc_states[old_AB & 0x0F];
  
  if (val != 0) {
     lastEncTime = millis(); 
     enc_count += val; 

     if (enc_count >= 4) {
        encoderDelta = 1;  
        enc_count = 0;
     } 
     else if (enc_count <= -4) {
        encoderDelta = -1; 
        enc_count = 0;
     }
  }
}

// ==========================================
// 7. BOOT ANIMATION (Custom WillsenSW)
// ==========================================
void playBootAnimation() {
  tft.fillScreen(COLOR_BG);
  
  const char* line1 = "Arduino - Oscilloscope";
  const char* line2 = "By WillsenSW";
  
  tft.setTextSize(1); // Smaller text to fit "Arduino - Oscilloscope"
  tft.setTextColor(ST7735_WHITE);

  // Line 1: Typewriter Effect
  // Center roughly: 160px width. Text is ~21 chars. 
  // 21 * 6px = ~126px. Start X around 17.
  int xStart1 = 17; 
  int yStart1 = 50; 
  
  for(int i=0; i<22; i++) { // Length of line 1
    if(line1[i] == '\0') break;
    tft.setCursor(xStart1 + (i*6), yStart1);
    tft.print(line1[i]);
    delay(50); 
  }

  // Line 2: Typewriter Effect
  // "By WillsenSW" is 12 chars. 12 * 6px = 72px.
  // Center: (160 - 72) / 2 = 44.
  int xStart2 = 44;
  int yStart2 = 70;

  for(int i=0; i<12; i++) { // Length of line 2
    if(line2[i] == '\0') break;
    tft.setCursor(xStart2 + (i*6), yStart2);
    tft.print(line2[i]);
    delay(100); 
  }

  delay(1000); // Pause to read
  tft.fillScreen(COLOR_BG); 
}

// ==========================================
// 8. MENU LOGIC
// ==========================================
void drawMenuPage() {
  tft.fillScreen(COLOR_BG);
  tft.drawRect(10, 10, SCREEN_W-20, SCREEN_H-20, ST7735_WHITE);
  
  tft.setTextSize(2);
  tft.setCursor(35, 20);
  tft.setTextColor(COLOR_TXT);
  tft.print(F("OPTIONS"));

  drawMenuItems();
}

void drawMenuItems() {
  tft.setTextSize(1);
  int startY = 55;
  int gap = 20;

  for(int i=0; i<3; i++) {
    int y = startY + (i * gap);
    
    tft.fillRect(15, y, 130, 10, COLOR_BG);

    int xOffset = (menuSelectIdx == i) ? 35 : 25; 
    tft.setCursor(xOffset, y);

    if (menuSelectIdx == i) {
      tft.setTextColor(COLOR_SEL);
      tft.print(F("> ")); 
    } else {
      tft.setTextColor(COLOR_TXT);
    }

    if (i == 0)      tft.print(showCH1 ? F("[X] CH1 Enable") : F("[ ] CH1 Enable"));
    else if (i == 1) tft.print(showCH2 ? F("[X] CH2 Enable") : F("[ ] CH2 Enable"));
    else if (i == 2) tft.print(F("AUTOSET"));
  }
}

void executeMenuAction() {
  if (menuSelectIdx == 0) showCH1 = !showCH1;
  else if (menuSelectIdx == 1) showCH2 = !showCH2;
  else if (menuSelectIdx == 2) runAutoset();
}

void runAutoset() {
  tft.fillScreen(COLOR_BG);
  tft.setCursor(40, 60);
  tft.setTextColor(COLOR_CH1);
  tft.print(F("Scanning..."));

  timeIdx = 6;  
  freqSource = 0; 
  
  delay(500); 

  currentMode = MODE_SCOPE;
  tft.fillScreen(COLOR_BG);
  drawGridAndBars();
  uiDirty = true;
}

// ==========================================
// 9. HELPER FUNCTIONS
// ==========================================
uint8_t adcToScreenY(int adcVal) {
  float voltage = (adcVal * V_REF) / ADC_RES;
  int zeroLineY = Y_CENTER + GRID_DIV; 
  float pxPerVolt = 10.0; 
  int y = zeroLineY - (int)(voltage * pxPerVolt);
  return constrain(y, TOP_H + 1, SCREEN_H - BOT_H - 1);
}

void calculateFreq() {
  if (!showCH1 && !showCH2) { measFreq=0; return; }
  
  int firstCross = -1, secondCross = -1;
  uint8_t center = adcToScreenY(512);
  uint8_t* buffer = (freqSource == 0) ? ch1_curr : ch2_curr;

  if (!showCH1 && showCH2) buffer = ch2_curr;

  for(int i=1; i<SMP_COUNT-1; i++) {
    if(buffer[i] == 255 || buffer[i+1] == 255) continue;
    
    if(buffer[i] >= center && buffer[i+1] < center) {
      if(firstCross == -1) firstCross = i;
      else { secondCross = i; break; }
    }
  }
  if(secondCross != -1 && firstCross != -1) {
    int diff = secondCross - firstCross;
    float periodUs = diff * timePerSampleUs[timeIdx];
    measFreq = 1000000.0 / periodUs; 
  } else measFreq = 0; 
}

int readADC(int channel) {
  digitalWrite(ADC_CS, LOW);
  SPI.transfer(0x01); 
  byte config = (1 << 7) | (channel << 6) | (1 << 5);
  byte msb = SPI.transfer(config); 
  byte lsb = SPI.transfer(0x00);
  digitalWrite(ADC_CS, HIGH);
  return ((msb & 0x03) << 8) | lsb;
}

void updateTopUI() {
  tft.fillRect(0, 0, SCREEN_W, TOP_H, 0x2104); 
  tft.drawFastHLine(0, TOP_H, SCREEN_W, ST7735_WHITE);
  tft.setTextSize(1);

  uint16_t c1 = (scopeFocus == 0) ? COLOR_ACC : COLOR_TXT;
  uint16_t c2 = (scopeFocus == 1) ? COLOR_ACC : COLOR_TXT;
  uint16_t c3 = (scopeFocus == 2) ? COLOR_ACC : COLOR_TXT;

  tft.setTextColor(c1); tft.setCursor(5, 4); tft.print(timeNames[timeIdx]);
  tft.setTextColor(c2); tft.setCursor(65, 4); tft.print(probeNames[probeIdx]);
  tft.setTextColor(c3); tft.setCursor(120, 4); tft.print(freqNames[freqSource]);
}

void updateBottomUI() {
  tft.fillRect(0, SCREEN_H - BOT_H, SCREEN_W, BOT_H, 0x2104);
  tft.drawFastHLine(0, SCREEN_H - BOT_H - 1, SCREEN_W, ST7735_WHITE);
  tft.setTextSize(1);
  
  if (showCH1) {
    tft.setTextColor(COLOR_CH1);
    tft.setCursor(5, SCREEN_H - 12); tft.print(F("V1=")); tft.print(measV1, 2); 
  }
  
  if (showCH2) {
    tft.setTextColor(COLOR_CH2);
    tft.setCursor(65, SCREEN_H - 12); tft.print(F("V2=")); tft.print(measV2, 2);
  }

  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(125, SCREEN_H - 12); 
  if(measFreq > 999) { tft.print(measFreq/1000.0, 1); tft.print(F("k")); } 
  else { tft.print((int)measFreq); tft.print(F("Hz")); }
}

// ------------------------------------------
// PERFECT GRID FUNCTIONS
// ------------------------------------------
void drawGridAndBars() {
  tft.fillRect(0, TOP_H, SCREEN_W, SCREEN_H - TOP_H - BOT_H, COLOR_BG);
  
  for (int x = 5; x <= 155; x += 25) {
    for (int y = 19; y <= 109; y += 5) tft.drawPixel(x, y, COLOR_GRID);
  }
  for (int y = 39; y <= 89; y += 25) {
    for (int x = 5; x <= 155; x += 5) tft.drawPixel(x, y, COLOR_GRID);
  }

  int zeroLine = Y_CENTER + GRID_DIV;
  tft.drawFastHLine(0, zeroLine, SCREEN_W, 0x52AA); 
  tft.drawFastHLine(0, TOP_H, SCREEN_W, ST7735_WHITE);
  tft.drawFastHLine(0, SCREEN_H - BOT_H - 1, SCREEN_W, ST7735_WHITE);
}

void restoreGridForX(int x) {
  if ((x - 5) % 25 == 0) {
      for (int y = 19; y <= 109; y += 5) {
         if (y != (Y_CENTER + GRID_DIV)) tft.drawPixel(x, y, COLOR_GRID);
      }
  }
  else if ((x - 5) % 5 == 0) {
      tft.drawPixel(x, 39, COLOR_GRID);
      tft.drawPixel(x, 64, COLOR_GRID); 
  }
  int zeroLine = Y_CENTER + GRID_DIV; 
  tft.drawPixel(x, zeroLine, 0x52AA);
}