#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// Use the same display libraries as the Waveshare example
#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include "pin_config.h"  // provided by your Waveshare setup

// Reuse data model and icons from TTGO project
#include "./lib/Information/Information.h"
#include "./lib/Icons/Icons.h"

// BLE UUIDs (unchanged)
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define DESTINATION_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define ETA_UUID "ca83fac2-2438-4d14-a8ae-a01831c0cf0d"
#define DIRECTION_UUID "dfc521a5-ce89-43bd-82a0-28a37f3a2b5a"
#define DIRECTION_DISTANCE_UUID "0343ff39-994e-481b-9136-036dabc02a0b"
#define ETA_MINUTES_UUID "563c187d-ff17-4a6a-8061-ca9b7b70b2b0"
#define DISTANCE_UUID "8bf31540-eb0d-476c-b233-f514678d2afb"
#define DIRECTION_PRECISE_UUID "a602346d-c2bb-4782-8ea7-196a11f85113"

// Instantiate bus and display same as example (QSPI + SH8601)
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS /* CS */, LCD_SCLK /* SCK */, LCD_SDIO0 /* SDIO0 */, LCD_SDIO1 /* SDIO1 */,
  LCD_SDIO2 /* SDIO2 */, LCD_SDIO3 /* SDIO3 */);

Arduino_GFX *gfx = new Arduino_SH8601(bus, -1 /* RST */, 0 /* rotation */, false /* IPS */, LCD_WIDTH, LCD_HEIGHT);

static inline uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Data model
Information destination("destinationdestinationdestination");
Information ETA("00:00");
Information direction("directionsdirections");
Information directionDistance("N/A");
Information ETA_Minute("00 min");
Information distance("100");
Information directionPrecise("34");

// BLE
BLEServer *pServer;
BLEService *pService;

// UI state
static uint16_t bgColor;
volatile long lastDebounceTime = 0;
volatile bool debounce = true;
volatile bool orientation = false;

int scroll_position = 0;
bool scroll_right = true;
unsigned long previousMillis = 0;
const long interval = 150;
const int charsToDisplay = 14;

// Direction line scroll state (independent from destination)
int dir_scroll_position = 0;
bool dir_scroll_right = true;
unsigned long dir_previousMillis = 0;
int dirCharsToDisplay = 0; // computed based on screen width and font size

// Dynamic layout metrics
int SCREEN_W = 0, SCREEN_H = 0;
// Icon placement (computed in layout)
int ICON_X = 0, ICON_Y = 0, ICON_W = 0, ICON_H = 0;
// Text baselines and sizes (new layout)
int SIZE_DEST = 3;       // Destination title size (top of screen)
int SIZE_DIRDIST = 6;    // Large distance to next direction (under icon) — doubled
int SIZE_DIRTXT = 3;     // Direction text
int SIZE_TIMELEFT = 2;   // Time remaining (ETA_Minute) on left
int SIZE_DISTLEFT = 2;   // Distance remaining on right
int SIZE_ETA = 2;        // ETA (clock time) in middle

int Y_TITLE = 0;   // Destination title baseline (top)
int Y_DIRDIST = 0; // Under icon
int Y_DIRTXT = 0;  // Below distance-to-next
int Y_BOTTOM = 0;  // Baseline for bottom row (pinned near bottom)

int X_DEST = 0; // stable X for destination scrolling (title)
int X_DIR = 0;  // stable X for direction scrolling
int X_LEFT_BOX = 0, X_MID_BOX = 0, X_RIGHT_BOX = 0, BOX_W = 0; // bottom row boxes (three columns)

static inline int lineHeight(int size) { return 8 * size + 4; } // default font 6x8 scaled, with padding

void computeLayout() {
  SCREEN_W = gfx->width();
  SCREEN_H = gfx->height();

  // Title at top
  Y_TITLE = 2;
  int titleH = lineHeight(SIZE_DEST);

  // Allocate ~50% of height for icon, capped to 90% of width to keep square
  int maxIconH = (int)(SCREEN_H * 0.5f);
  int maxIconW = (int)(SCREEN_W * 0.9f);
  int target = maxIconH;
  if (target > maxIconW) target = maxIconW;
  if (target < 85) target = 85; // never smaller than source
  ICON_W = target;
  ICON_H = target;
  ICON_X = (SCREEN_W - ICON_W) / 2;
  ICON_Y = Y_TITLE + titleH + 4;

  int y = ICON_Y + ICON_H + 8;
  // Large distance to next, then direction text
  Y_DIRDIST = y; y += lineHeight(SIZE_DIRDIST) + 4;
  Y_DIRTXT = y; y += lineHeight(SIZE_DIRTXT) + 8;
  // Bottom row pinned near bottom, height based on largest of the three sizes
  int bottomH = lineHeight(SIZE_TIMELEFT);
  int h2 = lineHeight(SIZE_DISTLEFT);
  if (h2 > bottomH) bottomH = h2;
  int h3 = lineHeight(SIZE_ETA);
  if (h3 > bottomH) bottomH = h3;
  Y_BOTTOM = SCREEN_H - bottomH - 14; // 14px bottom margin (move row slightly higher again)
  // Three equal boxes across the width
  BOX_W = SCREEN_W / 3;
  X_LEFT_BOX = 0;
  X_MID_BOX = BOX_W;
  X_RIGHT_BOX = 2 * BOX_W;
  // Precompute a stable X for destination so we can redraw without clearing the whole line
  int destW = charsToDisplay * 6 * SIZE_DEST;
  X_DEST = (SCREEN_W - destW) / 2;
  if (X_DEST < 0) X_DEST = 0;
  // Stable X for direction line (same viewport width policy)
  // Compute how many characters fit on one line for direction text; allow longer before scrolling
  dirCharsToDisplay = SCREEN_W / (6 * SIZE_DIRTXT);
  if (dirCharsToDisplay < 14) dirCharsToDisplay = 14; // ensure minimum similar to previous behavior
  int dirW = dirCharsToDisplay * 6 * SIZE_DIRTXT;
  X_DIR = (SCREEN_W - dirW) / 2;
  if (X_DIR < 0) X_DIR = 0;
}

// Basic centered text drawing using default 6px-wide glyphs
void drawTextCentered(int y, const String &text, int size, uint16_t color, uint16_t bg) {
  int textW = (int)text.length() * 6 * size;
  int x = (SCREEN_W - textW) / 2;
  if (x < 0) x = 0;
  int h = 8 * size;
  gfx->fillRect(0, y, SCREEN_W, h + 2, bg);
  gfx->setTextColor(color, bg);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->println(text);
}

// Center text inside a horizontal box
void drawTextCenteredInBox(int x0, int w, int y, const String &text, int size, uint16_t color, uint16_t bg) {
  int textW = (int)text.length() * 6 * size;
  int x = x0 + (w - textW) / 2;
  if (x < x0) x = x0;
  int h = 8 * size;
  gfx->fillRect(x0, y, w, h + 2, bg);
  gfx->setTextColor(color, bg);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->println(text);
}

// Nearest-neighbor scale blit for RGB565 bitmaps
void drawRGB565BitmapScaled(int x, int y, const uint16_t *src, int sw, int sh, int dw, int dh) {
  for (int dy = 0; dy < dh; ++dy) {
    int sy = (long)dy * sh / dh;
    const uint16_t *row = src + sy * sw;
    for (int dx = 0; dx < dw; ++dx) {
      int sx = (long)dx * sw / dw;
      uint16_t c = row[sx];
      gfx->drawPixel(x + dx, y + dy, c);
    }
  }
}

// Forward decl
void drawDirectionImage(const char *direction);
void IRAM_ATTR buttonPressed();

class MyCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    auto uuid = c->getUUID().toString();
  // Use Arduino String for portability (some BLE stacks return Arduino String)
  String s = c->getValue();
  // Only flag as edited if the new value differs to reduce redraw flicker
  if (uuid == DESTINATION_UUID) { if (s != destination.getString()) destination.setString(s.c_str()); }
  else if (uuid == ETA_UUID) { if (s != ETA.getString()) ETA.setString(s.c_str()); }
  else if (uuid == DIRECTION_UUID) { if (s != direction.getString()) direction.setString(s.c_str()); }
  else if (uuid == DIRECTION_DISTANCE_UUID) { if (s != directionDistance.getString()) directionDistance.setString(s.c_str()); }
  else if (uuid == ETA_MINUTES_UUID) { if (s != ETA_Minute.getString()) ETA_Minute.setString(s.c_str()); }
  else if (uuid == DISTANCE_UUID) { if (s != distance.getString()) distance.setString(s.c_str()); }
  else if (uuid == DIRECTION_PRECISE_UUID) { if (s != directionPrecise.getString()) directionPrecise.setString(s.c_str()); }
  }
};

bool ifConnected = false;
bool ifConnectionStateChange = true;

void setup() {
  Serial.begin(921600);
  Serial.println("Start!");

  // Display
  gfx->begin();
  gfx->Display_Brightness(200);
  gfx->setRotation(3); // match TTGO default orientation
  bgColor = 0x0000; // black background
  gfx->fillScreen(bgColor);
  gfx->setTextColor(0xFFFF, bgColor);
  gfx->setTextSize(1);
  computeLayout();

  // BLE
  BLEDevice::init("Navigator");
  pServer = BLEDevice::createServer();
  pService = pServer->createService(SERVICE_UUID);

  pinMode(GPIO_NUM_0, INPUT_PULLUP);
  attachInterrupt(GPIO_NUM_0, buttonPressed, FALLING);

  // Characteristics
  BLECharacteristic *destinationCharacteristic = pService->createCharacteristic(DESTINATION_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  BLECharacteristic *etaCharacteristic = pService->createCharacteristic(ETA_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  BLECharacteristic *directionCharacteristic = pService->createCharacteristic(DIRECTION_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  BLECharacteristic *directionDistanceCharacteristic = pService->createCharacteristic(DIRECTION_DISTANCE_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  BLECharacteristic *etaInMinutesCharacteristic = pService->createCharacteristic(ETA_MINUTES_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  BLECharacteristic *distanceCharacteristic = pService->createCharacteristic(DISTANCE_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  BLECharacteristic *directionPreciseCharacteristic = pService->createCharacteristic(DIRECTION_PRECISE_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);

  MyCallback *myCallback = new MyCallback();
  destinationCharacteristic->setCallbacks(myCallback);
  etaCharacteristic->setCallbacks(myCallback);
  directionCharacteristic->setCallbacks(myCallback);
  directionDistanceCharacteristic->setCallbacks(myCallback);
  etaInMinutesCharacteristic->setCallbacks(myCallback);
  distanceCharacteristic->setCallbacks(myCallback);
  directionPreciseCharacteristic->setCallbacks(myCallback);

  pService->start();
  Serial.println("Characteristic defined!");
}

void loop() {
  if ((millis() - lastDebounceTime) > 100) debounce = true;

  if ((pServer->getConnectedCount() == 0) && (ifConnected)) {
    ifConnected = false;
    ifConnectionStateChange = true;
    gfx->fillScreen(bgColor);
  } else if ((!pServer->getConnectedCount() == 0) && (!ifConnected)) {
    ifConnected = true;
    ifConnectionStateChange = true;
    gfx->fillScreen(bgColor);
  }

  if (!ifConnected) {
    if (ifConnectionStateChange) {
      BLEAdvertising *adv = BLEDevice::getAdvertising();
      adv->addServiceUUID(SERVICE_UUID);
      adv->setScanResponse(true);
      adv->setMinPreferred(0x06);
      adv->setMinPreferred(0x12);
      BLEDevice::startAdvertising();

  // Icon and label (centered, scaled)
  gfx->fillRect(0, 0, SCREEN_W, ICON_Y + ICON_H + 2, bgColor);
  drawRGB565BitmapScaled(ICON_X, ICON_Y, (uint16_t*)NO_CONNECTION, 85, 85, ICON_W, ICON_H);
  drawTextCentered(ICON_Y + ICON_H + 6, "No Connection", 2, 0xFFFF, bgColor);

      ifConnectionStateChange = false;
    }
  } else {
    if (ifConnectionStateChange) {
      // Clear dynamic area under icon when first connected
      gfx->fillRect(0, ICON_Y + ICON_H + 2, SCREEN_W, SCREEN_H - (ICON_Y + ICON_H + 2), bgColor);
      ifConnectionStateChange = false;
    }

    if (destination.getBoolean() && millis() - previousMillis >= interval) {
      previousMillis = millis();
      String displayString = destination.getString();
      int len = (int)displayString.length();
      if (len <= charsToDisplay) {
        // Static title (no scroll), draw once and clear dirty flag
        drawTextCentered(Y_TITLE, displayString, SIZE_DEST, 0xFFFF, bgColor);
        destination.setBoolean(false);
        scroll_position = 0;
        scroll_right = true;
      } else {
        // Scroll with padding at start and end
        String full = " " + displayString + " ";
        int flen = (int)full.length();
        int max_scroll = flen - charsToDisplay;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_right && scroll_position >= max_scroll) scroll_right = false;
        else if (!scroll_right && scroll_position <= 0) scroll_right = true;
        int end = scroll_position + charsToDisplay;
        if (end > flen) end = flen;
        String toDraw = full.substring(scroll_position, end);
        while ((int)toDraw.length() < charsToDisplay) toDraw += " ";
        gfx->setTextColor(0xFFFF, bgColor);
        gfx->setTextSize(SIZE_DEST);
        gfx->setCursor(X_DEST, Y_TITLE);
        gfx->print(toDraw);
        if (scroll_right) scroll_position += 1; else scroll_position -= 1;
      }
    }
  // Icon and stacked texts
  if (directionPrecise.getBoolean()) { directionPrecise.setBoolean(false); drawDirectionImage(directionPrecise.getString()); }
  if (directionDistance.getBoolean()) { directionDistance.setBoolean(false); drawTextCentered(Y_DIRDIST, directionDistance.getString(), SIZE_DIRDIST, 0xFFFF, bgColor); }
  // Direction text: single-line scroller with padding, similar to destination
  if (direction.getBoolean() && millis() - dir_previousMillis >= interval) {
    dir_previousMillis = millis();
    String displayString = direction.getString();
    int len = (int)displayString.length();
    if (len <= dirCharsToDisplay) {
      // Static (no scroll): draw once and clear dirty flag
      drawTextCentered(Y_DIRTXT, displayString, SIZE_DIRTXT, 0xFFFF, bgColor);
      direction.setBoolean(false);
      dir_scroll_position = 0;
      dir_scroll_right = true;
    } else {
      // Scroll with padding
      String full = " " + displayString + " ";
      int flen = (int)full.length();
      int max_scroll = flen - dirCharsToDisplay;
      if (max_scroll < 0) max_scroll = 0;
      if (dir_scroll_right && dir_scroll_position >= max_scroll) dir_scroll_right = false;
      else if (!dir_scroll_right && dir_scroll_position <= 0) dir_scroll_right = true;
      int end = dir_scroll_position + dirCharsToDisplay;
      if (end > flen) end = flen;
      String toDraw = full.substring(dir_scroll_position, end);
      while ((int)toDraw.length() < dirCharsToDisplay) toDraw += " ";
      gfx->setTextColor(0xFFFF, bgColor);
      gfx->setTextSize(SIZE_DIRTXT);
      gfx->setCursor(X_DIR, Y_DIRTXT);
      gfx->print(toDraw);
      if (dir_scroll_right) dir_scroll_position += 1; else dir_scroll_position -= 1;
    }
  }
  // Bottom row side-by-side (now three columns): time remaining, ETA, distance
  if (ETA_Minute.getBoolean()) { ETA_Minute.setBoolean(false); drawTextCenteredInBox(X_LEFT_BOX, BOX_W, Y_BOTTOM, ETA_Minute.getString(), SIZE_TIMELEFT, 0xFFFF, bgColor); }
  if (ETA.getBoolean()) { ETA.setBoolean(false); drawTextCenteredInBox(X_MID_BOX, BOX_W, Y_BOTTOM, ETA.getString(), SIZE_ETA, 0xFFFF, bgColor); }
  if (distance.getBoolean()) { distance.setBoolean(false); drawTextCenteredInBox(X_RIGHT_BOX, BOX_W, Y_BOTTOM, distance.getString(), SIZE_DISTLEFT, 0xFFFF, bgColor); }
  }
}

void drawDirectionImage(const char *direction) {
  String s = direction;
  const uint16_t *bmp = UNKNOWN; // default
  if (s == "0") bmp = ARRIVE;
  else if (s == "1") bmp = ARRIVE_LEFT;
  else if (s == "2") bmp = ARRIVE_RIGHT;
  else if (s == "3") bmp = CONTINUE_LEFT;
  else if (s == "4") bmp = CONTINUE_RETURN;
  else if (s == "5") bmp = CONTINUE_RIGHT;
  else if (s == "6") bmp = CONTINUE_SLIGHT_LEFT;
  else if (s == "7") bmp = CONTINUE_SLIGHT_RIGHT;
  else if (s == "8") bmp = CONTINUE_STRAIGHT;
  else if (s == "9") bmp = DEPART;
  else if (s == "10") bmp = FORK;
  else if (s == "11") bmp = POINTER;
  else if (s == "12") bmp = ROTATORY_EXIT;
  else if (s == "13") bmp = ROTATORY_EXIT_INVERTED;
  else if (s == "14") bmp = ROTATORY_LEFT;
  else if (s == "15") bmp = ROTATORY_LEFT_INVERTED;
  else if (s == "16") bmp = ROTATORY_RIGHT;
  else if (s == "17") bmp = ROTATORY_RIGHT_INVERTED;
  else if (s == "18") bmp = ROTATORY_SHARP_LEFT;
  else if (s == "19") bmp = ROTATORY_SHARP_LEFT_INVERTED;
  else if (s == "20") bmp = ROTATORY_SHARP_RIGHT;
  else if (s == "21") bmp = ROTATORY_SHARP_RIGHT_INVERTED;
  else if (s == "22") bmp = ROTATORY_SLIGHT_LEFT;
  else if (s == "23") bmp = ROTATORY_SLIGHT_LEFT_INVERTED;
  else if (s == "24") bmp = ROTATORY_SLIGHT_RIGHT;
  else if (s == "25") bmp = ROTATORY_SLIGHT_RIGHT_INVERTED;
  else if (s == "26") bmp = ROTATORY_STRAIGHT;
  else if (s == "27") bmp = ROTATORY_STRAIGHT_INVERTED;
  else if (s == "28") bmp = ROTATORY_TOTAL;
  else if (s == "29") bmp = ROTATORY_TOTAL_INVERTED;
  else if (s == "30") bmp = SHARP_LEFT;
  else if (s == "31") bmp = SHARP_RIGHT;
  else if (s == "32") bmp = SLIGHT_LEFT;
  else if (s == "33") bmp = SLIGHT_RIGHT;

  // Clear icon area only and draw scaled, centered icon (avoid clearing title area)
  gfx->fillRect(0, ICON_Y, SCREEN_W, ICON_H, bgColor);
  drawRGB565BitmapScaled(ICON_X, ICON_Y, (uint16_t*)bmp, 85, 85, ICON_W, ICON_H);
}

void IRAM_ATTR buttonPressed() {
  if (debounce) {
    lastDebounceTime = millis();
    orientation = !orientation;
    gfx->setRotation(orientation ? 3 : 1);
  computeLayout();
  }
}
