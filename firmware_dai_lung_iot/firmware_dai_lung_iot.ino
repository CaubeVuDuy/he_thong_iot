/*
 * =========================================================================
 * FIRMWARE ĐAI LƯNG IoT VIỆN DƯỠNG LÃO (KHÔNG CLOUD - 100% LOCAL LAN)
 * - Thừa hưởng thuật toán MPU6050 + AI TinyML từ Đồ án chuyên ngành
 * - Loại bỏ hoàn toàn Render Cloud HTTPS
 * - Tích hợp Local MQTT Broker (172.20.10.11:1883) kết nối tức thì về Web Y tá & Trạm giường
 * =========================================================================
 */

#include <WiFi.h>
#include <esp_wifi.h>
#include "PubSubClient.h"
#include <te_nga_inferencing.h>

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define WIFI_SSID_CFG     "YOUR_WIFI_SSID"
#define WIFI_PASS_CFG     "YOUR_WIFI_PASSWORD"
#define SERVER_IP_CFG     "172.20.10.11"
#endif

const char *WIFI_STA_SSID = WIFI_SSID_CFG;
const char *WIFI_STA_PASS = WIFI_PASS_CFG;
const char *MQTT_SERVER   = SERVER_IP_CFG;
const int   MQTT_PORT     = 1883;

const char *MQTT_TOPIC_FALL   = "elderly/node_wireless/fall";
const char *MQTT_TOPIC_CMD    = "elderly/actuator/buzzer_control";
const char *MQTT_TOPIC_HEALTH = "elderly/system/node_health";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// === CHÂN PHẦN CỨNG NGOẠI VI (CHUẨN THEO MẠCH ĐAI LƯNG THỰC TẾ) ===
#define BUZZER_PIN 4
#define LED_PIN    2

int mpu_addr = 0x68;
int sda_pin = 22;
int scl_pin = 21;
bool mpu_connected = false;
float accel_lsb_per_g = 4096.0;
float gyro_lsb_per_deg = 65.5;
int i2c_fail_count = 0;

float ai_threshold_cfg = 0.70;

// === BỘ ĐỆM AI TINYML (gom 1 giây = 50 mẫu x 6 trục) ===
float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
int feature_ix = 0;
float ai_te_nga = 0.0;

// === BỘ ĐỆM ĐỘNG HỌC 3D ===
#define HIST_SIZE 25
float a_hist[HIST_SIZE];
int hist_idx = 0;
int sample_count = 0;

float peak_acc = 0.0;
float peak_gyro = 0.0;
float peak_lateral_acc = 0.0;
volatile float live_total_a = 9.8;
volatile float live_std_dev = 0.0;
float live_tilt_angle = 0.0;

// === TRẠNG THÁI HỆ THỐNG ===
volatile bool is_fallen = false;
volatile bool fall_pending_sync = false;
unsigned long fall_trigger_time = 0;
int recovery_count = 0;

int walk_score = 0;
int rest_score = 0;
String current_stable_status = "DUNG / NGOI YEN";
String last_status = "";

// === QUẢN LÝ CÒI BÁO ĐỘNG (ACTIVE BUZZER 5V - GPIO 4) ===
unsigned long last_buzzer_tick = 0;
volatile bool buzzer_output_state = false;
unsigned long last_heartbeat_time = 0;
unsigned long last_mqtt_sync = 0;
unsigned long last_health_report = 0;
bool last_reported_mpu_ok = true;
int consecutive_noise_samples = 0;

// =========================================================================
// GIAO TIẾP SOFTWARE I2C CHỐNG TREO BUS TRÊN ESP32
// =========================================================================
inline void sdaHigh() { pinMode(sda_pin, INPUT_PULLUP); }
inline void sdaLow() {
  digitalWrite(sda_pin, LOW);
  pinMode(sda_pin, OUTPUT);
}
inline void sclHigh() {
  pinMode(scl_pin, INPUT_PULLUP);
  uint32_t t0 = micros();
  while (digitalRead(scl_pin) == LOW && (micros() - t0) < 250) {
  }
}
inline void sclLow() {
  digitalWrite(scl_pin, LOW);
  pinMode(scl_pin, OUTPUT);
}
inline void i2cDelay() { delayMicroseconds(5); }

void swI2cBusRecover() {
  sdaHigh();
  sclHigh();
  delayMicroseconds(5);
  if (digitalRead(sda_pin) == LOW) {
    for (int i = 0; i < 9; i++) {
      sclLow();
      delayMicroseconds(5);
      sclHigh();
      delayMicroseconds(5);
      if (digitalRead(sda_pin) == HIGH)
        break;
    }
  }
}

void swI2cStart() {
  sdaHigh();
  sclHigh();
  i2cDelay();
  sdaLow();
  i2cDelay();
  sclLow();
  i2cDelay();
}

void swI2cStop() {
  sdaLow();
  i2cDelay();
  sclHigh();
  i2cDelay();
  sdaHigh();
  i2cDelay();
}

bool swI2cWriteByte(uint8_t data) {
  for (int i = 7; i >= 0; i--) {
    if (data & (1 << i))
      sdaHigh();
    else
      sdaLow();
    delayMicroseconds(3);
    sclHigh();
    i2cDelay();
    sclLow();
    delayMicroseconds(3);
  }
  sdaHigh();
  delayMicroseconds(3);
  sclHigh();
  i2cDelay();
  bool ack = (digitalRead(sda_pin) == LOW);
  sclLow();
  delayMicroseconds(3);
  return ack;
}

uint8_t swI2cReadByte(bool ack) {
  sdaHigh();
  uint8_t data = 0;
  for (int i = 7; i >= 0; i--) {
    delayMicroseconds(3);
    sclHigh();
    i2cDelay();
    if (digitalRead(sda_pin) == HIGH)
      data |= (1 << i);
    sclLow();
    delayMicroseconds(3);
  }
  if (ack)
    sdaLow();
  else
    sdaHigh();
  delayMicroseconds(3);
  sclHigh();
  i2cDelay();
  sclLow();
  delayMicroseconds(3);
  sdaHigh();
  return data;
}

bool mpuWriteReg(int addr, uint8_t reg, uint8_t val) {
  swI2cStart();
  if (!swI2cWriteByte((addr << 1) | 0)) {
    swI2cStop();
    return false;
  }
  if (!swI2cWriteByte(reg)) {
    swI2cStop();
    return false;
  }
  if (!swI2cWriteByte(val)) {
    swI2cStop();
    return false;
  }
  swI2cStop();
  return true;
}

bool mpuReadBytes(int addr, uint8_t startReg, uint8_t *buf, int len) {
  swI2cStart();
  if (!swI2cWriteByte((addr << 1) | 0)) {
    swI2cStop();
    return false;
  }
  if (!swI2cWriteByte(startReg)) {
    swI2cStop();
    return false;
  }
  swI2cStart();
  if (!swI2cWriteByte((addr << 1) | 1)) {
    swI2cStop();
    return false;
  }
  for (int i = 0; i < len; i++) {
    buf[i] = swI2cReadByte(i < (len - 1));
  }
  swI2cStop();
  return true;
}

bool initAndConfigMPU(int sda, int scl, int addr) {
  sda_pin = sda;
  scl_pin = scl;
  swI2cBusRecover();

  if (!mpuWriteReg(addr, 0x6B, 0x00))
    return false;
  delay(5);

  uint8_t who = 0;
  if (!mpuReadBytes(addr, 0x75, &who, 1))
    return false;
  if (who == 0x00 || who == 0xFF)
    return false;

  mpuWriteReg(addr, 0x6C, 0x00);

  // Thang đo ±8g (4096 LSB/g)
  mpuWriteReg(addr, 0x1C, 0x10);
  uint8_t reg1C = 0;
  if (mpuReadBytes(addr, 0x1C, &reg1C, 1) && ((reg1C & 0x18) == 0x10)) {
    accel_lsb_per_g = 4096.0;
  } else {
    accel_lsb_per_g = 16384.0;
  }

  // Thang đo ±500 deg/s (65.5 LSB/deg/s)
  mpuWriteReg(addr, 0x1B, 0x08);
  uint8_t reg1B = 0;
  if (mpuReadBytes(addr, 0x1B, &reg1B, 1) && ((reg1B & 0x18) == 0x08)) {
    gyro_lsb_per_deg = 65.5;
  } else {
    gyro_lsb_per_deg = 131.0;
  }

  mpu_addr = addr;
  mpu_connected = true;
  Serial.printf(">>> [MPU6050 OK] (SDA:D%d, SCL:D%d, Addr:0x%02X, WHO_AM_I:0x%02X)\n",
                sda_pin, scl_pin, mpu_addr, who);
  return true;
}

bool autoDetectMPU() {
  int addrs[2] = {0x68, 0x69};
  int primaryPairs[2][2] = {{22, 21}, {21, 22}};
  for (int p = 0; p < 2; p++) {
    for (int a = 0; a < 2; a++) {
      if (initAndConfigMPU(primaryPairs[p][0], primaryPairs[p][1], addrs[a])) {
        return true;
      }
    }
  }

  int candidatePins[] = {22, 21, 23, 19, 18, 5, 15, 25, 26, 27, 32, 33};
  int numPins = sizeof(candidatePins) / sizeof(candidatePins[0]);
  for (int i = 0; i < numPins; i++) {
    for (int j = 0; j < numPins; j++) {
      if (i == j) continue;
      int sda = candidatePins[i];
      int scl = candidatePins[j];
      if ((sda == 22 && scl == 21) || (sda == 21 && scl == 22)) continue;
      for (int a = 0; a < 2; a++) {
        if (initAndConfigMPU(sda, scl, addrs[a])) {
          return true;
        }
      }
    }
  }

  mpu_connected = false;
  Serial.println(">>> [LOI MPU6050] Khong tim thay cam bien MPU6050!");
  return false;
}

// ================= ĐIỀU KHIỂN CÒI & CẢNH BÁO =================
void batCoiBuzzer() {
  is_fallen = true;
  fall_pending_sync = true;
  fall_trigger_time = millis();
  recovery_count = 0;
  current_stable_status = "TE NGA NGUY HIEM";
  Serial.println("\n🚨 [CẢNH BÁO] PHÁT HIỆN TÉ NGÃ! Kích hoạt còi báo động tại chỗ!");
}

void tatCoiBuzzer() {
  buzzer_output_state = false;
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
}

void capNhatCoiBuzzer() {
  if (!is_fallen) {
    tatCoiBuzzer();
    return;
  }

  unsigned long current_time = millis();
  if (current_time - fall_trigger_time > 3000) {
    tatCoiBuzzer();
    return;
  }

  unsigned long interval = (current_time - fall_trigger_time <= 1200) ? 70 : 110;
  if (current_time - last_buzzer_tick >= interval) {
    last_buzzer_tick = current_time;
    buzzer_output_state = !buzzer_output_state;
    digitalWrite(BUZZER_PIN, buzzer_output_state ? HIGH : LOW);
    digitalWrite(LED_PIN, buzzer_output_state ? HIGH : LOW);
  }
}

// ================= GIAO TIẾP LOCAL MQTT TỚI SERVER & CỔNG BIÊN =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.println(">>> [MQTT Nhan] Topic: " + String(topic) + " | Noi dung: " + msg);

  // Lắng nghe lệnh tắt còi từ Y tá (Web PC hoặc Trạm đầu giường)
  if (strcmp(topic, MQTT_TOPIC_CMD) == 0) {
    if (msg.indexOf("BUZZER_OFF") >= 0) {
      is_fallen = false;
      fall_pending_sync = false;
      tatCoiBuzzer();
      current_stable_status = "DUNG / NGOI YEN";
      Serial.println("🔇 [Lệnh Y tá] Đã tắt còi báo động từ xa!");
    }
  }
}

void sendFallAlertToMQTT(bool isFall) {
  if (!mqttClient.connected()) return;

  float impact = (live_total_a / 9.80665);
  String payload = "{";
  payload += "\"gateway_id\":\"BELT_NODE_WIRELESS\",";
  payload += "\"status\":\"" + String(isFall ? "FALL_DETECTED" : "SAFE") + "\",";
  payload += "\"impact_g\":" + String(impact, 2) + ",";
  payload += "\"tilt_angle\":" + String(live_tilt_angle, 1) + ",";
  payload += "\"ai_score\":" + String(ai_te_nga, 2) + ",";
  payload += "\"state_desc\":\"" + current_stable_status + "\",";
  payload += "\"buzzer\":" + String(buzzer_output_state ? "true" : "false");
  payload += "}";

  if (mqttClient.publish(MQTT_TOPIC_FALL, payload.c_str())) {
    if (isFall) fall_pending_sync = false;
    Serial.println("📡 [MQTT Đai Lưng] Đã đẩy dữ liệu về PC Y tá:");
    Serial.println(payload);
  }
}

void publishBeltHealth(const char *status, const char *errCode, const char *detail, bool force) {
  if (!mqttClient.connected()) return;
  if (!force && millis() - last_health_report < 10000) return;
  last_health_report = millis();
  String payload = "{\"node\":\"BELT_NODE\",\"status\":\"";
  payload += status;
  payload += "\",\"error_code\":\"";
  payload += errCode;
  payload += "\",\"detail\":\"";
  payload += detail;
  payload += "\"}";
  mqttClient.publish(MQTT_TOPIC_HEALTH, payload.c_str());
}

void reconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  static unsigned long lastReconnect = 0;
  if (millis() - lastReconnect > 5000) {
    lastReconnect = millis();
    String clientId = "ESP32_Belt_" + String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("✅ [MQTT] Đai lưng đã kết nối Local Broker trên PC Y tá!");
      mqttClient.subscribe(MQTT_TOPIC_CMD);
      if (mpu_connected) {
        publishBeltHealth("OK", "NONE", "MPU6050 san sang", true);
        last_reported_mpu_ok = true;
      } else {
        publishBeltHealth("ERROR", "MPU6050_DISCONNECTED", "Khong tim thay cam bien MPU6050", true);
        last_reported_mpu_ok = false;
      }
      if (fall_pending_sync && is_fallen) {
        sendFallAlertToMQTT(true);
      }
    }
  }
}

// ================= KẾT NỐI WI-FI LAN NỘI BỘ =================
void connectWiFi() {
  Serial.println(">>> [WIFI] Dang ket noi Wi-Fi LAN Hotspot: " + String(WIFI_STA_SSID));
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK);
  WiFi.persistent(false);
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 25) {
    delay(400);
    Serial.print(".");
    retries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("✅ [WIFI OK] Đai lưng đã kết nối! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("⚠️ [WIFI] Chưa kết nối được Wi-Fi. Chạy chế độ nội bộ MPU6050, tự kết nối lại sau.");
  }
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  static unsigned long lastWifiRetry = 0;
  if (millis() - lastWifiRetry >= 10000) {
    lastWifiRetry = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=======================================================");
  Serial.println("   KHỞI ĐỘNG ĐAI LƯNG IoT BÁO NGÃ (LOCAL MQTT EDITION) ");
  Serial.println("=======================================================");

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);

  // 1. Khởi tạo MPU6050 (Software I2C chống treo bus)
  autoDetectMPU();

  // Bíp 1 tiếng ngắn báo phần cứng sẵn sàng
  digitalWrite(BUZZER_PIN, HIGH); delay(80); digitalWrite(BUZZER_PIN, LOW);
  for (int i = 0; i < HIST_SIZE; i++) a_hist[i] = 9.8;

  // 3. Kết nối Wi-Fi LAN & cấu hình Local MQTT
  connectWiFi();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);

  Serial.println("✅ Đai lưng IoT đã sẵn sàng giám sát!");
}

// ================= LOOP =================
void loop() {
  // 1. Duy trì kết nối Wi-Fi & MQTT
  maintainWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) reconnectMQTT();
    mqttClient.loop();
  }

  // 2. Tự phục hồi MPU6050 nếu mất kết nối
  if (!mpu_connected) {
    publishBeltHealth("ERROR", "MPU6050_DISCONNECTED", "Mat ket noi / treo bus I2C cam bien MPU6050", last_reported_mpu_ok);
    last_reported_mpu_ok = false;
    static unsigned long last_scan = 0;
    if (millis() - last_scan >= 1000) {
      last_scan = millis();
      if (autoDetectMPU()) {
        publishBeltHealth("OK", "NONE", "MPU6050 da phuc hoi ket noi", true);
        last_reported_mpu_ok = true;
      }
    }
    capNhatCoiBuzzer();
    delay(20);
    return;
  }

  uint8_t raw_buf[14];
  if (!mpuReadBytes(mpu_addr, 0x3B, raw_buf, 14)) {
    i2c_fail_count++;
    if (i2c_fail_count >= 5) {
      i2c_fail_count = 0;
      mpu_connected = false;
      publishBeltHealth("ERROR", "MPU6050_I2C_FAIL", "Loi doc thanh ghi I2C MPU6050 5 lan lien tiep", true);
      last_reported_mpu_ok = false;
    }
    capNhatCoiBuzzer();
    delay(20);
    return;
  }

  i2c_fail_count = 0;

  int16_t raw_ax = (int16_t)((raw_buf[0] << 8) | raw_buf[1]);
  int16_t raw_ay = (int16_t)((raw_buf[2] << 8) | raw_buf[3]);
  int16_t raw_az = (int16_t)((raw_buf[4] << 8) | raw_buf[5]);
  int16_t raw_gx = (int16_t)((raw_buf[8] << 8) | raw_buf[9]);
  int16_t raw_gy = (int16_t)((raw_buf[10] << 8) | raw_buf[11]);
  int16_t raw_gz = (int16_t)((raw_buf[12] << 8) | raw_buf[13]);

  if (raw_ax == 0 && raw_ay == 0 && raw_az == 0 && raw_gx == 0 && raw_gy == 0 && raw_gz == 0) {
    initAndConfigMPU(sda_pin, scl_pin, mpu_addr);
    capNhatCoiBuzzer();
    delay(20);
    return;
  }

  float ax = (raw_ax / accel_lsb_per_g) * 9.80665;
  float ay = (raw_ay / accel_lsb_per_g) * 9.80665;
  float az = (raw_az / accel_lsb_per_g) * 9.80665;
  float gx = raw_gx / gyro_lsb_per_deg;
  float gy = raw_gy / gyro_lsb_per_deg;
  float gz = raw_gz / gyro_lsb_per_deg;

  float total_a = sqrt(ax * ax + ay * ay + az * az);
  float total_g = sqrt(gx * gx + gy * gy + gz * gz);

  // Lọc bỏ mẫu nhiễu I2C bất thường (ví dụ mẫu 0.14G = 1.37 m/s2 do sụt bit I2C)
  if (total_a < 3.5 || total_a > 78.0 || total_g > 1500.0) {
    consecutive_noise_samples++;
    if (consecutive_noise_samples == 15) {
      publishBeltHealth("WARNING", "MPU6050_NOISE", "Nhieu du lieu I2C bat thuong tu MPU6050", true);
    }
    capNhatCoiBuzzer();
    delay(5);
    return;
  }
  consecutive_noise_samples = 0;
  publishBeltHealth("OK", "NONE", "MPU6050 hoat dong binh thuong", !last_reported_mpu_ok);
  last_reported_mpu_ok = true;

  live_total_a = total_a;

  // Tính toán góc nghiêng Euler (Tilt angle so với phương thẳng đứng)
  if (total_a > 0.1) {
    float cos_tilt = abs(az) / total_a;
    if (cos_tilt > 1.0) cos_tilt = 1.0;
    live_tilt_angle = acos(cos_tilt) * 57.2958; // Ra độ
  }

  if (total_a > peak_acc) peak_acc = total_a;
  if (total_g > peak_gyro) peak_gyro = total_g;
  float max_horiz = max(abs(ax), abs(ay));
  if (max_horiz > peak_lateral_acc) peak_lateral_acc = max_horiz;

  a_hist[hist_idx] = total_a;
  hist_idx = (hist_idx + 1) % HIST_SIZE;

  // Gom mẫu cho AI TinyML
  if (feature_ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
    features[feature_ix++] = ax;
    features[feature_ix++] = ay;
    features[feature_ix++] = az;
    features[feature_ix++] = gx;
    features[feature_ix++] = gy;
    features[feature_ix++] = gz;
  }

  sample_count++;

  if (sample_count >= 5) {
    sample_count = 0;

    float sum = 0.0;
    for (int i = 0; i < HIST_SIZE; i++) sum += a_hist[i];
    float mean = sum / (float)HIST_SIZE;
    float variance = 0.0;
    for (int i = 0; i < HIST_SIZE; i++) {
      float diff = a_hist[i] - mean;
      variance += diff * diff;
    }
    float std_dev = sqrt(variance / (float)HIST_SIZE);
    live_std_dev = std_dev;

    // Bộ lọc ngưỡng vật lý kết hợp TinyML
    bool physics_fall = (peak_acc > 26.0 || peak_gyro > 280.0);
    bool ai_assisted_fall = (ai_te_nga >= ai_threshold_cfg && peak_acc > 22.5 && peak_gyro > 140.0);

    if (physics_fall || ai_assisted_fall) {
      if (!is_fallen) {
        batCoiBuzzer();
        // BẮN NGAY TỨC THÌ CẢNH BÁO TÉ NGÃ LÊN PC Y TÁ & TRẠM GIƯỜNG QUA MQTT
        sendFallAlertToMQTT(true);
      }
    }

    String display_status = "";

    if (is_fallen) {
      current_stable_status = "TE NGA NGUY HIEM";
      display_status = "🚨 TE NGA 🚨";
    } else {
      tatCoiBuzzer();

      if (std_dev >= 0.42) {
        walk_score += 3;
        if (rest_score > 0) rest_score -= 3;
        if (walk_score >= 12) {
          current_stable_status = "DANG DI BO";
          walk_score = 12;
        }
      } else {
        rest_score += 2;
        if (walk_score > 0) walk_score -= 2;
        if (rest_score >= 14) {
          current_stable_status = "DUNG / NGOI YEN";
          rest_score = 14;
        }
      }
      display_status = current_stable_status;
    }

    // In nhịp tim hệ thống định kỳ ra Serial & đẩy MQTT ngay khi đổi trạng thái (Đứng/Ngồi <-> Đi bộ)
    bool status_changed = (display_status != last_status);
    if (status_changed || millis() - last_heartbeat_time >= 2000) {
      last_status = display_status;
      last_heartbeat_time = millis();

      Serial.printf("Trạng thái: [ %s ] | Gia tốc: %.1f m/s2 | Góc nghiêng: %.1f deg | AI Ngã: %.2f\n",
                    display_status.c_str(), total_a, live_tilt_angle, ai_te_nga);
    }

    // Đồng bộ về Web Y tá ngay lập tức khi đổi trạng thái vận động, hoặc định kỳ mỗi 0.8 giây
    if ((status_changed && !is_fallen) || millis() - last_mqtt_sync >= 800) {
      last_mqtt_sync = millis();
      if (fall_pending_sync && is_fallen) {
        sendFallAlertToMQTT(true);
      } else if (!is_fallen) {
        sendFallAlertToMQTT(false);
      }
    }

    peak_acc = 0.0;
    peak_gyro = 0.0;
    peak_lateral_acc = 0.0;
  }

  // Chạy suy luận AI TinyML khi đủ 50 mẫu (1 giây)
  if (feature_ix >= EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE) {
    signal_t signal;
    numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);

    if (r == EI_IMPULSE_OK) {
      for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        String lbl = String(result.classification[i].label);
        if (lbl == "te_nga") {
          ai_te_nga = result.classification[i].value;
        }
      }
    }
    feature_ix = 0;
  }

  capNhatCoiBuzzer();
  delay(20);
}

