/*
  Code đã được sửa để ESP32 gửi dữ liệu MQTT về Raspberry Pi local.
  - MQTT broker: Mosquitto trên Raspberry Pi
  - Port: 1883
  - Không dùng HiveMQ Cloud
  - Không dùng SSL, username, password
  Lưu ý: đổi mqtt_server = "192.168.1.50" thành IP thật của Raspberry Pi.
*/

#include <WiFi.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <esp_sleep.h>

//==================================================
// 1. WIFI
//==================================================
const char* ssid = "Hi";
const char* password = "88888888";

//==================================================
// 2. MQTT LOCAL RASPBERRY PI
//==================================================
// Đổi IP này thành IP thật của Raspberry Pi
const char* mqtt_server = "172.20.10.2";
const int   mqtt_port   = 1883;

//==================================================
// 3. THÔNG TIN NODE
//==================================================
const char* nodeName   = "bep";
const char* clientHead = "ESP32_BEP_";
const char* baseTopic  = "nha/bep";

//==================================================
// 4. KHAI BÁO CHÂN
//==================================================
#define DHTPIN       4
#define DHTTYPE      DHT22

#define FLAME_PIN    34
#define MQ2_PIN      35
#define MP2_PIN      32

#define BUZZER_PIN   27

#define LED_XANH_1   5
#define LED_XANH_2   18
#define LED_VANG_1   19
#define LED_VANG_2   21
#define LED_DO_1     22
#define LED_DO_2     23

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);

//==================================================
// 5. DEEP SLEEP
//==================================================
const bool BAT_DEEP_SLEEP = true;
const bool CHI_NGU_KHI_AN_TOAN = true;
const uint64_t DEEP_SLEEP_TIME_US = 1000000ULL;  // 1 giây

RTC_DATA_ATTR int soLanKhoiDong = 0;

//==================================================
// 6. NGƯỠNG CẢNH BÁO
//==================================================
const float TEMP_WARN   = 50.0;
const float TEMP_DANGER = 70.0;

const int MQ2_WARN      = 1200;
const int MQ2_DANGER    = 2500;

const int MP2_WARN      = 1000;
const int MP2_DANGER    = 2000;

const int FLAME_WARN    = 800;
const int FLAME_DANGER  = 2100;

//==================================================
// 7. LỌC NHIỄU ANALOG
//==================================================
const int SAMPLE_COUNT = 10;

const float ALPHA_FLAME = 0.20;
const float ALPHA_GAS   = 0.20;
const float ALPHA_SMOKE = 0.20;

bool filterInitialized = false;

float flameLoc = 0;
float gasLoc   = 0;
float khoiLoc  = 0;

//==================================================
// 8. CHỐNG CẢNH BÁO GIẢ BẰNG TRUNG BÌNH TRƯỢT
//==================================================
const int AVG_WINDOW = 4;

float nhietdoBuffer[AVG_WINDOW];
int flameBuffer[AVG_WINDOW];
int gasBuffer[AVG_WINDOW];
int khoiBuffer[AVG_WINDOW];

int avgIndex = 0;
int avgCount = 0;

float nhietdoTB = NAN;
int flameTB = 0;
int gasTB   = 0;
int khoiTB  = 0;

//==================================================
// 9. BIẾN CẢM BIẾN
//==================================================
float nhietdo = NAN;
float doam    = NAN;

int flame = 0;
int gas   = 0;
int khoi  = 0;

int mucNhietDo = 0;
int mucLua     = 0;
int mucGas     = 0;
int mucKhoi    = 0;

int mucTam     = 0;
int mucCanhBao = 0;

//==================================================
// 10. CHỐNG NHẢY MỨC CẢNH BÁO
//==================================================
const int SO_LAN_TANG_MUC = 2;
const int SO_LAN_GIAM_MUC = 6;

int mucUngVien = -1;
int demUngVien = 0;

//==================================================
// 11. BIẾN THỜI GIAN
//==================================================
unsigned long lastSensorRead = 0;
unsigned long lastMsg = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastBuzzerToggle = 0;

bool buzzerState = false;
bool wifiWasConnected = false;

//==================================================
int docTrungBinhBo2Dau(int pin, int soMau) {
  if (soMau < 3) soMau = 3;
  if (soMau > 30) soMau = 30;

  int values[30];

  for (int i = 0; i < soMau; i++) {
    values[i] = analogRead(pin);
    delay(3);
  }

  for (int i = 0; i < soMau - 1; i++) {
    for (int j = i + 1; j < soMau; j++) {
      if (values[i] > values[j]) {
        int temp = values[i];
        values[i] = values[j];
        values[j] = temp;
      }
    }
  }

  long tong = 0;
  for (int i = 1; i < soMau - 1; i++) {
    tong += values[i];
  }

  return tong / (soMau - 2);
}

//==================================================
float locEMA(float giaTriMoi, float giaTriCu, float alpha) {
  return alpha * giaTriMoi + (1.0 - alpha) * giaTriCu;
}

//==================================================
float tinhTrungBinhFloat(float values[], int count) {
  if (count <= 0) return NAN;

  float tong = 0;
  int demHopLe = 0;

  for (int i = 0; i < count; i++) {
    if (!isnan(values[i])) {
      tong += values[i];
      demHopLe++;
    }
  }

  if (demHopLe == 0) return NAN;
  return tong / demHopLe;
}

//==================================================
int tinhTrungBinhInt(int values[], int count) {
  if (count <= 0) return 0;

  long tong = 0;
  for (int i = 0; i < count; i++) {
    tong += values[i];
  }

  return tong / count;
}

//==================================================
void capNhatBoDemTrungBinh(float nhietdoMoi, int flameMoi, int gasMoi, int khoiMoi) {
  nhietdoBuffer[avgIndex] = nhietdoMoi;
  flameBuffer[avgIndex]  = flameMoi;
  gasBuffer[avgIndex]    = gasMoi;
  khoiBuffer[avgIndex]   = khoiMoi;

  avgIndex++;
  if (avgIndex >= AVG_WINDOW) avgIndex = 0;

  if (avgCount < AVG_WINDOW) avgCount++;

  nhietdoTB = tinhTrungBinhFloat(nhietdoBuffer, avgCount);
  flameTB   = tinhTrungBinhInt(flameBuffer, avgCount);
  gasTB     = tinhTrungBinhInt(gasBuffer, avgCount);
  khoiTB    = tinhTrungBinhInt(khoiBuffer, avgCount);
}

//==================================================
int tinhMucNhietDo(float value) {
  if (isnan(value)) return 0;
  if (value >= TEMP_DANGER) return 2;
  if (value >= TEMP_WARN)   return 1;
  return 0;
}

//==================================================
int tinhMucAnalog(int value, int nguongCanhBao, int nguongNguyHiem) {
  if (value >= nguongNguyHiem) return 2;
  if (value >= nguongCanhBao)  return 1;
  return 0;
}

//==================================================
int layMucCaoNhat() {
  int maxLevel = 0;

  if (mucNhietDo > maxLevel) maxLevel = mucNhietDo;
  if (mucLua     > maxLevel) maxLevel = mucLua;
  if (mucGas     > maxLevel) maxLevel = mucGas;
  if (mucKhoi    > maxLevel) maxLevel = mucKhoi;

  return maxLevel;
}

//==================================================
void capNhatMucCanhBaoOnDinh(int mucMoi) {
  if (mucMoi == mucCanhBao) {
    mucUngVien = -1;
    demUngVien = 0;
    return;
  }

  if (mucMoi != mucUngVien) {
    mucUngVien = mucMoi;
    demUngVien = 1;
  } else {
    demUngVien++;
  }

  if (mucMoi > mucCanhBao && demUngVien >= SO_LAN_TANG_MUC) {
    mucCanhBao = mucMoi;
    mucUngVien = -1;
    demUngVien = 0;
  }

  if (mucMoi < mucCanhBao && demUngVien >= SO_LAN_GIAM_MUC) {
    mucCanhBao = mucMoi;
    mucUngVien = -1;
    demUngVien = 0;
  }
}

//==================================================
void docCamBien() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t)) nhietdo = t;
  if (!isnan(h)) doam = h;

  int flameRaw = docTrungBinhBo2Dau(FLAME_PIN, SAMPLE_COUNT);
  int gasRaw   = docTrungBinhBo2Dau(MQ2_PIN, SAMPLE_COUNT);
  int khoiRaw  = docTrungBinhBo2Dau(MP2_PIN, SAMPLE_COUNT);

  int flameMoi = 4095 - flameRaw;
  flameMoi = constrain(flameMoi, 0, 4095);

  int gasMoi  = constrain(gasRaw, 0, 4095);
  int khoiMoi = constrain(khoiRaw, 0, 4095);

  if (!filterInitialized) {
    flameLoc = flameMoi;
    gasLoc   = gasMoi;
    khoiLoc  = khoiMoi;
    filterInitialized = true;
  } else {
    flameLoc = locEMA(flameMoi, flameLoc, ALPHA_FLAME);
    gasLoc   = locEMA(gasMoi, gasLoc, ALPHA_GAS);
    khoiLoc  = locEMA(khoiMoi, khoiLoc, ALPHA_SMOKE);
  }

  flame = (int)flameLoc;
  gas   = (int)gasLoc;
  khoi  = (int)khoiLoc;

  capNhatBoDemTrungBinh(nhietdo, flame, gas, khoi);

  if (avgCount < AVG_WINDOW) {
    mucTam = 0;
    capNhatMucCanhBaoOnDinh(mucTam);
    return;
  }

  mucNhietDo = tinhMucNhietDo(nhietdoTB);
  mucLua     = tinhMucAnalog(flameTB, FLAME_WARN, FLAME_DANGER);
  mucGas     = tinhMucAnalog(gasTB, MQ2_WARN, MQ2_DANGER);
  mucKhoi    = tinhMucAnalog(khoiTB, MP2_WARN, MP2_DANGER);

  mucTam = layMucCaoNhat();
  capNhatMucCanhBaoOnDinh(mucTam);
}

//==================================================
void tatTatCaLed() {
  digitalWrite(LED_XANH_1, LOW);
  digitalWrite(LED_XANH_2, LOW);
  digitalWrite(LED_VANG_1, LOW);
  digitalWrite(LED_VANG_2, LOW);
  digitalWrite(LED_DO_1, LOW);
  digitalWrite(LED_DO_2, LOW);
}

//==================================================
void capNhatLed(int level) {
  tatTatCaLed();

  if (level == 0) {
    // Nếu bật deep sleep thì không bật LED xanh để tránh nháy theo chu kỳ ngủ/thức
    if (!BAT_DEEP_SLEEP) {
      digitalWrite(LED_XANH_1, HIGH);
      digitalWrite(LED_XANH_2, HIGH);
    }
  }
  else if (level == 1) {
    digitalWrite(LED_VANG_1, HIGH);
    digitalWrite(LED_VANG_2, HIGH);
  }
  else {
    digitalWrite(LED_DO_1, HIGH);
    digitalWrite(LED_DO_2, HIGH);
  }
}

//==================================================
void capNhatCoi(int level) {
  unsigned long now = millis();

  if (level == 0) {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerState = false;
    lastBuzzerToggle = now;
  }
  else if (level == 1) {
    if (now - lastBuzzerToggle >= 500) {
      lastBuzzerToggle = now;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  }
  else {
    if (now - lastBuzzerToggle >= 150) {
      lastBuzzerToggle = now;
      buzzerState = !buzzerState;
      digitalWrite(BUZZER_PIN, buzzerState ? HIGH : LOW);
    }
  }
}

//==================================================
const char* layTrangThaiCanhBao(int level) {
  switch (level) {
    case 0: return "binh_thuong";
    case 1: return "canh_bao";
    case 2: return "nguy_hiem";
    default: return "khong_xac_dinh";
  }
}

//==================================================
String taoChuoiNguonCanhBao() {
  String nguon = "";

  if (mucNhietDo > 0) {
    nguon += "nhietdo:";
    nguon += layTrangThaiCanhBao(mucNhietDo);
    nguon += ";";
  }

  if (mucLua > 0) {
    nguon += "lua:";
    nguon += layTrangThaiCanhBao(mucLua);
    nguon += ";";
  }

  if (mucGas > 0) {
    nguon += "gas:";
    nguon += layTrangThaiCanhBao(mucGas);
    nguon += ";";
  }

  if (mucKhoi > 0) {
    nguon += "khoi:";
    nguon += layTrangThaiCanhBao(mucKhoi);
    nguon += ";";
  }

  if (nguon == "") nguon = "khong_co";
  return nguon;
}

//==================================================
// KHỞI ĐỘNG WIFI KHÔNG CHẶN CHƯƠNG TRÌNH
// Cảm biến, LED và còi vẫn chạy kể cả khi chưa có WiFi.
//==================================================
void startWiFi() {
  Serial.println("Bat dau ket noi WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.persistent(false);
  WiFi.begin(ssid, password);
}

//==================================================
void checkWiFi() {
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.println("WiFi da ket noi");
      Serial.print("IP ESP32: ");
      Serial.println(WiFi.localIP());
      Serial.print("Gateway : ");
      Serial.println(WiFi.gatewayIP());
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("WiFi bi mat ket noi - canh bao tai cho van hoat dong");
  }

  if (now - lastWiFiRetry >= 10000) {
    lastWiFiRetry = now;
    Serial.println("Dang thu ket noi lai WiFi, khong dung chuong trinh...");
    WiFi.disconnect(true, false);
    delay(50);
    WiFi.begin(ssid, password);
  }
}

//==================================================
void choWiFiKetNoiTrongSetup() {
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 5000) {
    // Trong lúc chờ WiFi ở setup vẫn đọc cảm biến và cập nhật cảnh báo tại chỗ
    docCamBien();
    capNhatLed(mucCanhBao);
    capNhatCoi(mucCanhBao);
    delay(100);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    Serial.println("WiFi da ket noi trong setup");
    Serial.print("IP ESP32: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi chua ket noi duoc trong setup - tiep tuc chay canh bao tai cho");
  }
}

//==================================================
bool reconnectMQTT() {
  if (client.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  String clientId = String(clientHead) + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);
  String topicTrangThai = String(baseTopic) + "/trangthai";

  Serial.println("Dang ket noi Mosquitto local tren Raspberry Pi...");
  Serial.print("Client ID: ");
  Serial.println(clientId);
  Serial.print("MQTT server: ");
  Serial.println(mqtt_server);
  Serial.print("MQTT port: ");
  Serial.println(mqtt_port);

  bool ok = client.connect(
    clientId.c_str(),
    topicTrangThai.c_str(),
    1,
    true,
    "offline"
  );

  if (ok) {
    Serial.println("Ket noi Raspberry Pi MQTT thanh cong");
    client.publish(topicTrangThai.c_str(), "online", true);
    client.publish((String(baseTopic) + "/chedo").c_str(), "awake", true);
  } else {
    Serial.print("Ket noi Raspberry Pi MQTT that bai, state = ");
    Serial.println(client.state());
  }

  return ok;
}

//==================================================
void guiDuLieu() {
  if (!client.connected()) return;
  if (isnan(nhietdoTB) || isnan(doam)) return;

  const char* trangThaiTong = layTrangThaiCanhBao(mucCanhBao);
  String nguonCanhBao = taoChuoiNguonCanhBao();

  char nhietdoStr[16];
  char doamStr[16];
  char flameStr[16];
  char gasStr[16];
  char khoiStr[16];

  char mucTongStr[8];
  char mucNhietDoStr[8];
  char mucLuaStr[8];
  char mucGasStr[8];
  char mucKhoiStr[8];

  dtostrf(nhietdoTB, 1, 2, nhietdoStr);
  dtostrf(doam, 1, 2, doamStr);

  sprintf(flameStr, "%d", flameTB);
  sprintf(gasStr, "%d", gasTB);
  sprintf(khoiStr, "%d", khoiTB);

  sprintf(mucTongStr, "%d", mucCanhBao);
  sprintf(mucNhietDoStr, "%d", mucNhietDo);
  sprintf(mucLuaStr, "%d", mucLua);
  sprintf(mucGasStr, "%d", mucGas);
  sprintf(mucKhoiStr, "%d", mucKhoi);

  client.publish((String(baseTopic) + "/nhietdo").c_str(), nhietdoStr, true);
  client.publish((String(baseTopic) + "/doam").c_str(), doamStr, true);
  client.publish((String(baseTopic) + "/lua").c_str(), flameStr, true);
  client.publish((String(baseTopic) + "/gas").c_str(), gasStr, true);
  client.publish((String(baseTopic) + "/khoi").c_str(), khoiStr, true);

  client.publish((String(baseTopic) + "/canhbao_muc").c_str(), mucTongStr, true);
  client.publish((String(baseTopic) + "/canhbao_trangthai").c_str(), trangThaiTong, true);
  client.publish((String(baseTopic) + "/canhbao_nguon").c_str(), nguonCanhBao.c_str(), true);

  client.publish((String(baseTopic) + "/muc_nhietdo").c_str(), mucNhietDoStr, true);
  client.publish((String(baseTopic) + "/muc_lua").c_str(), mucLuaStr, true);
  client.publish((String(baseTopic) + "/muc_gas").c_str(), mucGasStr, true);
  client.publish((String(baseTopic) + "/muc_khoi").c_str(), mucKhoiStr, true);

  Serial.println("===== NODE BEP =====");
  Serial.print("Node               : "); Serial.println(nodeName);
  Serial.print("Nhiet do TB        : "); Serial.println(nhietdoStr);
  Serial.print("Do am              : "); Serial.println(doamStr);
  Serial.print("Lua TB             : "); Serial.println(flameStr);
  Serial.print("Gas TB             : "); Serial.println(gasStr);
  Serial.print("Khoi TB            : "); Serial.println(khoiStr);
  Serial.print("Muc on dinh        : "); Serial.println(mucCanhBao);
  Serial.print("Trang thai tong    : "); Serial.println(trangThaiTong);
  Serial.print("Nguon canh bao     : "); Serial.println(nguonCanhBao);
  Serial.println();
}

//==================================================
void inLyDoThucDay() {
  esp_sleep_wakeup_cause_t lyDo = esp_sleep_get_wakeup_cause();

  if (lyDo == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("Thuc day do bo hen gio Deep Sleep");
  } else if (lyDo == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.println("Khoi dong lan dau hoac reset binh thuong");
  } else {
    Serial.print("Thuc day do nguyen nhan khac, ma = ");
    Serial.println((int)lyDo);
  }
}

//==================================================
void vaoDeepSleep() {
  Serial.println("Chuan bi vao Deep Sleep...");

  digitalWrite(BUZZER_PIN, LOW);
  buzzerState = false;
  tatTatCaLed();

  if (client.connected()) {
    client.publish((String(baseTopic) + "/chedo").c_str(), "deep_sleep", true);
    client.loop();
    delay(200);
    client.disconnect();
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup(DEEP_SLEEP_TIME_US);

  Serial.println("ESP32 dang ngu...");
  Serial.flush();

  esp_deep_sleep_start();
}

//==================================================
void xuLyDeepSleepSauKhiGui() {
  if (!BAT_DEEP_SLEEP) return;

  if (CHI_NGU_KHI_AN_TOAN && mucCanhBao != 0) {
    Serial.println("Dang co canh bao, ESP32 khong vao Deep Sleep");
    return;
  }

  Serial.println("He thong an toan, cho phep vao Deep Sleep");
  vaoDeepSleep();
}

//==================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  soLanKhoiDong++;

  Serial.println();
  Serial.println("=================================");
  Serial.println("ESP32 NODE BEP KHOI DONG");
  Serial.print("So lan khoi dong: ");
  Serial.println(soLanKhoiDong);
  inLyDoThucDay();
  Serial.println("=================================");

  dht.begin();

  analogReadResolution(12);
  analogSetPinAttenuation(FLAME_PIN, ADC_11db);
  analogSetPinAttenuation(MQ2_PIN, ADC_11db);
  analogSetPinAttenuation(MP2_PIN, ADC_11db);

  pinMode(FLAME_PIN, INPUT);
  pinMode(MQ2_PIN, INPUT);
  pinMode(MP2_PIN, INPUT);

  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(LED_XANH_1, OUTPUT);
  pinMode(LED_XANH_2, OUTPUT);
  pinMode(LED_VANG_1, OUTPUT);
  pinMode(LED_VANG_2, OUTPUT);
  pinMode(LED_DO_1, OUTPUT);
  pinMode(LED_DO_2, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  tatTatCaLed();

  client.setServer(mqtt_server, mqtt_port);
  client.setKeepAlive(30);
  client.setSocketTimeout(10);
  client.setBufferSize(512);

  randomSeed(micros());
  delay(random(500, 2500));

  for (int i = 0; i < 5; i++) {
    docCamBien();
    delay(100);
  }

  capNhatLed(mucCanhBao);

  startWiFi();
  choWiFiKetNoiTrongSetup();

  if (WiFi.status() == WL_CONNECTED) {
    reconnectMQTT();
    client.loop();
  }

  if (client.connected()) {
    guiDuLieu();
    client.loop();
    delay(200);
  }

  xuLyDeepSleepSauKhiGui();

  lastMsg = millis();
  lastSensorRead = millis();
}

//==================================================
void loop() {
  // 1. Ưu tiên cao nhất: luôn đọc cảm biến và cảnh báo tại chỗ
  // Phần này chạy độc lập, không phụ thuộc WiFi/MQTT/Pi.
  if (millis() - lastSensorRead >= 500) {
    lastSensorRead = millis();
    docCamBien();
    capNhatLed(mucCanhBao);
  }

  capNhatCoi(mucCanhBao);

  // 2. WiFi chỉ là phần truyền dữ liệu, không được làm dừng cảnh báo tại chỗ
  checkWiFi();

  // 3. Chỉ kết nối MQTT khi đã có WiFi
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      unsigned long now = millis();

      if (now - lastReconnectAttempt >= 3000) {
        lastReconnectAttempt = now;
        reconnectMQTT();
      }
    }

    if (client.connected()) {
      client.loop();
    }
  }

  // 4. Gửi dữ liệu nếu MQTT đã kết nối; nếu mất WiFi thì bỏ qua, cảnh báo tại chỗ vẫn chạy
  if (client.connected() && millis() - lastMsg >= 3000) {
    lastMsg = millis();
    guiDuLieu();
    xuLyDeepSleepSauKhiGui();
  }

  // 5. Nếu đang nguy hiểm thì tuyệt đối không ngủ
  if (mucCanhBao != 0) {
    // Giữ ESP32 thức để LED/còi cảnh báo liên tục
  }

  delay(10);
}
