/*
  ESP32-S3-CAM + OV5640  (v2)
  Funciona com ou sem PSRAM:
    - com PSRAM: buffers em PSRAM, ate 2592x1944 (QSXGA)
    - sem PSRAM: buffer na RAM interna, limitado a SVGA (800x600)
  Mostra diagnostico do chip no Serial (flash, PSRAM, sensor).

  Endpoints:
    http://IP/            pagina simples com stream e botoes
    http://IP/capture     uma foto JPEG
    http://IP/status      parametros atuais da camera (JSON)
    http://IP/control?var=NOME&val=VALOR   ajusta parametros
    http://IP:81/stream   stream MJPEG

  Ferramentas (Arduino IDE):
    Placa: ESP32S3 Dev Module
    Flash Mode: QIO 80MHz
    Flash Size: 16MB
    PSRAM: OPI PSRAM  (modulos N16R8/N8R8)  |  QSPI PSRAM (modulos ...R2)
    Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)
    USB DFU On Boot: Disabled
    Upload Mode: UART0 / Hardware CDC
    USB CDC On Boot: Disabled se usar a porta COM/UART, Enabled se usar a USB nativa
*/

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

#ifndef BOARD_HAS_PSRAM
#warning "PSRAM desabilitada na IDE (Ferramentas > PSRAM). A camera vai rodar limitada a SVGA."
#endif

// ===================== Wi-Fi =====================
const char *WIFI_SSID = "NOME_DA_REDE";
const char *WIFI_PASS = "SENHA";

// IP fixo opcional (1 = IP fixo, 0 = DHCP)
#define USE_STATIC_IP 0
IPAddress local_IP(192, 168, 1, 50);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns1(8, 8, 8, 8);

// ===================== Pinagem (ESP32-S3-EYE / Freenove) =====================
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM   4
#define SIOC_GPIO_NUM   5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM     8
#define Y3_GPIO_NUM     9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM  6
#define HREF_GPIO_NUM   7
#define PCLK_GPIO_NUM   13

// ===================== Estado global =====================
httpd_handle_t web_httpd = NULL;
httpd_handle_t stream_httpd = NULL;
bool hasPsram = false;
framesize_t maxFrameSize = FRAMESIZE_SVGA;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 OV5640</title>
<style>body{font-family:Arial;text-align:center;background:#222;color:#eee}
img{max-width:100%;margin-top:10px}button{font-size:16px;padding:8px 16px;margin:4px}</style>
</head><body>
<h3>ESP32-S3 + OV5640</h3>
<button onclick="window.open('/capture?t='+Date.now())">Capturar foto</button>
<button onclick="window.open('/status')">Status</button><br>
<img id="s">
<script>document.getElementById('s').src='http://'+location.hostname+':81/stream';</script>
</body></html>
)rawliteral";

// ===================== Handlers HTTP =====================
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t capture_handler(httpd_req_t *req) {
  // descarta um quadro antigo para pegar uma imagem atual
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb) esp_camera_fb_return(fb);

  fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t stream_handler(httpd_req_t *req) {
  char part_buf[64];
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Falha ao capturar quadro");
      res = ESP_FAIL;
      break;
    }
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
    res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
    if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;  // cliente desconectou
  }
  return res;
}

static esp_err_t control_handler(httpd_req_t *req) {
  char query[128], var[32], val[16];
  size_t len = httpd_req_get_url_query_len(req) + 1;

  if (len <= 1 || len > sizeof(query) ||
      httpd_req_get_url_query_str(req, query, len) != ESP_OK ||
      httpd_query_key_value(query, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(query, "val", val, sizeof(val)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  int v = atoi(val);
  sensor_t *s = esp_camera_sensor_get();
  int r = -1;

  if (!strcmp(var, "framesize")) {
    if (v > (int)maxFrameSize) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Resolucao acima do limite (sem PSRAM?)");
      return ESP_FAIL;
    }
    r = s->set_framesize(s, (framesize_t)v);
  }
  else if (!strcmp(var, "quality"))        r = s->set_quality(s, v);          // 4..63 (menor = melhor)
  else if (!strcmp(var, "brightness"))     r = s->set_brightness(s, v);       // -2..2
  else if (!strcmp(var, "contrast"))       r = s->set_contrast(s, v);         // -2..2
  else if (!strcmp(var, "saturation"))     r = s->set_saturation(s, v);       // -2..2
  else if (!strcmp(var, "awb"))            r = s->set_whitebal(s, v);         // balanco de branco auto 0/1
  else if (!strcmp(var, "awb_gain"))       r = s->set_awb_gain(s, v);         // 0/1
  else if (!strcmp(var, "wb_mode"))        r = s->set_wb_mode(s, v);          // 0 auto,1 sol,2 nublado,3 escritorio,4 casa
  else if (!strcmp(var, "aec"))            r = s->set_exposure_ctrl(s, v);    // exposicao auto 0/1
  else if (!strcmp(var, "aec_value"))      r = s->set_aec_value(s, v);        // exposicao manual
  else if (!strcmp(var, "ae_level"))       r = s->set_ae_level(s, v);         // -2..2
  else if (!strcmp(var, "agc"))            r = s->set_gain_ctrl(s, v);        // ganho auto 0/1
  else if (!strcmp(var, "agc_gain"))       r = s->set_agc_gain(s, v);         // ganho manual
  else if (!strcmp(var, "hmirror"))        r = s->set_hmirror(s, v);
  else if (!strcmp(var, "vflip"))          r = s->set_vflip(s, v);
  else if (!strcmp(var, "special_effect")) r = s->set_special_effect(s, v);

  if (r != 0) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t status_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  char json[600];
  snprintf(json, sizeof(json),
           "{\"pid\":\"0x%04X\",\"psram\":%s,\"max_framesize\":%d,"
           "\"framesize\":%u,\"quality\":%u,"
           "\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,"
           "\"awb\":%u,\"awb_gain\":%u,\"wb_mode\":%u,"
           "\"aec\":%u,\"aec_value\":%u,\"ae_level\":%d,"
           "\"agc\":%u,\"agc_gain\":%u,\"hmirror\":%u,\"vflip\":%u,"
           "\"rssi\":%d}",
           s->id.PID, hasPsram ? "true" : "false", (int)maxFrameSize,
           s->status.framesize, s->status.quality,
           s->status.brightness, s->status.contrast, s->status.saturation,
           s->status.awb, s->status.awb_gain, s->status.wb_mode,
           s->status.aec, s->status.aec_value, s->status.ae_level,
           s->status.agc, s->status.agc_gain, s->status.hmirror, s->status.vflip,
           WiFi.RSSI());
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, strlen(json));
}

void startServers() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri   = {"/",        HTTP_GET, index_handler,   NULL};
  httpd_uri_t capture_uri = {"/capture", HTTP_GET, capture_handler, NULL};
  httpd_uri_t control_uri = {"/control", HTTP_GET, control_handler, NULL};
  httpd_uri_t status_uri  = {"/status",  HTTP_GET, status_handler,  NULL};

  if (httpd_start(&web_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(web_httpd, &index_uri);
    httpd_register_uri_handler(web_httpd, &capture_uri);
    httpd_register_uri_handler(web_httpd, &control_uri);
    httpd_register_uri_handler(web_httpd, &status_uri);
  }

  // stream em servidor separado (porta 81) para nao travar os outros endpoints
  config.server_port = 81;
  config.ctrl_port += 1;
  httpd_uri_t stream_uri = {"/stream", HTTP_GET, stream_handler, NULL};
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

// ===================== Diagnostico =====================
void printChipInfo() {
  Serial.println("------ Diagnostico ------");
  Serial.printf("Chip: %s rev %d, %d nucleos, %d MHz\n",
                ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), ESP.getCpuFreqMHz());
  Serial.printf("Flash: %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
#ifdef BOARD_HAS_PSRAM
  Serial.println("PSRAM na IDE: habilitada");
#else
  Serial.println("PSRAM na IDE: DESABILITADA (Ferramentas > PSRAM)");
#endif
  Serial.printf("PSRAM detectada: %s, %u KB\n",
                psramFound() ? "sim" : "nao", ESP.getPsramSize() / 1024);
  Serial.printf("Heap interno livre: %u KB\n", ESP.getFreeHeap() / 1024);
  Serial.println("-------------------------");
}

// ===================== Camera =====================
bool initCamera() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk  = XCLK_GPIO_NUM;
  config.pin_pclk  = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href  = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn  = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  hasPsram = psramFound();

  if (hasPsram) {
    // aloca buffers para a resolucao maxima da OV5640 (2592x1944)
    config.frame_size   = FRAMESIZE_QSXGA;
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    maxFrameSize        = FRAMESIZE_QSXGA;
  } else {
    Serial.println("AVISO: sem PSRAM, usando RAM interna (max SVGA 800x600).");
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    maxFrameSize        = FRAMESIZE_SVGA;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Falha ao iniciar camera: 0x%x\n", err);
    Serial.println("Verifique o cabo flat da camera e a pinagem.");
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("Sensor PID: 0x%04X (OV5640 = 0x5640)\n", s->id.PID);

  // resolucao inicial: XGA 1024x768 quando ha PSRAM
  // ajuste via /control?var=framesize&val=N
  if (hasPsram) {
    s->set_framesize(s, FRAMESIZE_XGA);
    Serial.println("Resolucao inicial: XGA 1024x768");
  }
  return true;
}

// ===================== Wi-Fi =====================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
#if USE_STATIC_IP
  if (!WiFi.config(local_IP, gateway, subnet, dns1)) {
    Serial.println("Falha ao configurar IP fixo");
  }
#endif
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);  // melhora a estabilidade do stream

  Serial.print("Conectando ao Wi-Fi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 20000) {
      Serial.println("\nTimeout no Wi-Fi (SSID/senha? rede 2,4 GHz?). Reiniciando...");
      ESP.restart();
    }
  }
  Serial.printf("\nConectado! RSSI: %d dBm\n", WiFi.RSSI());
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nESP32-S3 + OV5640 (v2)");
  printChipInfo();

  if (!initCamera()) {
    delay(5000);
    ESP.restart();
  }
  connectWiFi();
  startServers();

  String ip = WiFi.localIP().toString();
  Serial.printf("Pagina:  http://%s/\n", ip.c_str());
  Serial.printf("Captura: http://%s/capture\n", ip.c_str());
  Serial.printf("Stream:  http://%s:81/stream\n", ip.c_str());
}

void loop() {
  // reconecta se o Wi-Fi cair
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi caiu, reconectando...");
    WiFi.reconnect();
    delay(5000);
  }
  delay(1000);
}
