const express = require('express');
const cors = require('cors');
const mqtt = require('mqtt');
const app = express();
const port = 3000;

// Kết nối MQTT broker
const mqttClient = mqtt.connect('mqtt://192.168.2.159:1883', {
  clientId: 'NodeServer_ClothesRack',
  username: '', // Nếu cần
  password: ''
});

// Các topic MQTT
const topic_data = 'clothesrack/data';
const topic_mode = 'clothesrack/mode';
const topic_command = 'clothesrack/command';
const topic_config = 'clothesrack/config'; // NEW: cấu hình từ web -> ESP32

// Biến toàn cục lưu trữ trạng thái
let temp = 0;
let hum = 0;
let rain = 0;
let state = 'closed';
let mode = 'auto';
let command = '';

// Xử lý kết nối MQTT
mqttClient.on('connect', () => {
  console.log('Connected to MQTT broker');
  mqttClient.subscribe(topic_data, (err) => {
    if (!err) {
      console.log('Subscribed to topic:', topic_data);
    }
  });
});

// Xử lý tin nhắn MQTT từ ESP32
mqttClient.on('message', (topic, message) => {
  if (topic === topic_data) {
    const data = JSON.parse(message.toString());
    temp = data.temp;
    hum = data.hum;
    rain = data.rain;
    state = data.state;
    mode = data.mode;
    console.log('Dữ liệu nhận từ ESP32:', { temp, hum, rain, state, mode });
  }
});

app.use(express.json());
app.use(cors());

// API cho web (giữ nguyên HTTP)
// 1. Web lấy dữ liệu
app.get('/data', (req, res) => {
  let mData = { mTem: temp, mHum: hum, mRain: rain, mState: state, mMode: mode };
  res.json(mData);
});

// 2. Web gửi lệnh điều khiển
app.post('/command', (req, res) => {
  command = req.body.cmd; // "open" hoặc "close"
  if (command === 'open') state = 'open';
  if (command === 'close') state = 'closed';
  console.log('Nhận lệnh từ web:', command, ' -> state:', state);
  // Publish lệnh tới ESP32 qua MQTT
  mqttClient.publish(topic_command, command);
  res.json({ status: 'ok' });
});

// 3. Web đổi chế độ
app.post('/mode', (req, res) => {
  mode = req.body.mode; // "auto" hoặc "manual"
  console.log('Đổi chế độ:', mode);
  // Publish chế độ tới ESP32 qua MQTT
  mqttClient.publish(topic_mode, mode);
  res.json({ success: true, mode });
});

app.listen(port, () => {
  console.log(`Server listening on port ${port}`);
});
// 4. Web gửi cấu hình chiều dài dây (mm) / đường kính tang (mm) / steps
app.post('/config', (req, res) => {
  const { line_mm, drum_mm, steps } = req.body || {};
  const payload = {};
  if (Number(line_mm) > 0) payload.line_mm = Number(line_mm);
  if (Number(drum_mm) > 0) payload.drum_mm = Number(drum_mm);
  if (Number(steps)   > 0) payload.steps   = Number(steps);

  if (!Object.keys(payload).length) {
    return res.status(400).json({ error: 'No valid config provided' });
  }

  console.log('Publish config to ESP32:', payload);
  mqttClient.publish(topic_config, JSON.stringify(payload));
  res.json({ ok: true, sent: payload });
});

