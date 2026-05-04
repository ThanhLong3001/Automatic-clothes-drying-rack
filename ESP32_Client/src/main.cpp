#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <DHT_U.h>
#include <AccelStepper.h>
#include <math.h>  // dùng M_PI

// WiFi credentials
const char *ssid = "Nghia 89";
const char *password = "nghia89123@";

// MQTT broker settings
const char *mqtt_server = "192.168.2.159"; // Địa chỉ IP của MQTT broker
const int   mqtt_port   = 1883;            // Cổng mặc định của MQTT
const char *mqtt_client_id = "ESP32_ClothesRack";
const char *mqtt_user = "";                // Nếu broker yêu cầu user/pass, điền vào đây
const char *mqtt_pass = "";

// MQTT topics
const char *topic_data    = "clothesrack/data";       // publish dữ liệu cảm biến
const char *topic_mode    = "clothesrack/mode";       // subscribe chế độ
const char *topic_command = "clothesrack/command";    // subscribe lệnh
const char *topic_config  = "clothesrack/config";     // NEW: subscribe cấu hình (line_mm, drum_mm, steps)

// DHT11
#define DHTPIN  2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Stepper motor 28BYJ-48 + ULN2003
#define MOTOR_PIN1 17
#define MOTOR_PIN2 16
#define MOTOR_PIN3 4
#define MOTOR_PIN4 0
AccelStepper stepper(AccelStepper::FULL4WIRE, MOTOR_PIN1, MOTOR_PIN3, MOTOR_PIN2, MOTOR_PIN4);
const long stepsPerRevolution = 2048; // 28BYJ-48
bool   motorRunning  = false;         // Trạng thái động cơ
String systemState   = "closed";      // "open"/"closed"
String mode          = "auto";        // "auto"/"manual"

// Rain sensor
#define RAIN_SENSOR_PIN 5
bool rainDetected = false;

// ---- CONFIG cho tính vòng quay theo chiều dài dây ----
#define DRUM_DIAMETER_MM_DEFAULT 40.0  // đường kính tang cuốn (mm) -> đo thực tế
float line_length_mm   = 3000.0;       // chiều dài dây phơi mặc định (mm) ~ 3m
float drum_diameter_mm = DRUM_DIAMETER_MM_DEFAULT;
long  travelSteps      = stepsPerRevolution; // số step chạy khi open/close (tính từ chiều dài)

// WiFi and MQTT clients
WiFiClient espClient;
PubSubClient client(espClient);

long computeTravelSteps() {
  // số vòng = chiều dài / chu vi tang
  double rev = (double)line_length_mm / (M_PI * (double)drum_diameter_mm);
  long steps = lround(rev * (double)stepsPerRevolution);
  // an toàn: tối thiểu 1 vòng
  if (steps < stepsPerRevolution) steps = stepsPerRevolution;
  return steps;
}

void mqttCallback(char *topic, byte *payload, unsigned int length);

void setup() {
  Serial.begin(9600);
  dht.begin();
  pinMode(RAIN_SENSOR_PIN, INPUT);

  // Kết nối WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(100);
  }
  Serial.println("\nWiFi connected!");

  // Cấu hình MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  // Cấu hình động cơ
  stepper.setMaxSpeed(1000);
  stepper.setAcceleration(500);

  // Tính sẵn travelSteps từ cấu hình mặc định
  travelSteps = computeTravelSteps();
  Serial.printf("Init config: line=%.1f mm, drum=%.1f mm -> steps=%ld\n",
                line_length_mm, drum_diameter_mm, travelSteps);
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("Message received on [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  // ---- Xử lý cấu hình (NEW) ----
  if (String(topic) == topic_config) {
    // Hỗ trợ JSON đơn giản: {"line_mm": 3200, "drum_mm": 22} hoặc gửi riêng từng cái
    // (tuỳ chọn) {"steps": 4096} để ép số bước trực tiếp
    float newLine = NAN, newDrum = NAN;
    long  newSteps = -1;

    // parse rất nhẹ kiểu tìm chuỗi (tránh thêm thư viện JSON)
    int p;

    p = message.indexOf("\"line_mm\"");
    if (p >= 0) {
      int c = message.indexOf(":", p);
      if (c > 0) newLine = message.substring(c+1).toFloat();
    }

    p = message.indexOf("\"drum_mm\"");
    if (p >= 0) {
      int c = message.indexOf(":", p);
      if (c > 0) newDrum = message.substring(c+1).toFloat();
    }

    p = message.indexOf("\"steps\"");
    if (p >= 0) {
      int c = message.indexOf(":", p);
      if (c > 0) newSteps = message.substring(c+1).toInt();
    }

    bool updated = false;
    if (!isnan(newLine) && newLine > 100) { // >10 cm
      line_length_mm = newLine;
      updated = true;
    }
    if (!isnan(newDrum) && newDrum > 5) {   // >5 mm
      drum_diameter_mm = newDrum;
      updated = true;
    }

    if (newSteps > 0) {
      travelSteps = newSteps;
      updated = true;
    } else if (updated) {
      travelSteps = computeTravelSteps();
    }

    if (updated) {
      Serial.printf("Config updated: line=%.1f mm, drum=%.1f mm, steps=%ld\n",
                    line_length_mm, drum_diameter_mm, travelSteps);
    } else {
      Serial.println("Config message ignored (no valid fields).");
    }
  }

  // ---- Xử lý chế độ ----
  if (String(topic) == topic_mode) {
    if (message.indexOf("manual") >= 0) mode = "manual";
    else if (message.indexOf("auto") >= 0) mode = "auto";
    Serial.print("Mode set to: "); Serial.println(mode);
  }

  // ---- Xử lý lệnh thủ công ----
  if (String(topic) == topic_command && mode == "manual" && !motorRunning) {
    if (message.indexOf("open") >= 0) {
      Serial.println("Thủ công -> Kéo ra");
      motorRunning = true;
      stepper.moveTo(stepper.currentPosition() + travelSteps);
      while (stepper.distanceToGo() != 0) stepper.run();
      motorRunning = false;
      systemState = "open";
    } else if (message.indexOf("close") >= 0) {
      Serial.println("Thủ công -> Thu vào");
      motorRunning = true;
      stepper.moveTo(stepper.currentPosition() - travelSteps);
      while (stepper.distanceToGo() != 0) stepper.run();
      motorRunning = false;
      systemState = "closed";
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect(mqtt_client_id, mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      // Subscribe các topic
      client.subscribe(topic_mode);
      client.subscribe(topic_command);
      client.subscribe(topic_config); // NEW
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  // Đọc cảm biến DHT11
  float temp = dht.readTemperature();
  float hum  = dht.readHumidity();
  if (isnan(temp)) temp = 0;
  if (isnan(hum))  hum  = 0;

  // Đọc cảm biến mưa (LOW = ướt tuỳ module; bạn đảo nếu thực tế khác)
  int rainState = digitalRead(RAIN_SENSOR_PIN);
  rainDetected = (rainState == LOW) ? 1 : 0;

  // Xử lý cảm biến mưa (chế độ auto)
  if (mode == "auto") {
    if (rainDetected && !motorRunning && systemState != "closed") {
      Serial.println("🌧️ Mưa -> Thu vào");
      motorRunning = true;
      stepper.moveTo(stepper.currentPosition() - travelSteps);
      while (stepper.distanceToGo() != 0) stepper.run();
      motorRunning = false;
      systemState = "closed";
    } else if (!rainDetected && !motorRunning && systemState != "open") {
      Serial.println("☀️ Tạnh mưa -> Kéo ra");
      motorRunning = true;
      stepper.moveTo(stepper.currentPosition() + travelSteps);
      while (stepper.distanceToGo() != 0) stepper.run();
      motorRunning = false;
      systemState = "open";
    }
  }

  // Publish dữ liệu cảm biến (giữ key cũ cho tương thích)
  String jsonData = String("{\"temp\":") + String(temp) +
                    ", \"hum\":"   + String(hum) +
                    ", \"rain\":"  + String(rainDetected) +
                    ", \"state\":\"" + systemState + "\"" +
                    ", \"mode\":\""  + mode + "\"" +
                    // thông tin cấu hình (tuỳ chọn)
                    ", \"line_mm\":" + String(line_length_mm, 1) +
                    ", \"drum_mm\":" + String(drum_diameter_mm, 1) +
                    ", \"steps\":"   + String(travelSteps) +
                    "}";
  client.publish(topic_data, jsonData.c_str());

  delay(3000); // Gửi dữ liệu mỗi 3 giây
}
