#include "HampterToolRegistry.h"

#include <string.h>

namespace {

template <size_t Capacity>
bool copyBounded(const char* source, char (&destination)[Capacity]) {
  static_assert(Capacity > 0);
  if (source == nullptr) source = "";
  const size_t length = strnlen(source, Capacity);
  const bool fits = length < Capacity;
  const size_t copied = fits ? length : Capacity - 1;
  memcpy(destination, source, copied);
  destination[copied] = '\0';
  return fits;
}

}  // namespace

ObservationBuilder::ObservationBuilder() = default;

void ObservationBuilder::reset(JsonVariant result) {
  result_ = result;
  text_[0] = '\0';
  errorCode_[0] = '\0';
  errorMessage_[0] = '\0';
  ok_ = false;
  resultOverflowed_ = false;
}

void ObservationBuilder::error(const char* code, const char* message) {
  text_[0] = '\0';
  result_.set(nullptr);
  ok_ = false;
  resultOverflowed_ = false;
  (void)copyBounded(code ? code : "error", errorCode_);
  (void)copyBounded(message, errorMessage_);
}

void ObservationBuilder::success(const char* text) {
  errorCode_[0] = '\0';
  errorMessage_[0] = '\0';
  ok_ = true;
  resultOverflowed_ = !copyBounded(text, text_);
  // Store a linked pointer into this Runtime-owned buffer. It remains valid
  // until the result is serialized before the next Tool job is handled.
  result_.set(JsonString(text_, true));
}

void ObservationBuilder::success() {
  errorCode_[0] = '\0';
  errorMessage_[0] = '\0';
  ok_ = true;
}

JsonVariant ObservationBuilder::result() { return result_; }

void ToolRegistry::add(ITool* tool) {
  if (tool == nullptr) {
    error_ = "null Tool";
    return;
  }
  if (count_ >= kCapacity) {
    error_ = "Tool registry capacity exceeded";
    return;
  }
  const char* candidate = tool->name();
  if (candidate == nullptr || candidate[0] == '\0') {
    error_ = "Tool name is empty";
    return;
  }
  if (find(candidate) != nullptr) {
    error_ = "duplicate Tool name";
    return;
  }
  tools_[count_++] = tool;
}

bool ToolRegistry::initAll() {
  if (!valid()) return false;
  for (size_t i = 0; i < count_; ++i) {
    if (!tools_[i]->init()) {
      error_ = "Tool init failed";
      return false;
    }
  }
  return true;
}

ITool* ToolRegistry::at(size_t index) const {
  return index < count_ ? tools_[index] : nullptr;
}

ITool* ToolRegistry::find(const char* name) const {
  if (name == nullptr) return nullptr;
  for (size_t i = 0; i < count_; ++i) {
    if (strcmp(tools_[i]->name(), name) == 0) return tools_[i];
  }
  return nullptr;
}
