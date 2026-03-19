#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include <ctype.h>
#include <math.h>

#ifndef NUM_LEDS
#define NUM_LEDS 12
#endif

// ★ 전방 선언 (순환 include 방지)
extern float port_get_inport_value(const char* name);

// 경량 수식 파서 (비교 및 논리 연산자 + InPort 변수 지원)
class ExpressionEvaluator {
public:
  float eval(const char* expr, float theta, float t, int i) {
    _expr = expr;
    _pos = 0;
    _theta = theta;
    _t = t;
    _i = i;
    return _parseConditional();
  }

private:
  const char* _expr;
  size_t _pos;
  float _theta, _t;
  int _i;

  char _peek() const {
    return _expr[_pos];
  }

  char _consume() {
    return _expr[_pos++];
  }

  void _skipWhitespace() {
    while (isspace(_peek())) _pos++;
  }

  // 조건식: conditional → logicalOr ('?' conditional ':' conditional)?
  // right-associative to support nested ternary expressions.
  float _parseConditional() {
    _skipWhitespace();
    float cond = _parseLogicalOr();
    _skipWhitespace();

    if (_peek() == '?') {
      _consume(); // '?'
      float whenTrue = _parseConditional();
      _skipWhitespace();
      if (_peek() == ':') {
        _consume(); // ':'
      }
      float whenFalse = _parseConditional();
      return (cond != 0.0f) ? whenTrue : whenFalse;
    }

    return cond;
  }

  // 논리 OR: logicalOr → logicalAnd ('||' logicalAnd)*
  float _parseLogicalOr() {
    _skipWhitespace();
    float result = _parseLogicalAnd();
    
    while (true) {
      _skipWhitespace();
      if (_peek() == '|' && _expr[_pos + 1] == '|') {
        _consume(); _consume();
        float right = _parseLogicalAnd();
        result = (result != 0 || right != 0) ? 1.0f : 0.0f;
      } else {
        break;
      }
    }
    return result;
  }

  // 논리 AND: logicalAnd → comparison ('&&' comparison)*
  float _parseLogicalAnd() {
    _skipWhitespace();
    float result = _parseComparison();
    
    while (true) {
      _skipWhitespace();
      if (_peek() == '&' && _expr[_pos + 1] == '&') {
        _consume(); _consume();
        float right = _parseComparison();
        result = (result != 0 && right != 0) ? 1.0f : 0.0f;
      } else {
        break;
      }
    }
    return result;
  }

  // 비교: comparison → expression (('<' | '>' | '<=' | '>=' | '==' | '!=') expression)?
  float _parseComparison() {
    _skipWhitespace();
    float result = _parseExpression();
    
    _skipWhitespace();
    char op1 = _peek();
    
    if (op1 == '<' || op1 == '>' || op1 == '=' || op1 == '!') {
      _consume();
      char op2 = _peek();
      
      // <=, >=, ==, !=
      if ((op1 == '<' && op2 == '=') || 
          (op1 == '>' && op2 == '=') || 
          (op1 == '=' && op2 == '=') || 
          (op1 == '!' && op2 == '=')) {
        _consume();
        float right = _parseExpression();
        
        if (op1 == '<') return (result <= right) ? 1.0f : 0.0f;
        if (op1 == '>') return (result >= right) ? 1.0f : 0.0f;
        if (op1 == '=') return (fabs(result - right) < 0.0001f) ? 1.0f : 0.0f;
        if (op1 == '!') return (fabs(result - right) >= 0.0001f) ? 1.0f : 0.0f;
      }
      // <, >
      else if (op1 == '<' || op1 == '>') {
        float right = _parseExpression();
        if (op1 == '<') return (result < right) ? 1.0f : 0.0f;
        if (op1 == '>') return (result > right) ? 1.0f : 0.0f;
      }
    }
    
    return result;
  }

  // 파싱: expression → term (('+' | '-') term)*
  float _parseExpression() {
    _skipWhitespace();
    float result = _parseTerm();
    
    while (true) {
      _skipWhitespace();
      char op = _peek();
      if (op == '+' || op == '-') {
        _consume();
        float right = _parseTerm();
        result = (op == '+') ? (result + right) : (result - right);
      } else {
        break;
      }
    }
    return result;
  }

  // term → factor (('*' | '/' | '%') factor)*
  float _parseTerm() {
    _skipWhitespace();
    float result = _parseFactor();
    
    while (true) {
      _skipWhitespace();
      char op = _peek();
      if (op == '*' || op == '/' || op == '%') {
        _consume();
        float right = _parseFactor();
        if (op == '*') {
          result *= right;
        } else if (op == '/') {
          result = (right != 0) ? (result / right) : 0;
        } else if (op == '%') {
          result = fmod(result, right);
        }
      } else {
        break;
      }
    }
    return result;
  }

  // factor → '!' factor | unary
  float _parseFactor() {
    _skipWhitespace();
    
    // 논리 NOT
    if (_peek() == '!') {
      _consume();
      _skipWhitespace();
      // != 연산자와 구분 (다음이 =가 아닐 때만 NOT)
      if (_peek() != '=') {
        return (_parseFactor() == 0) ? 1.0f : 0.0f;
      } else {
        // != 연산자인 경우 위치 되돌림
        _pos--;
        return _parseUnary();
      }
    }
    
    return _parseUnary();
  }

  float _parseUnary() {
    _skipWhitespace();
    
    // 음수
    if (_peek() == '-') {
      _consume();
      return -_parseUnary();
    }
    
    // 괄호
    if (_peek() == '(') {
      _consume();
      float result = _parseConditional();
      _skipWhitespace();
      if (_peek() == ')') _consume();
      return result;
    }
    
    // 숫자
    if (isdigit(_peek()) || _peek() == '.') {
      return _parseNumber();
    }
    
    // 변수 또는 함수
    if (isalpha(_peek()) || _peek() == '_') {
      return _parseIdentifier();
    }
    
    return 0;
  }

  float _parseNumber() {
    size_t start = _pos;
    while (isdigit(_peek()) || _peek() == '.') _pos++;
    
    char buffer[32];
    size_t len = min((size_t)31, _pos - start);
    strncpy(buffer, _expr + start, len);
    buffer[len] = '\0';
    
    return atof(buffer);
  }

  float _parseIdentifier() {
    size_t start = _pos;
    while (isalnum(_peek()) || _peek() == '_') _pos++;
    
    char buffer[32];
    size_t len = min((size_t)31, _pos - start);
    strncpy(buffer, _expr + start, len);
    buffer[len] = '\0';
    
    _skipWhitespace();
    
    // 함수 호출
    if (_peek() == '(') {
      _consume();
      float arg1 = _parseConditional();
      _skipWhitespace();
      
      // 2개 인자 함수
      if (_peek() == ',') {
        _consume();
        float arg2 = _parseConditional();
        _skipWhitespace();
        if (_peek() == ')') _consume();
        
        if (strcmp(buffer, "max") == 0) return max(arg1, arg2);
        if (strcmp(buffer, "min") == 0) return min(arg1, arg2);
        if (strcmp(buffer, "mod") == 0) return fmod(arg1, arg2);
        if (strcmp(buffer, "pow") == 0) return pow(arg1, arg2);
        return 0;
      }
      
      // 1개 인자 함수
      if (_peek() == ')') _consume();
      
      if (strcmp(buffer, "sin") == 0) return sin(arg1);
      if (strcmp(buffer, "cos") == 0) return cos(arg1);
      if (strcmp(buffer, "tan") == 0) return tan(arg1);
      if (strcmp(buffer, "abs") == 0) return fabs(arg1);
      if (strcmp(buffer, "sqrt") == 0) return sqrt(arg1);
      if (strcmp(buffer, "floor") == 0) return floor(arg1);
      if (strcmp(buffer, "ceil") == 0) return ceil(arg1);
      return 0;
    }
    
    // ===== 내장 변수 =====
    if (strcmp(buffer, "theta") == 0) return _theta;
    if (strcmp(buffer, "t") == 0) return _t;
    if (strcmp(buffer, "i") == 0) return (float)_i;
    if (strcmp(buffer, "pi") == 0) return PI;
    
    // ===== ★ InPort 변수 자동 조회 =====
    float val = port_get_inport_value(buffer);
    if (!isnan(val)) {
      return val;
    }
    
    return 0;
  }
};

// 동적 패턴 컨트롤러
#include <Preferences.h>

class DynamicPattern {
public:
  struct Pattern {
    bool valid = false;
    String name;      // 패턴 이름 추가
    String hue_expr;
    String sat_expr;
    String val_expr;
  };

  // NVS 초기화 및 로드
  void begin() {
    _prefs.begin("patterns", false); // Namespace: patterns
    _loadFromNVS();
  }

  // 패턴 저장 (Slot 1~5)
  bool savePattern(int slot, const char* name, const char* hue, const char* sat, const char* val) {
    if (slot < 1 || slot > 5) return false;
    
    _patterns[slot].valid = true;
    _patterns[slot].name = name;
    _patterns[slot].hue_expr = hue;
    _patterns[slot].sat_expr = sat;
    _patterns[slot].val_expr = val;

    // NVS 저장
    _saveToNVS(slot);
    return true;
  }

  // 패턴 실행 (Slot 0 ~ 6)
  // Slot 0: 패턴 중지 (기본 눈 깜빡임으로 복귀)
  // Slot 6: 완전 소등 (Blackout)
  bool executePattern(int slot, float duration_sec) {
    if (slot == 0) {
      stop();
      return true;
    }
    
    // Slot 1~5: Saved Patterns, Slot 6: Blackout
    if (slot < 1 || slot > 6) return false;
    
    // Slot 6 doesn't need validation check (it's hardcoded logic)
    if (slot != 6 && !_patterns[slot].valid) return false;

    _current_slot = slot;
    _current_duration = duration_sec;
    _active = true;
    _start_time = millis();
    return true;
  }

  // 다음 유효한 슬롯 실행 (버튼 제어용)
  // 0 -> 1 -> 3 -> 5 -> 0 ... 순환 (Slot 6 제외)
  void cycleNextSlot() {
    int start = (_current_slot == 0) ? 0 : _current_slot;
    int next = start;
    
    // 최대 6번(0~5) 시도하여 다음 유효한 슬롯 찾기
    for (int i = 0; i < 6; i++) {
      next = (next + 1) > 5 ? 0 : (next + 1);
      
      if (next == 0) {
        // IDLE로 복귀
        stop();
        return;
      }
      
      if (_patterns[next].valid) {
        // 유효한 패턴 발견 -> 무한 실행
        executePattern(next, 0.0f);
        return;
      }
    }
    
    // 유효한 패턴이 하나도 없으면 IDLE 유지
    stop();
  }

  // 패턴 목록
  int getMaxSlots() const { return 6; } // 1-5: User, 6: Blackout
  
  const Pattern* getPattern(int slot) const {
    if (slot >= 1 && slot <= 5) return &_patterns[slot];
    return nullptr;
  }

  void stop() {
    _active = false;
    _current_slot = 0;
  }

  bool isActive() const { return _active; }

  void update(CRGB* leds, uint32_t now) {
    if (!_active || _current_slot == 0) return;
    
    // 시간 체크
    float elapsed = (now - _start_time) / 1000.0f;
    
    // Duration이 0보다 크면 시간 체크
    if (_current_duration > 0 && elapsed >= _current_duration) {
      stop();
      return;
    }

    // Slot 6: Blackout (모두 끄기)
    if (_current_slot == 6) {
      FastLED.clear();
      // Note: EyeController::update calls FastLED.show() after this returns
      // but to be safe/consistent with pattern logic, we fill leds buffer.
      // EyeController calls FastLED.show() if isActive() is true.
      // So just clearing 'leds' array is enough.
      for(int i=0; i<NUM_LEDS; i++) leds[i] = CRGB::Black;
      return;
    }
    
    float t = elapsed;
    const Pattern& p = _patterns[_current_slot];
    
    for (int i = 0; i < NUM_LEDS; i++) {
      float theta = (2.0f * PI * i) / NUM_LEDS;
      
      float h = _evaluator.eval(p.hue_expr.c_str(), theta, t, i);
      float s = _evaluator.eval(p.sat_expr.c_str(), theta, t, i);
      float v = _evaluator.eval(p.val_expr.c_str(), theta, t, i);
      
      // 정규화
      h = fmod(h, 2 * PI);
      if (h < 0) h += 2 * PI;
      
      s = constrain(s, 0.0f, 1.0f);
      v = constrain(fabs(v), 0.0f, 1.0f);
      
      // HSV → RGB
      uint8_t hue_byte = (uint8_t)((h / (2 * PI)) * 255);
      uint8_t sat_byte = (uint8_t)(s * 255);
      uint8_t val_byte = (uint8_t)(v * 255);
      
      leds[i] = CHSV(hue_byte, sat_byte, val_byte);
    }
  }

private:
  Pattern _patterns[6]; // Index 1~5 used
  int _current_slot = 0;
  float _current_duration = 0.0f;
  bool _active = false;
  uint32_t _start_time = 0;
  ExpressionEvaluator _evaluator;
  Preferences _prefs;

  void _loadFromNVS() {
    for (int i = 1; i <= 5; i++) {
      String keyPrefix = "p" + String(i) + "_";
      if (_prefs.isKey((keyPrefix + "valid").c_str())) {
        _patterns[i].valid = _prefs.getBool((keyPrefix + "valid").c_str());
        _patterns[i].name = _prefs.getString((keyPrefix + "name").c_str(), "Pattern " + String(i));
        _patterns[i].hue_expr = _prefs.getString((keyPrefix + "hue").c_str(), "0");
        _patterns[i].sat_expr = _prefs.getString((keyPrefix + "sat").c_str(), "1");
        _patterns[i].val_expr = _prefs.getString((keyPrefix + "val").c_str(), "0.5");
      }
    }
  }

  void _saveToNVS(int slot) {
    String keyPrefix = "p" + String(slot) + "_";
    _prefs.putBool((keyPrefix + "valid").c_str(), _patterns[slot].valid);
    _prefs.putString((keyPrefix + "name").c_str(), _patterns[slot].name);
    _prefs.putString((keyPrefix + "hue").c_str(), _patterns[slot].hue_expr);
    _prefs.putString((keyPrefix + "sat").c_str(), _patterns[slot].sat_expr);
    _prefs.putString((keyPrefix + "val").c_str(), _patterns[slot].val_expr);
  }
};
