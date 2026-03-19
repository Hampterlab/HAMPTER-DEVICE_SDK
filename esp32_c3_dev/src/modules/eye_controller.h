#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "dynamic_pattern.h"

#if defined(ESP32)
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
#endif

#ifndef LED_PIN
#define LED_PIN     6
#endif
#ifndef NUM_LEDS
#define NUM_LEDS    12
#endif
#ifndef LED_TYPE
#define LED_TYPE    WS2812B
#endif
#ifndef COLOR_ORDER
#define COLOR_ORDER GRB
#endif

#ifndef BUTTON_PIN
#define BUTTON_PIN  9
#endif
#define POWER_LED_PIN 10
#define MCP_LED_PIN 4
#define POWER_LED_BRIGHTNESS 20 // 파워 LED 밝기 조절 (0~255)

// 전역 LED 버퍼
static CRGB leds[NUM_LEDS];

class EyeController {
public:
  enum class Mood : uint8_t { Neutral, Annoyed, Angry };
  enum class BlinkPhase : uint8_t { Idle, Closing, Hold, Opening };

  struct Config {
    // 타이밍
    uint16_t baseBlinkMs   = 10000; // 기본 10초
    uint16_t jitterMs      = 2000;  // ±1초
    uint16_t closeMs       = 140;   // 감기
    uint16_t holdMs        =  80;   // 유지
    uint16_t openMs        = 160;   // 뜨기
    uint8_t  baseBrightness= 100;   // 기본 밝기
    uint16_t tickMs        = 16;    // 태스크 주기(~60fps)

    // 연출 옵션
    bool     eyelidSweep   = true;  // true면 눈꺼풀 스윕 사용
    uint8_t  featherLEDs   = 2;     // 경계 부드러움(LED 개수 기준)
    uint8_t  doubleBlinkPct= 20;    // 더블 블링크 확률(%)
    uint16_t doubleBlinkGapMin = 200; // 2회차 시작 지연 최소(ms)
    uint16_t doubleBlinkGapMax = 300; // 최대(ms)

    // 기하 정보
    uint8_t  topIndex      = 3;     // ★ 맨 위 LED 인덱스
  } cfg;

  DynamicPattern dynamicPattern;

  static EyeController& instance() {
    static EyeController inst;
    return inst;
  }

  // MCP 연결 상태 LED 제어
  void setMCPStatus(bool connected) {
    digitalWrite(MCP_LED_PIN, connected ? HIGH : LOW);
  }

  void begin() {
    if (_inited) return;
    
    pinMode(BUTTON_PIN, INPUT_PULLUP); // 버튼 핀 초기화

    // 상태 LED 초기화
    pinMode(POWER_LED_PIN, OUTPUT);
    pinMode(MCP_LED_PIN, OUTPUT);
    analogWrite(POWER_LED_PIN, POWER_LED_BRIGHTNESS); // 전원 켜짐 표시 (밝기 조절)
    digitalWrite(MCP_LED_PIN, LOW);    // MCP 연결 대기

    // NVS 로드 및 패턴 시스템 초기화
    dynamicPattern.begin();

    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(cfg.baseBrightness);
    FastLED.clear(true);
    setMood(Mood::Neutral, /*immediateShow=*/true);

    randomSeed((uint32_t)micros());
    _scheduleNextBlink(millis(), /*immediate=*/false);
    _inited = true;
    _startBackgroundTask();
  }

  void update() {
    if (!_inited) return;
    const uint32_t now = millis();

    // --- 버튼 폴링 로직 (Short/Long Press) ---
    bool btnState = digitalRead(BUTTON_PIN);
    
    // 버튼 눌림 (Falling Edge)
    if (btnState == LOW && _lastBtnState == HIGH) {
      _btnPressTime = now;
      _longPressTriggered = false;
    }
    
    // 버튼 누르고 있는 중
    if (btnState == LOW) {
      // 1초 이상 누르면 롱프레스 처리 (한 번만) - 전원 토글
      if (!_longPressTriggered && (now - _btnPressTime > 1000)) {
        _longPressTriggered = true;
        
        _powerOn = !_powerOn; // 전원 상태 토글

        if (_powerOn) {
          // 전원 켜짐
          // analogWrite(POWER_LED_PIN, POWER_LED_BRIGHTNESS); // 이미 켜져있으므로 생략 가능하나 명시적으로 유지
        } else {
          // 전원 꺼짐 (Sleep Mode) - 눈만 끔, 파워 LED는 유지
          // analogWrite(POWER_LED_PIN, 0); // <--- 제거됨: 파워 LED는 켜둔 상태 유지
          dynamicPattern.stop(); 
          FastLED.clear(true);
        }
      }
    }

    // 버튼 뗌 (Rising Edge)
    if (btnState == HIGH && _lastBtnState == LOW) {
      // 롱프레스가 아니었다면 숏프레스 처리 (디바운스 50ms 고려)
      if (!_longPressTriggered && (now - _btnPressTime > 50)) {
        if (_powerOn) {
           dynamicPattern.cycleNextSlot(); // 전원 켜져있을 때만 패턴 변경
        }
      }
    }
    
    _lastBtnState = btnState;
    // --- 버튼 폴링 로직 끝 ---

    if (!_powerOn) return; // 전원 꺼져있으면 여기서 리턴 (LED 렌더링 중단)

    // 동적 패턴 우선 처리
    if (dynamicPattern.isActive()) {
      dynamicPattern.update(leds, now);
      FastLED.show();
      return;
    }

    // 기존 깜박임 로직
    switch (_phase) {
      case BlinkPhase::Idle:
        if ((int32_t)(now - _nextDue) >= 0) {
          _startPhase(BlinkPhase::Closing, now);
        } else {
          _renderOpen();
        }
        break;

      case BlinkPhase::Closing: {
        uint8_t scale = _progressScale(now, _phaseStart, cfg.closeMs, true);
        _renderByPhase(scale);
        if (_phaseDone(now, _phaseStart, cfg.closeMs)) {
          _startPhase(BlinkPhase::Hold, now);
        }
      } break;

      case BlinkPhase::Hold:
        _renderByPhase(0);
        if (_phaseDone(now, _phaseStart, cfg.holdMs)) {
          _startPhase(BlinkPhase::Opening, now);
        }
        break;

      case BlinkPhase::Opening: {
        uint8_t scale = _progressScale(now, _phaseStart, cfg.openMs, false);
        _renderByPhase(scale);
        if (_phaseDone(now, _phaseStart, cfg.openMs)) {
          _phase = BlinkPhase::Idle;
          if (!_pendingDouble && cfg.doubleBlinkPct > 0 &&
              (uint8_t)random(0, 100) < cfg.doubleBlinkPct) {
            _pendingDouble = true;
            uint16_t gap = cfg.doubleBlinkGapMin +
                           (uint16_t)random(0, (int)max<int>(0, cfg.doubleBlinkGapMax - cfg.doubleBlinkGapMin + 1));
            _nextDue = now + gap;
          } else {
            _pendingDouble = false;
            _scheduleNextBlink(now, /*immediate=*/false);
          }
        }
      } break;
    }
  }

  void setMood(Mood m, bool immediateShow = false) {
    _mood = m;
    switch (m) {
      case Mood::Neutral:  _color = CRGB(0, 255, 0);   break;
      case Mood::Annoyed:  _color = CRGB(255, 255, 0); break;
      case Mood::Angry:    _color = CRGB(255, 0, 0);   break;
    }
    if (immediateShow && _phase == BlinkPhase::Idle) _renderOpen();
  }

  Mood currentMood() const { return _mood; }

private:
  EyeController() {}
  bool _inited = false;
  bool _powerOn = true; // 전원 상태 추적

  Mood _mood = Mood::Neutral;
  CRGB _color = CRGB(0, 255, 0);

  BlinkPhase _phase = BlinkPhase::Idle;
  uint32_t _phaseStart = 0;
  uint32_t _nextDue    = 0;
  bool     _pendingDouble = false;
  
  // 버튼 관련 멤버
  bool     _lastBtnState = HIGH;
  uint32_t _btnPressTime = 0;
  bool     _longPressTriggered = false;

  void _cycleMood() {
    switch (_mood) {
      case Mood::Neutral: setMood(Mood::Annoyed, true); break;
      case Mood::Annoyed: setMood(Mood::Angry, true);   break;
      case Mood::Angry:   setMood(Mood::Neutral, true); break;
    }
  }

#if defined(ESP32)
  TaskHandle_t _taskHandle = nullptr;
#endif

  void _startPhase(BlinkPhase p, uint32_t now) { _phase = p; _phaseStart = now; }

  void _scheduleNextBlink(uint32_t now, bool immediate) {
    uint32_t base = cfg.baseBlinkMs;
    if (!immediate) base = _withJitter(cfg.baseBlinkMs, cfg.jitterMs);
    _nextDue = now + base;
  }

  static bool _phaseDone(uint32_t now, uint32_t start, uint16_t dur) {
    return (int32_t)(now - (start + dur)) >= 0;
  }

  static uint8_t _progressScale(uint32_t now, uint32_t start, uint16_t dur, bool closing) {
    if (dur == 0) return closing ? 0 : 255;
    uint32_t elapsed = now - start;
    if (elapsed >= dur) return closing ? 0 : 255;
    uint32_t t = (elapsed * 255UL) / dur;
    return closing ? (255 - t) : t;
  }

  static uint32_t _withJitter(uint32_t base, uint16_t jitter) {
    if (jitter == 0) return base;
    int16_t half = jitter / 2;
    int16_t r = random(-half, half + 1);
    int32_t v = (int32_t)base + r;
    if (v < 50) v = 50;
    return (uint32_t)v;
  }

  void _renderOpen() { _renderBothLids(/*openRatio=*/1.0f); }

  void _renderByPhase(uint8_t scale) {
    if (!cfg.eyelidSweep) {
      CRGB c = _color; c.nscale8_video(scale);
      fill_solid(leds, NUM_LEDS, c);
      FastLED.show();
      return;
    }
    float openRatio = scale / 255.0f;
    _renderBothLids(openRatio);
  }

  void _renderBothLids(float openRatio) {
    const float low  = (1.0f - openRatio) * 0.5f;
    const float high = 1.0f - low;

    const float feather = (cfg.featherLEDs > 0) ? (float)cfg.featherLEDs / (float)NUM_LEDS : 0.0f;

    for (uint8_t i = 0; i < NUM_LEDS; ++i) {
      int16_t di = (int16_t)i - (int16_t)cfg.topIndex;
      di %= (int16_t)NUM_LEDS; if (di < 0) di += NUM_LEDS;
      float theta = (2.0f * PI) * ((float)di / (float)NUM_LEDS);

      float h = (cosf(theta) + 1.0f) * 0.5f;

      float lit = 0.0f;
      if (h >= (low + feather) && h <= (high - feather)) {
        lit = 1.0f;
      } else if (feather > 0.0f) {
        if (h > low && h < (low + feather)) {
          float t = (h - low) / feather;
          if (t < 0) t = 0; if (t > 1) t = 1;
          lit = t;
        }
        if (h < high && h > (high - feather)) {
          float t = (high - h) / feather;
          if (t < 0) t = 0; if (t > 1) t = 1;
          lit = max(lit, t);
        }
      }

      CRGB c = _color;
      c.nscale8_video((uint8_t)(lit * 255.0f));
      leds[i] = c;
    }
    FastLED.show();
  }

  static void _taskLoop(void* pv) {
    EyeController* self = static_cast<EyeController*>(pv);
    for (;;) {
      self->update();
      vTaskDelay(pdMS_TO_TICKS(self->cfg.tickMs));
    }
  }

  void _startBackgroundTask() {
#if defined(ESP32)
    if (_taskHandle) return;
    xTaskCreate(&_taskLoop, "EyeBlinkTask", 4096, this, 1, &_taskHandle);
#endif
  }
};