/*
 * FIRMWARE NODE CẢM BIẾN CAMERA THỊ GIÁC (CAMERA VISION SENSOR)
 * - Thiết bị: ESP32-CAM (AI-Thinker ESP-32S + mạch nạp ESP32-CAM-MB) hỗ trợ tự nhận OV3660 / OV2640
 * - Chức năng: Phát luồng video MJPEG (/stream) & chụp ảnh tĩnh (/capture) qua HTTP LAN
 * - Tự cấu hình toàn bộ chân camera AI-Thinker và tự chỉnh hướng ảnh lật/độ sáng cho OV3660
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_http_server.h"

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#define WIFI_SSID_CFG     "YOUR_WIFI_SSID"
#define WIFI_PASS_CFG     "YOUR_WIFI_PASSWORD"
#define SERVER_IP_CFG     "172.20.10.11"
#endif

const char* ssid      = WIFI_SSID_CFG;
const char* password  = WIFI_PASS_CFG;
const char* server_ip = SERVER_IP_CFG;
const int   server_port = 3000;

// ================= CẤU HÌNH CHÂN CHO ESP32-CAM AI-THINKER =================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
bool camera_ok = false;
esp_err_t camera_err = ESP_OK;
const char* camera_sensor_name = "UNKNOWN";
unsigned long lastWifiRetry = 0;

static esp_err_t stream_handler(httpd_req_t *req) {
    if (!camera_ok) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t * _jpg_buf = NULL;
    char part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("❌ Lỗi lấy khung hình Camera!");
            res = ESP_FAIL;
        } else {
            _jpg_buf_len = fb->len;
            _jpg_buf = fb->buf;
        }

        if (res == ESP_OK) {
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }

        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        }
        if (res != ESP_OK) {
            break;
        }
        delay(25);
    }
    return res;
}

static esp_err_t capture_handler(httpd_req_t *req) {
    if (!camera_ok) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Xả bỏ 1 khung hình đệm cũ trong DMA trước khi chụp ảnh mới chuẩn sáng
    camera_fb_t * stale = esp_camera_fb_get();
    if (stale) esp_camera_fb_return(stale);

    camera_fb_t * fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("❌ Lỗi chụp ảnh từ Camera!");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    Serial.printf("📸 Đã chụp và gửi 1 khung hình JPEG (%u bytes) cho Web PC Y tá!\n", fb->len);
    return res;
}

void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_open_sockets = 6;
    config.send_wait_timeout = 3;
    config.recv_wait_timeout = 3;

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = stream_handler,
        .user_ctx  = NULL
    };

    httpd_uri_t capture_uri = {
        .uri       = "/capture",
        .method    = HTTP_GET,
        .handler   = capture_handler,
        .user_ctx  = NULL
    };

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        httpd_register_uri_handler(stream_httpd, &capture_uri);
        Serial.println("✅ Web Server Camera đã kích hoạt:");
        Serial.println("   • Video Stream: http://<IP_CAM>/stream");
        Serial.println("   • Chụp ảnh tĩnh (Snapshot): http://<IP_CAM>/capture");
    }
}

void registerCameraIpToServer() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    char errHex[16];
    snprintf(errHex, sizeof(errHex), "0x%x", (unsigned int)camera_err);
    String url = "http://" + String(server_ip) + ":" + String(server_port) +
                 "/api/camera/register?ip=" + WiFi.localIP().toString() +
                 "&ok=" + String(camera_ok ? "1" : "0") +
                 "&err=" + String(errHex) +
                 "&sensor=" + String(camera_sensor_name);
    http.setReuse(false);
    http.setTimeout(2000);
    if (http.begin(url)) {
        http.GET();
        http.end();
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== KHỞI ĐỘNG NODE CẢM BIẾN CAMERA ESP32-CAM ===");

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM;
    config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    // Tự động điều chỉnh bộ đệm theo PSRAM để không bị lỗi khởi tạo 0x105
    if (psramFound()) {
        config.frame_size = FRAMESIZE_VGA;
        config.jpeg_quality = 12;
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
        config.frame_size = FRAMESIZE_CIF;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
    }

    // Khởi tạo cảm biến Camera (nếu lỗi vẫn kết nối Wi-Fi để báo lỗi lên Server cho Kỹ thuật viên)
    camera_err = esp_camera_init(&config);
    if (camera_err != ESP_OK) {
        camera_ok = false;
        camera_sensor_name = "ERR";
        Serial.printf("❌ Khởi tạo Camera thất bại với mã lỗi: 0x%x\n", camera_err);
    } else {
        camera_ok = true;
        sensor_t *s = esp_camera_sensor_get();
        if (s && s->id.PID == OV3660_PID) {
            camera_sensor_name = "OV3660";
            s->set_vflip(s, 1);
            s->set_brightness(s, 1);
            s->set_saturation(s, -2);
        } else if (s && s->id.PID == OV2640_PID) {
            camera_sensor_name = "OV2640";
        } else {
            camera_sensor_name = "CAM";
        }
        Serial.printf("✅ Cảm biến camera %s đã sẵn sàng!\n", camera_sensor_name);
    }

    // Kết nối Wi-Fi (không treo vô hạn nếu Hotspot bật chậm)
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid, password);
    Serial.print("Đang kết nối Wi-Fi: ");
    Serial.println(ssid);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        Serial.print(".");
        retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✅ Wi-Fi Đã Kết Nối!");
        if (camera_ok && stream_httpd == NULL) {
            startCameraServer();
        }
        registerCameraIpToServer();
        Serial.println("\n------------------------------------------------------");
        Serial.print("📹 Luồng Camera sẵn sàng tại: http://");
        Serial.print(WiFi.localIP());
        Serial.println("/stream");
        Serial.println("------------------------------------------------------");
    } else {
        Serial.println("\n⚠️ Chưa kết nối được Wi-Fi, sẽ tự thử lại mỗi 10 giây.");
    }
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        if (millis() - lastWifiRetry >= 10000) {
            lastWifiRetry = millis();
            Serial.println("🔄 Đang kết nối lại Wi-Fi Hotspot...");
            WiFi.disconnect();
            WiFi.begin(ssid, password);
        }
    } else {
        if (camera_ok && stream_httpd == NULL) {
            startCameraServer();
        }
        registerCameraIpToServer();
    }
    delay(10000);
}
