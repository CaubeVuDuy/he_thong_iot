#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

/*
  ============================================================
  THIẾT BỊ GIÁM SÁT HÀNH VI VÀ CẢNH BÁO TÉ NGÃ - ESP32 & MPU6050
  Code: Thu thập dữ liệu 6 trục quán tính (50Hz) nạp lên Edge Impulse
  ============================================================
  Sơ đồ cắm dây:
    - MPU6050 VCC -> ESP32 3V3 (hoặc VIN/5V)
    - MPU6050 GND -> ESP32 GND
    - MPU6050 SCL -> ESP32 GPIO 21 (D21)
    - MPU6050 SDA -> ESP32 GPIO 22 (D22)
  ============================================================
*/

Adafruit_MPU6050 mpu;

#define SAMPLING_FREQ_HZ    50
#define INTERVAL_MS         (1000 / SAMPLING_FREQ_HZ)

unsigned long lastSampleTime = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  // Cấu hình chân I2C: D22 làm SDA, D21 làm SCL (mạch chuẩn không chéo dây)
  Wire.begin(22, 21);

  Serial.println("\n--- DANG KHOI TAO MPU6050 ---");
  if (!mpu.begin(0x68, &Wire)) {
    Serial.println("Loi: Khong tim thay MPU6050! Kiem tra lai 4 day noi.");
    while (1) {
      delay(1000);
    }
  }

  Serial.println("MPU6050 da san sang!");

  // Cấu hình thông số cảm biến cho bài toán nhận diện té ngã
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);   // Dải đo ±8g (chống tràn số khi va đập té ngã)
  mpu.setGyroRange(MPU6050_RANGE_1000_DEG);       // Dải đo ±1000 deg/s
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);     // Lọc thông thấp 21Hz loại nhiễu tần số cao

  delay(500);
}

void loop() {
  unsigned long now = millis();

  // Đảm bảo đúng tần số lấy mẫu 50Hz (mỗi 20ms in ra 1 hàng dữ liệu)
  if (now - lastSampleTime >= INTERVAL_MS) {
    lastSampleTime = now;

    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    // In 6 kênh dữ liệu: ax, ay, az (m/s^2), gx, gy, gz (rad/s)
    Serial.printf("%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                  a.acceleration.x, a.acceleration.y, a.acceleration.z,
                  g.gyro.x, g.gyro.y, g.gyro.z);
  }
}
