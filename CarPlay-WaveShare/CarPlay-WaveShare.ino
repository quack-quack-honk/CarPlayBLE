#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>

// Use the same display libraries as the Waveshare example
#include <Arduino_GFX_Library.h>
#include <Arduino_DriveBus_Library.h>
#include "pin_config.h"  // provided by your Waveshare setup

// Reuse data model and icons from TTGO project
#include "../CarPlay-TTGO/lib/Information/Information.h"
#include "../CarPlay-TTGO/lib/Icons/Icons.h"

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

// Dynamic layout metrics
int SCREEN_W = 0, SCREEN_H = 0;
// Icon placement (computed in layout)
int ICON_X = 0, ICON_Y = 0, ICON_W = 0, ICON_H = 0;
// Text baselines and sizes
int SIZE_DEST = 2, SIZE_DIRTXT = 2, SIZE_DIRDIST = 2, SIZE_ETA = 3, SIZE_ETAMIN = 2, SIZE_DISTANCE = 2;
int Y_DEST = 0, Y_DIRTXT = 0, Y_DIRDIST = 0, Y_ETA = 0, Y_ETAMIN = 0, Y_DISTANCE = 0;

static inline int lineHeight(int size) { return 8 * size + 4; } // default font 6x8 scaled, with padding

void computeLayout() {
  SCREEN_W = gfx->width();
  SCREEN_H = gfx->height();

  // Allocate ~50% of height for icon, capped to 90% of width to keep square
  int maxIconH = (int)(SCREEN_H * 0.5f);
  int maxIconW = (int)(SCREEN_W * 0.9f);
  int target = maxIconH;
  if (target > maxIconW) target = maxIconW;
  if (target < 85) target = 85; // never smaller than source
  ICON_W = target;
  ICON_H = target;
  ICON_X = (SCREEN_W - ICON_W) / 2;
  ICON_Y = 4;

  int y = ICON_Y + ICON_H + 6;
  Y_DEST = y; y += lineHeight(SIZE_DEST);
  Y_DIRTXT = y; y += lineHeight(SIZE_DIRTXT);
  Y_DIRDIST = y; y += lineHeight(SIZE_DIRDIST);
  Y_ETA = y; y += lineHeight(SIZE_ETA);
  Y_ETAMIN = y; y += lineHeight(SIZE_ETAMIN);
  Y_DISTANCE = y; y += lineHeight(SIZE_DISTANCE);
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
    if (uuid == DESTINATION_UUID) destination.setString(c->getValue().c_str());
    else if (uuid == ETA_UUID) ETA.setString(c->getValue().c_str());
    else if (uuid == DIRECTION_UUID) direction.setString(c->getValue().c_str());
    else if (uuid == DIRECTION_DISTANCE_UUID) directionDistance.setString(c->getValue().c_str());
    else if (uuid == ETA_MINUTES_UUID) ETA_Minute.setString(c->getValue().c_str());
    else if (uuid == DISTANCE_UUID) distance.setString(c->getValue().c_str());
    else if (uuid == DIRECTION_PRECISE_UUID) directionPrecise.setString(c->getValue().c_str());
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
  bgColor = color565(56, 178, 92);
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

    if (directionPrecise.getBoolean()) { directionPrecise.setBoolean(false); drawDirectionImage(directionPrecise.getString()); }
    if (destination.getBoolean() && millis() - previousMillis >= interval) {
      previousMillis = millis();
      String displayString = destination.getString();
      int len = (int)displayString.length();
      int max_scroll = len - charsToDisplay;
      if (max_scroll < 0) max_scroll = 0;
      if (scroll_right && scroll_position >= max_scroll) scroll_right = false; else if (!scroll_right && scroll_position <= 0) scroll_right = true;
      int end = scroll_position + charsToDisplay;
      if (end > len) end = len;
      String toDraw = displayString.substring(scroll_position, end);
      // Destination (centered)
      drawTextCentered(Y_DEST, toDraw, SIZE_DEST, 0xFFFF, bgColor);
      if (scroll_right) scroll_position += 1; else scroll_position -= 1;
    }
    if (direction.getBoolean()) { direction.setBoolean(false); drawTextCentered(Y_DIRTXT, direction.getString(), SIZE_DIRTXT, 0xFFFF, bgColor); }
    if (directionDistance.getBoolean()) { directionDistance.setBoolean(false); drawTextCentered(Y_DIRDIST, directionDistance.getString(), SIZE_DIRDIST, 0xFFFF, bgColor); }
    if (ETA.getBoolean()) { ETA.setBoolean(false); drawTextCentered(Y_ETA, ETA.getString(), SIZE_ETA, 0xFFFF, bgColor); }
    if (ETA_Minute.getBoolean()) { ETA_Minute.setBoolean(false); drawTextCentered(Y_ETAMIN, ETA_Minute.getString(), SIZE_ETAMIN, 0xFFFF, bgColor); }
    if (distance.getBoolean()) { distance.setBoolean(false); drawTextCentered(Y_DISTANCE, distance.getString(), SIZE_DISTANCE, 0xFFFF, bgColor); }
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

  // Clear icon area and draw scaled, centered icon
  gfx->fillRect(0, 0, SCREEN_W, ICON_Y + ICON_H, bgColor);
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
