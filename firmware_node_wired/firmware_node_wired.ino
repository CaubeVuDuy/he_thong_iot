/*
 * NODE TRẠM ĐẦU GIƯỜNG (WIRED SENSOR NODE) - 100% WI-FI LAN, KHÔNG CLOUD
 * - Sensor có dây: MAX30102 (I2C 0x57) đo nhịp tim (BPM) và SpO2
 * - Màn hình OLED 0.96" I2C SSD1315 128x64 (0x3C): chung bus SDA=21,
 * SCL=22, 3.3V, GND
 * - Actuator: còi 5V (GPIO25 qua transistor) kêu 1 tiếng khi đo xong
 * - Mỗi lần đo hợp lệ hiển thị ngay lên OLED tại giường & gửi về Local MQTT
 * Broker trên PC Y tá
 */

#include "MAX30105.h"
#include "OledSSD1315.h"
#include "PubSubClient.h"
#include "spo2_algorithm.h"
#include <WiFi.h>
#include <Wire.h>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define WIFI_SSID_CFG     "YOUR_WIFI_SSID"
#define WIFI_PASS_CFG     "YOUR_WIFI_PASSWORD"
#define SERVER_IP_CFG     "172.20.10.11"
#endif

const char *ssid = WIFI_SSID_CFG;
const char *password = WIFI_PASS_CFG;
const char *mqtt_server = SERVER_IP_CFG;
const int mqtt_port = 1883;

const char *mqtt_topic_vitals = "elderly/node_wired/vitals";
const char *mqtt_topic_health = "elderly/system/node_health";

#define BUZZER_PIN 25
#define I2C_SDA 21
#define I2C_SCL 22

#define FINGER_ON_THRESHOLD 50000
#define FINGER_OFF_THRESHOLD 30000
#define RETRY_COOLDOWN_MS 3000
#define SETTLE_SAMPLES 30
#define MAX_WINDOWS 10
#define SLIDE_SAMPLES 20
#define REQUIRED_VALID_WINDOWS 3
#define MAX_CONVERGE_BPM_SPREAD 10
#define MAX_CONVERGE_SPO2_SPREAD 4

WiFiClient espClient;
PubSubClient mqttClient(espClient);
MAX30105 particleSensor;
OledSSD1315 oled;

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

bool max30102_ready = false;
bool fingerLatched = false;
unsigned long lastSensorRetry = 0;
unsigned long lastOledRetry = 0;
unsigned long lastMeasureAttempt = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastIRPrint = 0;
unsigned long lastHealthReport = 0;
bool lastReportedMaxOk = true;
bool lastReportedOledOk = true;
int measureFailStreak = 0;

bool pendingPublish = false;
int pendingBpm = 0;
int pendingSpo2 = 0;
unsigned long pendingMeasuredAt = 0;

enum OledScreenMode {
  OLED_BOOT,
  OLED_IDLE,
  OLED_MEASURING,
  OLED_RESULT,
  OLED_FAIL,
  OLED_SENSOR_ERR
};

bool hasLastResult = false;
int lastDisplayBpm = 0;
int lastDisplaySpo2 = 0;
unsigned long lastResultAt = 0;
unsigned long failBannerUntil = 0;
int measureProgressStep = 0;
String lastOledRenderKey = "";

void renderOledScreen(OledScreenMode mode, bool force = false) {
  if (!oled.isReady())
    return;

  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bool mqttOk = wifiOk && mqttClient.connected();
  const char *netBadge = mqttOk ? "MQTT:OK" : (wifiOk ? "WIFI:OK" : "OFFLINE");
  bool hintSwap = hasLastResult && (millis() - lastResultAt > 15000);

  char keyBuf[64];
  snprintf(keyBuf, sizeof(keyBuf), "%d|%s|%d|%d|%d|%d", (int)mode, netBadge,
           lastDisplayBpm, lastDisplaySpo2, hintSwap ? 1 : 0,
           measureProgressStep);
  if (!force && lastOledRenderKey == keyBuf)
    return;
  lastOledRenderKey = keyBuf;

  oled.clear();

  if (mode == OLED_BOOT) {
    oled.drawText(0, 2, "TRAM DAU GIUONG", 1);
    oled.drawHLine(0, 12, 128);
    oled.drawTextCentered(22, "KHOI DONG", 2);
    oled.drawTextCentered(48, "Dang ket noi WiFi...", 1);
  } else if (mode == OLED_SENSOR_ERR) {
    oled.drawText(0, 2, "CANH BAO PHAN CUNG", 1);
    oled.drawHLine(0, 12, 128);
    oled.drawTextCentered(20, "LOI SENSOR", 2);
    oled.drawTextCentered(42, "Khong thay MAX30102", 1);
    oled.drawTextCentered(53, "Kiem tra day SDA/SCL", 1);
  } else if (mode == OLED_MEASURING) {
    oled.drawText(0, 2, "TRAM GIUONG   DANG DO", 1);
    oled.drawHLine(0, 12, 128);
    oled.drawTextCentered(20, "DANG DO...", 2);
    if (measureProgressStep <= 0) {
      oled.drawTextCentered(42, "On dinh ngon tay...", 1);
    } else {
      char progBuf[24];
      snprintf(progBuf, sizeof(progBuf), "Tien do: %d/%d",
               measureProgressStep, REQUIRED_VALID_WINDOWS);
      oled.drawTextCentered(42, progBuf, 1);
    }
    oled.drawTextCentered(53, "Giu yen, an nhe deu", 1);
  } else if (mode == OLED_FAIL) {
    oled.drawText(0, 2, "THONG BAO", 1);
    oled.drawText(84, 2, netBadge, 1);
    oled.drawHLine(0, 12, 128);
    oled.drawTextCentered(20, "DO LOI!", 2);
    oled.drawTextCentered(42, "Dat lai ngon tay", 1);
    oled.drawTextCentered(53, "va giu yen cho do", 1);
  } else if (mode == OLED_RESULT && hasLastResult) {
    char headerLeft[16];
    snprintf(headerLeft, sizeof(headerLeft),
             hintSwap ? "LAN DO CUOI" : "KET QUA DO");
    oled.drawText(0, 2, headerLeft, 1);
    oled.drawText(84, 2, netBadge, 1);
    oled.drawHLine(0, 12, 128);

    char bpmLine[16];
    char spo2Line[16];
    snprintf(bpmLine, sizeof(bpmLine), "BPM : %d", lastDisplayBpm);
    snprintf(spo2Line, sizeof(spo2Line), "SpO2: %d%%", lastDisplaySpo2);
    oled.drawText(4, 18, bpmLine, 2);
    oled.drawText(4, 37, spo2Line, 2);

    const char *footer =
        hintSwap ? "Dat tay de do lan moi"
                 : (mqttOk ? "Da gui ve PC Y ta" : "Hien tai giuong (OFF)");
    oled.drawTextCentered(56, footer, 1);
  } else {
    oled.drawText(0, 2, "TRAM GIUONG", 1);
    oled.drawText(84, 2, netBadge, 1);
    oled.drawHLine(0, 12, 128);
    oled.drawTextCentered(20, "DAT TAY", 2);
    oled.drawTextCentered(42, "Len cam bien MAX30102", 1);
    oled.drawTextCentered(53, "De do BPM va SpO2", 1);
  }

  if (!oled.display()) {
    Serial.println("⚠️ Mất kết nối màn hình OLED I2C (0x3C).");
  }
}

void beepDone() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(150);
  digitalWrite(BUZZER_PIN, LOW);
}

void initMAX30102() {
  if (particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    particleSensor.setup(0x3C, 4, 2, 100, 411, 4096);
    max30102_ready = true;
    Serial.println("✅ Đã nhận diện và khởi tạo MAX30102 thành công!");
  } else {
    max30102_ready = false;
  }
}

bool readSample(uint32_t &red, uint32_t &ir, unsigned long timeoutMs) {
  unsigned long t0 = millis();
  while (particleSensor.available() == 0) {
    particleSensor.check();
    if (millis() - t0 > timeoutMs)
      return false;
    delay(1);
  }
  red = particleSensor.getFIFORed();
  ir = particleSensor.getFIFOIR();
  particleSensor.nextSample();
  return true;
}

bool isWindowStable() {
  uint32_t irMin = 0xFFFFFFFF;
  uint32_t irMax = 0;
  uint32_t redMax = 0;
  uint64_t irSum = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    uint32_t irVal = irBuffer[i];
    uint32_t redVal = redBuffer[i];
    if (irVal < irMin) irMin = irVal;
    if (irVal > irMax) irMax = irVal;
    if (redVal > redMax) redMax = redVal;
    irSum += irVal;
  }
  uint32_t irMean = (uint32_t)(irSum / BUFFER_SIZE);
  if (irMean < FINGER_ON_THRESHOLD)
    return false;
  if (irMax >= 260000 || redMax >= 260000) {
    Serial.println("⚠️ Tín hiệu quang bị bão hòa, hãy đặt ngón tay nhẹ hơn.");
    return false;
  }
  // Peak-to-peak DC swing > 25% of mean indicates finger movement artifact
  if ((uint64_t)(irMax - irMin) * 4 > irMean) {
    Serial.println("⚠️ Ngón tay hơi rung, đang bỏ qua cửa sổ nhiễu...");
    return false;
  }
  return true;
}

static void sort3(int &a, int &b, int &c) {
  int t;
  if (a > b) { t = a; a = b; b = t; }
  if (b > c) { t = b; b = c; c = t; }
  if (a > b) { t = a; a = b; b = t; }
}

bool measureVitals(int &bpm, int &spo2) {
  particleSensor.clearFIFO();
  while (particleSensor.available())
    particleSensor.nextSample();

  measureProgressStep = 0;
  renderOledScreen(OLED_MEASURING, true);

  // Discard initial ~1.2s after finger touch so DC baseline & pressure settle
  for (int s = 0; s < SETTLE_SAMPLES; s++) {
    uint32_t r, i;
    if (!readSample(r, i, 500))
      return false;
    if (i < FINGER_OFF_THRESHOLD) {
      Serial.println("⚠️ Ngón tay bị nhấc ra trong lúc ổn định.");
      return false;
    }
    if (s % 10 == 0)
      mqttClient.loop();
  }

  int validBpms[MAX_WINDOWS];
  int validSpo2s[MAX_WINDOWS];
  int validCount = 0;

  int pos = 0;
  for (int window = 0; window < MAX_WINDOWS; window++) {
    while (pos < BUFFER_SIZE) {
      uint32_t r, i;
      if (!readSample(r, i, 500)) {
        Serial.println("⚠️ Mất dữ liệu từ cảm biến khi đang đo.");
        return false;
      }
      if (i < FINGER_OFF_THRESHOLD) {
        Serial.println("⚠️ Ngón tay bị nhấc ra giữa chừng, hãy giữ yên tay.");
        return false;
      }
      redBuffer[pos] = r;
      irBuffer[pos] = i;
      pos++;
      if (pos % SLIDE_SAMPLES == 0)
        mqttClient.loop();
    }

    if (isWindowStable()) {
      int32_t spo2Value, hrValue;
      int8_t spo2Valid, hrValid;
      maxim_heart_rate_and_oxygen_saturation(irBuffer, BUFFER_SIZE, redBuffer,
                                             &spo2Value, &spo2Valid, &hrValue,
                                             &hrValid);

      if (spo2Valid && hrValid && hrValue >= 45 && hrValue <= 175 &&
          spo2Value >= 80 && spo2Value <= 100) {
        validBpms[validCount] = (int)hrValue;
        validSpo2s[validCount] = (int)spo2Value;
        validCount++;
        Serial.printf("  [Cửa sổ %d/%d] BPM=%d | SpO2=%d%%\n", window + 1,
                      MAX_WINDOWS, (int)hrValue, (int)spo2Value);

        measureProgressStep = (validCount < REQUIRED_VALID_WINDOWS)
                                  ? validCount
                                  : REQUIRED_VALID_WINDOWS;
        renderOledScreen(OLED_MEASURING);

        if (validCount >= REQUIRED_VALID_WINDOWS) {
          int b0 = validBpms[validCount - 3];
          int b1 = validBpms[validCount - 2];
          int b2 = validBpms[validCount - 1];
          int s0 = validSpo2s[validCount - 3];
          int s1 = validSpo2s[validCount - 2];
          int s2 = validSpo2s[validCount - 1];
          sort3(b0, b1, b2);
          sort3(s0, s1, s2);
          if ((b2 - b0) <= MAX_CONVERGE_BPM_SPREAD &&
              (s2 - s0) <= MAX_CONVERGE_SPO2_SPREAD) {
            bpm = b1;
            spo2 = s1;
            return true;
          }
        }
      }
    }

    memmove(redBuffer, redBuffer + SLIDE_SAMPLES,
            (BUFFER_SIZE - SLIDE_SAMPLES) * sizeof(uint32_t));
    memmove(irBuffer, irBuffer + SLIDE_SAMPLES,
            (BUFFER_SIZE - SLIDE_SAMPLES) * sizeof(uint32_t));
    pos = BUFFER_SIZE - SLIDE_SAMPLES;
  }

  // Fallback if 3+ valid windows exist but drifted slightly: pick closest consistent pair
  if (validCount >= 3) {
    int bestGap = 999;
    int bestBpm = 0;
    int bestSpo2 = 0;
    for (int i = 0; i < validCount; i++) {
      for (int j = i + 1; j < validCount; j++) {
        int bGap = abs(validBpms[i] - validBpms[j]);
        int sGap = abs(validSpo2s[i] - validSpo2s[j]);
        if (bGap <= MAX_CONVERGE_BPM_SPREAD &&
            sGap <= MAX_CONVERGE_SPO2_SPREAD && bGap < bestGap) {
          bestGap = bGap;
          bestBpm = (validBpms[i] + validBpms[j] + 1) / 2;
          bestSpo2 = (validSpo2s[i] + validSpo2s[j] + 1) / 2;
        }
      }
    }
    if (bestGap < 999) {
      bpm = bestBpm;
      spo2 = bestSpo2;
      return true;
    }
  }
  return false;
}

bool publishPendingVitals() {
  if (!pendingPublish || !mqttClient.connected())
    return false;
  String payload = "{\"node\":\"BEDSIDE_WIRED\",\"bpm\":" + String(pendingBpm) +
                   ",\"spo2\":" + String(pendingSpo2) +
                   ",\"age_ms\":" + String(millis() - pendingMeasuredAt) + "}";
  if (mqttClient.publish(mqtt_topic_vitals, payload.c_str())) {
    pendingPublish = false;
    Serial.println("📡 Đã gửi kết quả đo về PC Y tá: " + payload);
    return true;
  }
  return false;
}

void publishBedsideHealth(const char *status, const char *errCode,
                          const char *detail, bool force) {
  if (!mqttClient.connected())
    return;
  if (!force && millis() - lastHealthReport < 10000)
    return;
  lastHealthReport = millis();
  String payload = "{\"node\":\"BEDSIDE_NODE\",\"status\":\"";
  payload += status;
  payload += "\",\"error_code\":\"";
  payload += errCode;
  payload += "\",\"detail\":\"";
  payload += detail;
  payload += "\"}";
  mqttClient.publish(mqtt_topic_health, payload.c_str());
}

void reconnectMQTT() {
  static unsigned long lastReconnect = 0;
  if (millis() - lastReconnect < 5000)
    return;
  lastReconnect = millis();
  String clientId = "ESP32_Bedside_" + String(random(0xffff), HEX);
  if (mqttClient.connect(clientId.c_str())) {
    Serial.println("✅ [MQTT] Đã kết nối Local Broker trên PC Y tá!");
    if (max30102_ready) {
      publishBedsideHealth("OK", "NONE", "MAX30102 0x57 san sang", true);
      lastReportedMaxOk = true;
    } else {
      publishBedsideHealth("ERROR", "MAX30102_NOT_FOUND",
                           "Khong tim thay cam bien MAX30102 tai dia chi 0x57",
                           true);
      lastReportedMaxOk = false;
    }
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED)
    return;
  if (millis() - lastWifiRetry < 10000)
    return;
  lastWifiRetry = millis();
  Serial.println("🔄 Mất Wi-Fi, đang kết nối lại...");
  WiFi.disconnect();
  WiFi.begin(ssid, password);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(
      "\n==============================================================");
  Serial.println(
      "   TRẠM ĐẦU GIƯỜNG - ĐO NHỊP TIM & SpO2 (WI-FI LAN NỘI BỘ)   ");
  Serial.println(
      "==============================================================");

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  if (oled.begin(Wire)) {
    Serial.printf("✅ Đã nhận màn hình OLED SSD1315 tại địa chỉ 0x%02X\n",
                  oled.getAddress());
    renderOledScreen(OLED_BOOT, true);
  } else {
    Serial.println("⚠️ Không thấy màn hình OLED tại 0x3C/0x3D. Tiếp tục chạy "
                   "không có màn hình.");
  }

  Serial.print("Đang kết nối Wi-Fi LAN: ");
  Serial.println(ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n✅ Wi-Fi đã kết nối! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n⚠️ Chưa kết nối được Wi-Fi LAN, sẽ tự thử lại trong nền.");
  }

  mqttClient.setServer(mqtt_server, mqtt_port);

  Wire.beginTransmission(0x57);
  if (Wire.endTransmission() == 0) {
    Serial.println("-> Tìm thấy MAX30102 tại địa chỉ 0x57");
  } else {
    Serial.println("⚠️ Không thấy địa chỉ 0x57 trên bus I2C. Kiểm tra lại dây "
                   "VIN/GND/SCL/SDA.");
  }
  initMAX30102();

  beepDone();
  renderOledScreen(max30102_ready ? OLED_IDLE : OLED_SENSOR_ERR, true);
  Serial.println("Sẵn sàng. Đặt ngón tay lên cảm biến để đo.");
}

void loop() {
  maintainWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected())
      reconnectMQTT();
    mqttClient.loop();
    if (publishPendingVitals() && hasLastResult) {
      renderOledScreen(OLED_RESULT, true);
    }
  }

  if (!oled.isReady() && millis() - lastOledRetry > 5000) {
    lastOledRetry = millis();
    if (oled.begin(Wire)) {
      Serial.printf("✅ Màn hình OLED (0x%02X) đã phục hồi kết nối.\n",
                    oled.getAddress());
      lastOledRenderKey = "";
      if (max30102_ready && !lastReportedOledOk) {
        publishBedsideHealth("OK", "NONE", "MAX30102 & OLED san sang", true);
      }
      lastReportedOledOk = true;
    } else if (max30102_ready && lastReportedOledOk) {
      publishBedsideHealth("WARNING", "OLED_NOT_FOUND",
                           "Khong thay man hinh OLED I2C (0x3C)", true);
      lastReportedOledOk = false;
    }
  }

  if (!max30102_ready) {
    renderOledScreen(OLED_SENSOR_ERR);
    publishBedsideHealth("ERROR", "MAX30102_NOT_FOUND",
                         "Khong tim thay cam bien MAX30102 tai dia chi 0x57 "
                         "(long day hoac hong cam bien)",
                         lastReportedMaxOk);
    lastReportedMaxOk = false;
    if (millis() - lastSensorRetry > 2000) {
      lastSensorRetry = millis();
      initMAX30102();
      if (max30102_ready) {
        publishBedsideHealth("OK", "NONE", "MAX30102 0x57 da phuc hoi ket noi",
                             true);
        lastReportedMaxOk = true;
      }
    }
    delay(50);
    return;
  }

  long irValue = particleSensor.getIR();

  if (irValue == 0) {
    max30102_ready = false;
    renderOledScreen(OLED_SENSOR_ERR);
    publishBedsideHealth(
        "ERROR", "MAX30102_SIGNAL_LOST",
        "Mat tin hieu dot ngot tu cam bien MAX30102 (irValue = 0)", true);
    lastReportedMaxOk = false;
    Serial.println("🔄 Mất tín hiệu từ MAX30102, đang khởi tạo lại...");
    return;
  }

  publishBedsideHealth("OK", "NONE", "MAX30102 0x57 san sang",
                       !lastReportedMaxOk);
  lastReportedMaxOk = true;

  if (irValue < FINGER_OFF_THRESHOLD) {
    fingerLatched = false;
    if (millis() < failBannerUntil) {
      renderOledScreen(OLED_FAIL);
    } else {
      renderOledScreen(hasLastResult ? OLED_RESULT : OLED_IDLE);
    }
    if (millis() - lastIRPrint > 3000) {
      lastIRPrint = millis();
      Serial.printf("⏳ Đang chờ ngón tay... (IR: %ld / ngưỡng: %d)\n", irValue,
                    FINGER_ON_THRESHOLD);
    }
  } else if (irValue >= FINGER_ON_THRESHOLD && !fingerLatched &&
             millis() - lastMeasureAttempt > RETRY_COOLDOWN_MS) {
    lastMeasureAttempt = millis();
    Serial.println(
        "👉 Phát hiện ngón tay! Giữ yên, đang đo nhịp tim & SpO2...");

    int bpm, spo2;
    if (measureVitals(bpm, spo2)) {
      measureFailStreak = 0;
      pendingBpm = bpm;
      pendingSpo2 = spo2;
      pendingMeasuredAt = millis();
      pendingPublish = true;
      fingerLatched = true;
      hasLastResult = true;
      lastDisplayBpm = bpm;
      lastDisplaySpo2 = spo2;
      lastResultAt = millis();
      failBannerUntil = 0;
      beepDone();
      Serial.printf("❤️ Kết quả đo: %d BPM | SpO2 = %d %%\n", bpm, spo2);
      publishPendingVitals();
      renderOledScreen(OLED_RESULT, true);
    } else {
      measureFailStreak++;
      failBannerUntil = millis() + 2500;
      renderOledScreen(OLED_FAIL, true);
      if (measureFailStreak >= 3) {
        publishBedsideHealth("WARNING", "MAX30102_MEASURE_FAIL",
                             "Do nhip tim/SpO2 that bai nhieu lan lien tiep",
                             true);
      }
      Serial.println(
          "⚠️ Đo chưa hợp lệ, hãy đặt lại ngón tay nhẹ nhàng và giữ yên.");
    }
  } else {
    if (millis() < failBannerUntil) {
      renderOledScreen(OLED_FAIL);
    } else {
      renderOledScreen(hasLastResult ? OLED_RESULT : OLED_IDLE);
    }
  }

  delay(10);
}
