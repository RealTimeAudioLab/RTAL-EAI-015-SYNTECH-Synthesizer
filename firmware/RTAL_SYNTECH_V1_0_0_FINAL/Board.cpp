#include "Board.h"
#include "PINS_JC4827W543.h"

#include <Arduino_GFX_Library.h>
#include <SPI.h>
#include <SD.h>
#include <TAMC_GT911.h>
#include "esp_heap_caps.h"

static SPIClass sSdSPI(HSPI);

static Arduino_DataBus *sBus = nullptr;
static Arduino_GFX *sGfx = nullptr;
static Arduino_NV3041A *sPanel = nullptr;

static TAMC_GT911 sTouch(
    TOUCH_SDA, TOUCH_SCL,
    TOUCH_INT, TOUCH_RES,
    LCD_WIDTH, LCD_HEIGHT
);

// LVGL 8.4 display/input objects.  These driver structs must remain alive
// for the complete lifetime of LVGL, therefore they are static.
static lv_disp_draw_buf_t sDrawBufDesc;
static lv_disp_drv_t sDispDrv;
static lv_disp_t *sDisplay = nullptr;
static lv_indev_drv_t sIndevDrv;
static lv_indev_t *sIndev = nullptr;
static lv_color_t *sDrawBuf = nullptr;

// FIX7U: Core0-only touch diagnostic instrumentation. No change to audio task.
static volatile uint8_t sTouchDiagMode = 0;
static uint32_t sTouchReadCount = 0;
static uint32_t sTouchReadTotalUs = 0;
static uint32_t sTouchReadMaxUs = 0;
static uint32_t sTouchPressedReads = 0;
static uint32_t sTouchCBCount = 0;
static uint32_t sTouchDiagLastPrintMs = 0;
static bool sSDReady = false;
static volatile bool sTouchPressed = false;
static volatile int16_t sTouchX = -1, sTouchY = -1;
static volatile uint32_t sTouchLastChangeMs = 0;
static volatile uint8_t sDisplayTestMode = 0;
// FIX7Z: test LVGL handler cadence independently of the audio engine.
static volatile uint32_t sLvglIntervalMs7Z = 5;
static volatile bool sLvglHandlerPaused8A = false;
static volatile uint32_t sLvglHandlerCalls7Z = 0;
static volatile uint32_t sLvglHandlerMaxUs7Z = 0;
static volatile uint32_t sLvglHandlerOver3ms7Z = 0;
static volatile uint32_t sSkippedFlushes7Y = 0;
static volatile uint64_t sSkippedPixels7Y = 0;
static volatile uint32_t sGuiFrames = 0;
static volatile uint32_t sGuiFlushes = 0;
static volatile uint64_t sGuiPixels = 0;

// Release display path: QSPI direct RGB565 byte stream retained from tested FIX1.
static constexpr uint16_t LVGL_BUF_LINES = 60;
static constexpr size_t LVGL_BUF_PIXELS = LCD_WIDTH * LVGL_BUF_LINES;
static constexpr size_t LVGL_BUF_BYTES = LVGL_BUF_PIXELS * sizeof(lv_color_t);

static void flushCB(
    lv_disp_drv_t *disp,
    const lv_area_t *area,
    lv_color_t *color_p)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    const uint32_t pixels = (uint32_t)w * (uint32_t)h;
    sGuiFlushes++;
    sGuiPixels += pixels;
    if(sDisplayTestMode != 0) {
        ++sSkippedFlushes7Y;
        sSkippedPixels7Y += pixels;
        if(lv_disp_flush_is_last(disp)) sGuiFrames++;
        lv_disp_flush_ready(disp);
        return;
    }

    sPanel->startWrite();
    sPanel->setAddrWindow(area->x1, area->y1, (uint16_t)w, (uint16_t)h);

    // Proven FIX1 path: byte-swap LVGL RGB565 in-place, transmit directly,
    // then restore the LVGL buffer. This avoids Arduino_GFX writePixels()'
    // per-pixel staging copy while preserving the tested colour order.
    uint16_t *px = reinterpret_cast<uint16_t *>(color_p);
    for(uint32_t i = 0; i < pixels; ++i) {
        const uint16_t v = px[i];
        px[i] = (uint16_t)((v << 8) | (v >> 8));
    }
    sBus->writeBytes(reinterpret_cast<uint8_t *>(px), pixels * 2U);
    for(uint32_t i = 0; i < pixels; ++i) {
        const uint16_t v = px[i];
        px[i] = (uint16_t)((v << 8) | (v >> 8));
    }

    sPanel->endWrite();
    if(lv_disp_flush_is_last(disp)) sGuiFrames++;
    lv_disp_flush_ready(disp);
}

static void touchCB(
    lv_indev_drv_t *indev_drv,
    lv_indev_data_t *data)
{
    (void)indev_drv;

    ++sTouchCBCount;
    if (sTouchDiagMode == 1) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    const uint32_t touchReadStart = micros();
    sTouch.read();
    const uint32_t touchReadElapsed = (uint32_t)(micros() - touchReadStart);
    ++sTouchReadCount;
    sTouchReadTotalUs += touchReadElapsed;
    if (touchReadElapsed > sTouchReadMaxUs) sTouchReadMaxUs = touchReadElapsed;
    if (sTouch.isTouched && sTouch.touches > 0) ++sTouchPressedReads;
    if (sTouchDiagMode == 2) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    // FIX8B: Restore immediate GT911 press/release forwarding.
    // The 12/30 ms filter in FIX8A suppressed short touches because
    // LVGL polls the input device at discrete intervals.
    const bool pressed = (sTouch.isTouched && sTouch.touches > 0);
    if (pressed) {
        data->point.x = constrain(sTouch.points[0].x, 0, LCD_WIDTH - 1);
        data->point.y = constrain(sTouch.points[0].y, 0, LCD_HEIGHT - 1);
        sTouchX = data->point.x;
        sTouchY = data->point.y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    static uint32_t lastTouchPrint = 0;
    if (pressed && (uint32_t)(millis() - lastTouchPrint) > 300) {
        lastTouchPrint = millis();
        Serial.printf("[TOUCH] x=%d y=%d\n", (int)data->point.x, (int)data->point.y);
    }
    if (pressed != sTouchPressed) {
        sTouchPressed = pressed;
        sTouchLastChangeMs = millis();
    }
}

bool Board::begin()
{
    pinMode(GFX_BL, OUTPUT);
    digitalWrite(GFX_BL, LOW);

    // -------------------------------------------------------------------------
    // 1. SD first - exact proven MiniAmused pattern
    // -------------------------------------------------------------------------
    Serial.println("[B01] SD pins");
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    delay(10);

    Serial.println("[B02] HSPI.begin(..., SS=-1)");
    sSdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, -1);
    delay(20);

    Serial.println("[B03] SD.begin 8 MHz");
    sSDReady = SD.begin(SD_CS, sSdSPI, SD_SPEED);

    if (sSDReady) {
        const uint8_t cardType = SD.cardType();
        const uint64_t cardSizeMB = SD.cardSize() / (1024ULL * 1024ULL);
        Serial.printf("[B04] SD OK type=%u size=%llu MB\n",
                      (unsigned)cardType,
                      (unsigned long long)cardSizeMB);
    } else {
        Serial.println("[B04] SD mount FAILED - continuing");
    }

    // -------------------------------------------------------------------------
    // 2. QSPI / NV3041A after SD
    // -------------------------------------------------------------------------
    Serial.println("[B05] before QSPI constructor");
    sBus = new Arduino_ESP32QSPI(GFX_CS, GFX_SCK, GFX_D0, GFX_D1, GFX_D2, GFX_D3);
    if (!sBus) {
        Serial.println("[B06] FATAL: QSPI allocation failed");
        return false;
    }
    Serial.println("[B06] QSPI constructor returned");

    Serial.println("[B07] before NV3041A constructor");
    sPanel = new Arduino_NV3041A(sBus, GFX_NOT_DEFINED, 0, true);
    sGfx = sPanel;
    if (!sPanel) {
        Serial.println("[B08] FATAL: NV3041A allocation failed");
        return false;
    }
    Serial.println("[B08] NV3041A ready; FIX3 exact MiniAmused byte-swapped flush");

    Serial.println("[B09] gfx->begin");
    if (!sGfx->begin(GFX_SPEED)) {
        Serial.println("[B10] FATAL: gfx->begin failed");
        return false;
    }
    Serial.println("[B10] display OK");
    // V0.3A: no diagnostic RGB bars, no 1.5 second boot delay.
    sGfx->fillScreen(RGB565_BLACK);

    // Keep proven MiniAmused panel initialization, omit its branded splash.
    digitalWrite(GFX_BL, HIGH);

    // -------------------------------------------------------------------------
    // 3. LVGL 8.4.0
    // -------------------------------------------------------------------------
    Serial.println("[B11] lv_init - LVGL 8.4.0");
    lv_init();

    Serial.println("[B12] allocate 60-line RGB565 buffer");
    sDrawBuf = static_cast<lv_color_t *>(
        heap_caps_malloc(LVGL_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
    );
    if (!sDrawBuf) {
        sDrawBuf = static_cast<lv_color_t *>(malloc(LVGL_BUF_BYTES));
    }
    if (!sDrawBuf) {
        Serial.println("[B13] FATAL: LVGL buffer allocation failed");
        return false;
    }
    Serial.printf("[B13] LVGL buffer OK: %u bytes / %u pixels addr=%p internal=%s dma=%s psram=%s\n",
                  (unsigned)LVGL_BUF_BYTES, (unsigned)LVGL_BUF_PIXELS, sDrawBuf,
                  esp_ptr_internal(sDrawBuf) ? "YES" : "NO",
                  esp_ptr_dma_capable(sDrawBuf) ? "YES" : "NO",
                  esp_ptr_external_ram(sDrawBuf) ? "YES" : "NO");
    Serial.printf("[B13b] LCD=%ux%u RGB565 QSPI=%lu Hz bufferLines=%u\n",
                  (unsigned)LCD_WIDTH, (unsigned)LCD_HEIGHT,
                  (unsigned long)GFX_SPEED, (unsigned)LVGL_BUF_LINES);

    lv_disp_draw_buf_init(&sDrawBufDesc, sDrawBuf, nullptr, LVGL_BUF_PIXELS);

    Serial.println("[B14] lv_disp_drv_register");
    lv_disp_drv_init(&sDispDrv);
    sDispDrv.hor_res = LCD_WIDTH;
    sDispDrv.ver_res = LCD_HEIGHT;
    sDispDrv.flush_cb = flushCB;
    sDispDrv.draw_buf = &sDrawBufDesc;
    sDisplay = lv_disp_drv_register(&sDispDrv);
    if (!sDisplay) {
        Serial.println("[B15] FATAL: lv_disp_drv_register failed");
        return false;
    }
    // STAGE4: LVGL input remains responsive, but full display refresh is
    // limited to 25 Hz to reduce shared flash/cache/QSPI pressure on Core1.
    // LVGL 8.4 display owns its internal refresh timer.
    if (sDisplay->refr_timer) lv_timer_set_period(sDisplay->refr_timer, 40);
    Serial.println("[B15] LVGL 8.4 display configured; refresh period 40ms");

    // -------------------------------------------------------------------------
    // 4. GT911 + LVGL 8.4 input driver
    // -------------------------------------------------------------------------
    Serial.println("[B16] GT911 begin");
    sTouch.begin(GT911_ADDR1);
    sTouch.setRotation(ROTATION_INVERTED);

    Serial.println("[B17] LVGL 8.4 input register");
    lv_indev_drv_init(&sIndevDrv);
    sIndevDrv.type = LV_INDEV_TYPE_POINTER;
    sIndevDrv.read_cb = touchCB;
    sIndev = lv_indev_drv_register(&sIndevDrv);
    if (!sIndev) {
        Serial.println("[B18] FATAL: lv_indev_drv_register failed");
        return false;
    }
    Serial.println("[B18] GT911 ready");

    digitalWrite(GFX_BL, HIGH);
    Serial.println("[B19] backlight ON");
    return true;
}


void Board::serviceLVGL()
{
    // LVGL 8.x has no lv_tick_set_cb() used by our former LVGL 9 port.
    // Feed the LVGL tick from Arduino millis() on every service call, even if
    // the expensive timer handler itself is throttled to 2 ms.
    static uint32_t lastTickMs = 0;
    static uint32_t lastHandlerMs = 0;
    const uint32_t now = millis();

    if(lastTickMs == 0) lastTickMs = now;
    const uint32_t elapsed = (uint32_t)(now - lastTickMs);
    if(elapsed) {
        lv_tick_inc(elapsed);
        lastTickMs = now;
    }

    if((uint32_t)(now - lastHandlerMs) < sLvglIntervalMs7Z)
        return;
    lastHandlerMs = now;

    if(sDisplayTestMode != 2 && !sLvglHandlerPaused8A) {
        const uint32_t startUs = micros();
        lv_timer_handler();
        const uint32_t durationUs = (uint32_t)(micros() - startUs);
        ++sLvglHandlerCalls7Z;
        if(durationUs > sLvglHandlerMaxUs7Z) sLvglHandlerMaxUs7Z = durationUs;
        if(durationUs > 3000) ++sLvglHandlerOver3ms7Z;
    }
    if ((uint32_t)(now - sTouchDiagLastPrintMs) >= 2000) {
        sTouchDiagLastPrintMs = now;
        Board::printTouchDiagnostics();
    }
}

bool Board::retrySD(uint32_t speedHz) {
    // Only called on the Core0 GUI/setup thread while no SD File is open.
    // Do not touch QSPI, LVGL, GT911, I2S or MIDI.
    Serial.printf("[SD RETRY] release and reinitialize HSPI @%lu Hz\\n",(unsigned long)speedHz);
    SD.end();
    sSDReady=false;
    sSdSPI.end();
    pinMode(SD_CS,OUTPUT);
    digitalWrite(SD_CS,HIGH);
    delay(80);
    sSdSPI.begin(SD_SCK,SD_MISO,SD_MOSI,-1);
    delay(40);
    sSDReady=SD.begin(SD_CS,sSdSPI,speedHz);
    if(sSDReady && SD.cardType()!=CARD_NONE) {
        File dir=SD.open("/RTAL_SYNTECH/PRESETS",FILE_READ);
        const bool directoryOK=(bool)dir && dir.isDirectory();
        if(dir)dir.close();
        Serial.printf("[SD RETRY] mounted; preset directory=%s\\n",directoryOK?"OK":"MISSING");
        return true; // Mount and preset data are deliberately reported separately.
    }
    sSDReady=false;
    Serial.println("[SD RETRY] mount failed or no card");
    return false;
}
bool Board::sdReady() { return sSDReady; }
bool Board::touchPressed() { return sTouchPressed; }
bool Board::touchPosition(int16_t &x,int16_t &y) {
    if(!sTouchPressed)return false;
    x=sTouchX;y=sTouchY;
    return true;
}
uint32_t Board::touchLastChangeMs() { return sTouchLastChangeMs; }

uint32_t Board::guiFrameCount() { return sGuiFrames; }
uint32_t Board::guiFlushCount() { return sGuiFlushes; }
uint64_t Board::guiPixelCount() { return sGuiPixels; }

// FIX7U: Called only from the Core0 GUI/serial owner.
void Board::setTouchDiagnosticMode(uint8_t mode) {
    if (mode > 2) return;
    sTouchDiagMode = mode;
    sTouchReadCount = sTouchReadTotalUs = sTouchReadMaxUs = 0;
    sTouchPressedReads = sTouchCBCount = 0;
    sTouchPressed = false;
    Serial.printf("[TOUCH7U] mode=%u (%s) core=%d counters reset\n",
      (unsigned)mode, mode==0?"NORMAL":mode==1?"OFF":"READ_ONLY", xPortGetCoreID());
}
uint8_t Board::touchDiagnosticMode() { return sTouchDiagMode; }
void Board::printTouchDiagnostics() {
    const uint32_t n = sTouchReadCount;
    Serial.printf("[TOUCH7U] mode=%u core=%d cb=%lu reads=%lu pressedReads=%lu readAvgUs=%lu readMaxUs=%lu\n",
      (unsigned)sTouchDiagMode, xPortGetCoreID(),
      (unsigned long)sTouchCBCount, (unsigned long)n,
      (unsigned long)sTouchPressedReads,
      (unsigned long)(n ? sTouchReadTotalUs/n : 0),
      (unsigned long)sTouchReadMaxUs);
}

void Board::setDisplayTestMode(uint8_t mode) { if(mode <= 2) { sDisplayTestMode=mode; if(mode==0 && sDisplay) lv_obj_invalidate(lv_scr_act()); } }
uint8_t Board::displayTestMode() { return sDisplayTestMode; }
void Board::printDisplayTestStats() { Serial.printf("[DISPLAY7Y] mode=%u skippedFlush=%lu skippedPixels=%llu totalFlush=%lu totalPixels=%llu\n",(unsigned)sDisplayTestMode,(unsigned long)sSkippedFlushes7Y,(unsigned long long)sSkippedPixels7Y,(unsigned long)sGuiFlushes,(unsigned long long)sGuiPixels); }

// FIX7Z runtime diagnostics; invoked only from the GUI owner task.
void Board::setLvglInterval7Z(uint32_t ms) { if(ms>=5 && ms<=200) sLvglIntervalMs7Z=ms; }
void Board::printLvglIntervalStats7Z() {
    Serial.printf("[LVGL7Z] intervalMs=%lu calls=%lu maxUs=%lu over3ms=%lu displayMode=%u\n",
      (unsigned long)sLvglIntervalMs7Z,(unsigned long)sLvglHandlerCalls7Z,
      (unsigned long)sLvglHandlerMaxUs7Z,(unsigned long)sLvglHandlerOver3ms7Z,(unsigned)sDisplayTestMode);
}

// FIX8A: Diagnostic-only controls, owned by the Core0 GUI task.
void Board::setLvglHandlerPaused8A(bool paused) {
    if(sLvglHandlerPaused8A && !paused && sDisplay) lv_obj_invalidate(lv_scr_act());
    sLvglHandlerPaused8A = paused;
}
bool Board::lvglHandlerPaused8A() { return sLvglHandlerPaused8A; }
void Board::printMemoryStats8A() {
    const uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA);
    const uint32_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    const uint32_t psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    Serial.printf("[MEM8A] draw=%p bytes=%u lines=%u buffers=1 internal=%u dma=%u psram=%u intFree=%lu intLargest=%lu dmaFree=%lu dmaLargest=%lu psramFree=%lu\n",
       sDrawBuf, (unsigned)LVGL_BUF_BYTES, (unsigned)LVGL_BUF_LINES,
       sDrawBuf && esp_ptr_internal(sDrawBuf), sDrawBuf && esp_ptr_dma_capable(sDrawBuf),
       sDrawBuf && esp_ptr_external_ram(sDrawBuf),
       (unsigned long)internalFree,(unsigned long)internalLargest,
       (unsigned long)dmaFree,(unsigned long)dmaLargest,(unsigned long)psramFree);
}
