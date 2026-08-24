#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>

namespace {

constexpr char kSsid[] = "HAMPTER_TX_DIAG";
constexpr char kPassword[] = "hampterdiag";
constexpr uint16_t kUdpPort = 3333;
constexpr int8_t kSafePower = 34;  // 8.5 dBm, known API readback on this board.

WiFiUDP udp;
int8_t activePower = kSafePower;
uint32_t packetsEchoed = 0;

bool supportedPower(int value) {
  constexpr int kPowers[] = {8, 20, 28, 34, 44, 52, 60, 68, 74, 78};
  for (int power : kPowers) {
    if (value == power) return true;
  }
  return false;
}

void emitStatus(const char* event, esp_err_t setResult, esp_err_t getResult,
                int8_t readback) {
  Serial.printf(
      "{\"event\":\"%s\",\"reset_reason\":%d,"
      "\"requested_quarter_dbm\":%d,\"requested_dbm\":%.2f,"
      "\"set_result\":%d,\"set_result_name\":\"%s\","
      "\"get_result\":%d,\"get_result_name\":\"%s\","
      "\"readback_quarter_dbm\":%d,\"readback_dbm\":%.2f,"
      "\"ap_ip\":\"%s\",\"ssid\":\"%s\","
      "\"heap_free\":%u,\"heap_min_free\":%u,"
      "\"heap_largest\":%u,\"packets_echoed\":%u}\n",
      event, static_cast<int>(esp_reset_reason()), activePower,
      activePower * 0.25, static_cast<int>(setResult),
      esp_err_to_name(setResult), static_cast<int>(getResult),
      esp_err_to_name(getResult), readback, readback * 0.25,
      WiFi.softAPIP().toString().c_str(), kSsid,
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(
          heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
      static_cast<unsigned>(packetsEchoed));
}

void startRadio(int8_t requestedPower) {
  udp.stop();
  WiFi.softAPdisconnect(true);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_AP);

  activePower = requestedPower;
  const bool apStarted = WiFi.softAP(kSsid, kPassword, 6, false, 1);
  const esp_err_t setResult =
      apStarted ? esp_wifi_set_max_tx_power(activePower) : ESP_FAIL;
  int8_t readback = INT8_MIN;
  const esp_err_t getResult = esp_wifi_get_max_tx_power(&readback);

  udp.begin(kUdpPort);
  emitStatus(apStarted ? "tx_stage_ready" : "tx_stage_failed", setResult,
             getResult, readback);
}

void applyPower(int power) {
  if (!supportedPower(power)) {
    Serial.printf("{\"event\":\"command_rejected\","
                  "\"reason\":\"unsupported_power\",\"value\":%d}\n",
                  power);
    return;
  }
  Serial.printf("{\"event\":\"applying_power\","
                "\"requested_quarter_dbm\":%d}\n",
                power);
  Serial.flush();
  startRadio(static_cast<int8_t>(power));
}

void handleSerial() {
  static char line[32];
  static size_t length = 0;
  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\r') continue;
    if (ch != '\n' && length + 1 < sizeof(line)) {
      line[length++] = ch;
      continue;
    }
    line[length] = '\0';
    int value = 0;
    if (sscanf(line, "APPLY %d", &value) == 1) {
      applyPower(value);
    } else if (strcmp(line, "STATUS") == 0) {
      int8_t readback = INT8_MIN;
      const esp_err_t getResult = esp_wifi_get_max_tx_power(&readback);
      emitStatus("tx_stage_status", ESP_OK, getResult, readback);
    } else if (length != 0) {
      Serial.printf("{\"event\":\"command_rejected\","
                    "\"reason\":\"expected_APPLY_or_STATUS\"}\n");
    }
    length = 0;
  }
}

void handleUdp() {
  const int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  uint8_t buffer[256];
  const int received = udp.read(buffer, sizeof(buffer));
  if (received <= 0) return;
  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.write(buffer, static_cast<size_t>(received));
  udp.endPacket();
  ++packetsEchoed;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t serialDeadline = millis() + 3000;
  while (!Serial && static_cast<int32_t>(serialDeadline - millis()) > 0) {
    delay(10);
  }

  Serial.printf(
      "{\"event\":\"tx_probe_start\",\"chip\":\"%s\","
      "\"revision\":%u,\"sdk\":\"%s\","
      "\"usb_cdc\":true,\"command\":\"APPLY <quarter_dbm>\","
      "\"allowed\":[8,20,28,34,44,52,60,68,74,78]}\n",
      ESP.getChipModel(), ESP.getChipRevision(), ESP.getSdkVersion());
  startRadio(kSafePower);
}

void loop() {
  handleSerial();
  handleUdp();
  delay(1);
}
