#pragma once
#include <Arduino.h>
#include <lvgl.h>

namespace Board {
    bool begin();
    void serviceLVGL();
    bool sdReady();
    bool retrySD(uint32_t speedHz);
    bool touchPressed();
    bool touchPosition(int16_t &x,int16_t &y); // most recent GT911 physical point
    uint32_t touchLastChangeMs();
    uint32_t guiFrameCount();
    uint32_t guiFlushCount();
    uint64_t guiPixelCount();
}

// FIX7U diagnostic modes: 0 NORMAL, 1 OFF (no GT911 reads), 2 READ_ONLY.
namespace Board {
    void setTouchDiagnosticMode(uint8_t mode);
    uint8_t touchDiagnosticMode();
    void printTouchDiagnostics();
}

// FIX7Y: 0 normal, 1 LVGL render without QSPI, 2 GUI service suspended.
namespace Board { void setDisplayTestMode(uint8_t mode); uint8_t displayTestMode(); void printDisplayTestStats(); }

namespace Board { void setLvglInterval7Z(uint32_t ms); void printLvglIntervalStats7Z(); }

namespace Board {
    void setLvglHandlerPaused8A(bool paused);
    bool lvglHandlerPaused8A();
    void printMemoryStats8A();
}
