#pragma once

#include <ArduinoJson.h>
#include <sdkconfig.h>

namespace hampter::internal {
class Runtime;
}

// One bounded response owned and reused by the Runtime. A Tool can return any
// JSON value that ObjectLink can encode; no per-call response object is created.
class ObservationBuilder {
 public:
  ObservationBuilder();

  void error(const char* code, const char* message);
  // Fast path for the common MCP text result. The text is copied into a fixed
  // buffer, so passing a stack-local string is safe.
  void success(const char* text);
  // Marks the value built through result() as successful.
  void success();

  // Build a structured result directly in the reusable ObjectLink envelope:
  //   JsonObject value = output.result().to<JsonObject>();
  //   value["calibrated"] = true;
  //   output.success();
  // The root may also be an array, number, boolean, string, or null.
  // The returned handle is valid only during invoke(); do not retain it or
  // pass it to another task.
  JsonVariant result();

 private:
  friend class hampter::internal::Runtime;

  static constexpr size_t kResultCapacity =
      CONFIG_HAMPTER_TOOL_RESULT_MAX_BYTES + 1;
  static constexpr size_t kErrorCodeCapacity = 97;
  static constexpr size_t kErrorMessageCapacity = 161;

  void reset(JsonVariant result);

  JsonVariant result_;
  char text_[kResultCapacity]{};
  char errorCode_[kErrorCodeCapacity]{};
  char errorMessage_[kErrorMessageCapacity]{};
  bool ok_ = false;
  bool resultOverflowed_ = false;
};

struct ITool {
  virtual ~ITool() = default;
  virtual bool init() = 0;
  virtual const char* name() const = 0;
  virtual void describe(JsonObject& tool) = 0;
  virtual bool invoke(JsonObjectConst args, ObservationBuilder& output) = 0;
};
