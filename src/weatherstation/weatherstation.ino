#include <NanoESP.h>
#include <NanoESP_HTTP.h>
#include <Thread.h>
#include "arduino_secrets.h"

const int WIFI_RECONNECT_ATTEMPTS = 10;
const int WIFI_RECONNECT_BACKOFF = 15; //seconds
const int MQTT_CONNECTION_ID = 0;
const int MQTT_PUBLISH_INTERVAL = 30; //seconds
const String MQTT_ROOT_TOPIC = "weatherstation";
const int HTTP_SERVER_PORT = 80;
const String HTTP_DATA_PATH = "/data";
const String PUBLISH_NAME_TEMPERATURE = "temperature";
const String PUBLISH_NAME_LIGHT = "light";
const String PUBLISH_NAME_RAIN = "rain";

const uint8_t LED_PIN = 10;
const uint8_t THERMISTOR_PIN = A6;
const uint8_t PHOTORESISTOR_PIN = A7;
const uint8_t RAINSENSOR_PIN = A0;

const int NUM_SENSOR_SAMPLES = 30;
const int THERMISTOR_RESISTANCE = 10000;
const float THERMISTOR_CORRECTION = 0; //0.8
const int PHOTORESISTOR_RESISTANCE = 10000;

NanoESP _nanoesp = NanoESP();
NanoESP_HTTP _http = NanoESP_HTTP(_nanoesp);
Thread _wifiThread = Thread();
Thread _mqttThread = Thread();
Thread _sensorThread = Thread();
Thread _publishThread = Thread();

void connectWifi(int attempt = 1, int maxAttempts = 1);
bool _mqttConnected = false;
const long _mqttKeepAliveTime = 120; //seconds
unsigned long _mqttPreviousMillisSend = 0;
bool _firstSensorReading = true;
int _temperatureData[NUM_SENSOR_SAMPLES];
int _lightData[NUM_SENSOR_SAMPLES];
int _rainData[NUM_SENSOR_SAMPLES];
float _temperature;
float _lightPercentage;
float _rainPercentage;

void setup() {
  // put your setup code here, to run once:
  Serial.begin(19200);
  pinMode(LED_PIN, OUTPUT);

  _nanoesp.init(false);
  _nanoesp.sendCom("AT+CWDHCP=1,1"); //enable DHCP for station mode

  _wifiThread.onRun(checkWifiConnection);
  _wifiThread.setInterval(30000);
  _mqttThread.onRun(pingMqtt);
  _mqttThread.setInterval((_mqttKeepAliveTime / 4) * 1000);
  _sensorThread.onRun(readSensorData);
  _sensorThread.setInterval(1000);
  _publishThread.onRun(publishSensorData);
  _publishThread.setInterval(MQTT_PUBLISH_INTERVAL * 1000);

  connectWifi();
  connectMqtt();
  readSensorData();

  _nanoesp.startTcpServer(HTTP_SERVER_PORT);
}

void loop() {
  // put your main code here, to run repeatedly:
  if (_wifiThread.shouldRun()) {
    _wifiThread.run(); //This will block everything if reconnecting isn't possible
  }
  if (_mqttThread.shouldRun()) {
    _mqttThread.run();
  }
  if (_sensorThread.shouldRun()) {
    _sensorThread.run();
  }
  if (_publishThread.shouldRun()) {
    _publishThread.run();
  }

  processHttpRequest();
  delay(500);
}

void checkWifiConnection() {
  Serial.println("Checking Wifi connection");
  if (!_nanoesp.wifiConnected()) {
    disconnectMqtt();
    connectWifi(1, WIFI_RECONNECT_ATTEMPTS);
  }
}

void connectWifi(int attempt, int maxAttempts) {
  Serial.println("Connecting to Wifi. Attempt " + String(attempt) + " of " + String(maxAttempts));
  digitalWrite(LED_PIN, HIGH);
  if (_nanoesp.configWifi(STATION, SECRET_WIFI_SSID, SECRET_WIFI_PASS)) {
    Serial.println("Wifi connected.");
    Serial.println(_nanoesp.getIp());
    digitalWrite(LED_PIN, LOW);
  } else {
    Serial.println("Wifi not connected");
    delay(500);
    if (attempt < maxAttempts) {
      int seconds = (attempt + 1) * WIFI_RECONNECT_BACKOFF;
      Serial.println("Trying again in " + String(seconds) + "s");
      blinkLed(seconds);
      connectWifi(++attempt, maxAttempts);
    } else {
      Serial.println("DON'T PANIC");
      panic();
    }
  }
}

void readSensorData() {
  Serial.println("Reading data from sensors");
  _temperature = convertToTemperature(recalculateAverage(_temperatureData, analogRead(THERMISTOR_PIN)));
  _lightPercentage = convertToPercentage(recalculateAverage(_lightData, analogRead(PHOTORESISTOR_PIN)));
  _rainPercentage = convertToPercentage(recalculateAverage(_rainData, 1023 - analogRead(RAINSENSOR_PIN)));
  _firstSensorReading = false;
}

float recalculateAverage(int data[], int newValue) {
  float sum = 0.0;
  for (int i = 0; i < NUM_SENSOR_SAMPLES - 1; i++) {
    data[i] = _firstSensorReading ? newValue : data[i + 1];
    sum += data[i];
  }
  data[NUM_SENSOR_SAMPLES - 1] = newValue;
  sum += newValue;
  return sum / NUM_SENSOR_SAMPLES;
}

float convertToTemperature(int analogInput) {
  //source: http://www.circuitbasics.com/arduino-thermistor-temperature-sensor-tutorial/
  float c1 = 1.009249522e-03;
  float c2 = 2.378405444e-04;
  float c3 = 2.019202697e-07;
  float logR2 = log(THERMISTOR_RESISTANCE * (1023.0 / analogInput - 1.0));
  return (1.0 / (c1 + c2 * logR2 + c3 * logR2 * logR2 * logR2)) - (273.15 + THERMISTOR_CORRECTION);
}

float convertToPercentage(int analogInput) {
  return (analogInput / 1023.0) * 100;
}

void publishSensorData() {
  if (!_mqttConnected) {
    Serial.println("Publishing data, but no MQTT connection established");
    connectMqtt();
  }
  //should be connected by now, so retry
  if (_mqttConnected) {
    Serial.println("Publishing data");
    sendTopicData(PUBLISH_NAME_TEMPERATURE, String(_temperature));
    sendTopicData(PUBLISH_NAME_LIGHT, String(_lightPercentage));
    sendTopicData(PUBLISH_NAME_RAIN, String(_rainPercentage));
  }
}

void sendTopicData(String topic, String data) {
  publishMqtt(MQTT_ROOT_TOPIC + "/" + topic, data);
}

void processHttpRequest() {
  String method, resource, parameter;
  int id;

  if (_http.recvRequest(id, method, resource, parameter)) { //Incoming request, parameters by reference
    Serial.println("Received HTTP request " + String(id));
    if (!isAllowedHttpRequest(method, resource)) {
      _nanoesp.sendData(id, "HTTP/1.1 403 Forbidden");
    } else {
      _nanoesp.sendData(id, "HTTP/1.1 200 OK");
      _nanoesp.sendData(id, "Content-Type: application/json");
      _nanoesp.sendData(id, "\r\n{");
      _nanoesp.sendData(id, "\"" + PUBLISH_NAME_TEMPERATURE + "\":" + _temperature + ",");
      _nanoesp.sendData(id, "\"" + PUBLISH_NAME_LIGHT + "\":" + _lightPercentage + ",");
      _nanoesp.sendData(id, "\"" + PUBLISH_NAME_RAIN + "\":" + _rainPercentage);
      _nanoesp.sendData(id, "}");
    }
    _nanoesp.closeConnection(id);
  }
}

bool isAllowedHttpRequest(String method, String resource) {
  return method.equalsIgnoreCase("get") && resource.equalsIgnoreCase(HTTP_DATA_PATH);
}

void blinkLed(int seconds) {
  for (int i = 0; i < seconds; i += 2) {
    setLedMode(HIGH, 1000);
    setLedMode(LOW, 1000);
  }
}

void panic() {
  //The dot duration is the basic unit of time measurement in code transmission.
  //The duration of a dash is three times the duration of a dot.
  //Each dot or dash is followed by a short silence, equal to the dot duration.
  //The letters of a word are separated by a space equal to three dots (one dash),
  //and the words are separated by a space equal to seven dots.
  int dotLength = 200;
  setLedMode(LOW, dotLength * 7);
  while (true) {
    morse('S', dotLength);
    morse('O', dotLength);
    morse('S', dotLength);
    setLedMode(LOW, dotLength * 4); //length of space minus length of dash
  }
}

void morse(char character, int dotLength) {
  if (character == 's' || character == 'S') {
    setLedMode(HIGH, dotLength);
    setLedMode(LOW, dotLength);
    setLedMode(HIGH, dotLength);
    setLedMode(LOW, dotLength);
    setLedMode(HIGH, dotLength);
    setLedMode(LOW, dotLength * 3);
  } else if (character == 'o' || character == 'O') {
    setLedMode(HIGH, dotLength * 3);
    setLedMode(LOW, dotLength);
    setLedMode(HIGH, dotLength * 3);
    setLedMode(LOW, dotLength);
    setLedMode(HIGH, dotLength * 3);
    setLedMode(LOW, dotLength * 3);
  }
}

void setLedMode(int mode, int duration) {
  digitalWrite(LED_PIN, mode);
  delay(duration);
}

void connectMqtt() {
  Serial.println("Connecting to MQTT");
  _mqttConnected = false;
  if (!_nanoesp.wifiConnected()) {
    return;
  }
  
  if (!_nanoesp.newConnection(MQTT_CONNECTION_ID, TCP, SECRET_MQTT_BROKER, SECRET_MQTT_PORT)) {
    return;
  }
  
  byte connectFlag = 0;
  if (SECRET_MQTT_USER != "") connectFlag += 1;
  connectFlag = connectFlag << 1;
  if (SECRET_MQTT_PASS != "") connectFlag += 1;
  connectFlag = connectFlag << 1;
  connectFlag = connectFlag << 4;
  connectFlag += true; //cleanSession
  connectFlag = connectFlag << 1;

  unsigned char data[]  = {
    //Header (CONNECT BYTE, LEN)
    0x10, 0x00,
    //Variable Header
    0x00, 0x04, 'M', 'Q', 'T', 'T', //String LEN (2BYTE) + Protocol name
    0x04, //Protocol Level4
    connectFlag, //Connect Flag (User Name, Password, Will Retain, Will QoS, Will flag, Clean Session, X)
    0x00, _mqttKeepAliveTime, //Keep Alive Timer
    //0x00, 0x00 //String Len UTF-8?
  };//CONNEC message initialization,
  //Then: ClientIdentifier, Will: Topic, Message, UserName, Password

  int lenMsg = sizeof(data);
  unsigned char vDeviceId[SECRET_MQTT_CLIENT.length() + 2];
  utf8(SECRET_MQTT_CLIENT, vDeviceId);
  lenMsg += sizeof(vDeviceId);

  unsigned char vUserName[SECRET_MQTT_USER.length() + 2];
  unsigned char vPassword[SECRET_MQTT_PASS.length() + 2];

  if (SECRET_MQTT_USER != "") {
    utf8(SECRET_MQTT_USER, vUserName);
    utf8(SECRET_MQTT_PASS, vPassword);
    lenMsg += sizeof(vUserName);
    lenMsg += sizeof(vPassword);
  }
  //len MSG
  data[1] = lenMsg - 2;

  unsigned char msg [lenMsg];
  int lenTemp = sizeof(data);
  charAdd(data, lenTemp, vDeviceId, sizeof(vDeviceId), msg);
  lenTemp += sizeof(vDeviceId);

  if (SECRET_MQTT_USER != "") {
    charAdd(msg, lenTemp, vUserName, sizeof(vUserName), msg);
    lenTemp += sizeof(vUserName);
    charAdd(msg, lenTemp, vPassword, sizeof(vPassword), msg);
    lenTemp += sizeof(vPassword);
  }

  if (sendMqtt(msg, sizeof(msg), 2 << 4)) {
    char buffer[2];
    !_nanoesp.readBytes(buffer, 2); //After Header for ConnAck
    if (buffer[1] != 0) {
      Serial.print(F("Connection refused. Code: "));
      Serial.println(buffer[1], HEX);
    } else {
      _mqttConnected = true;
      Serial.println("Connected to MQTT");
    }
  }
}

void disconnectMqtt() {
  Serial.println("Disconnecting from MQTT");
  // Fixed Head,MS,LSB,    ,M,   Q       ?       ?   ?     ?  ,Ver X?,Conec,Kepp Alive tim
  unsigned char data[]  = {
    //Header
    14 << 4, 0x00, //Publish
  };
  sendMqtt(data, sizeof(data));
  _mqttConnected = false;
}

void pingMqtt() {
  if (_mqttConnected) {
    Serial.println("Sending MQTT ping");
    // Fixed Head
    unsigned char data[]  = {
      //Header
      12 << 4, 0x00, //Publish
    };
    sendMqtt(data, sizeof(data), 13 << 4);
  }
}

void publishMqtt(const String& topic, const String& value) {
  // Fixed Head
  unsigned char data[]  = {
    //Header
    0x30, 0x00 //Publish
  };
  //MQTT_PUB .3 = DUP, .2.1 = QOS, .0 = RETAIN
  data[0] += 0 << 1;
  data[0] += 0;

  unsigned char vTopic[topic.length() + 2];
  utf8(topic, vTopic);
  int lenMsg  = sizeof(data) + sizeof(vTopic) + value.length();
  unsigned char vmsg [lenMsg];
  int lenTemp = sizeof(data);
  charAdd(data, lenTemp, vTopic, sizeof(vTopic), vmsg);
  lenTemp += sizeof(vTopic);

  for (int i = 0; i < value.length(); i++) {
    vmsg[i + lenTemp] = value[i];
  }
  vmsg[1] = lenMsg - 2;

  sendMqtt(vmsg, sizeof(vmsg));
}

bool sendMqtt(unsigned char data[], int LenChar) {
  data[1] = LenChar - 2; //Header Len
  _nanoesp.println("AT+CIPSEND=" + String(MQTT_CONNECTION_ID) + "," + String(LenChar));
  if (_nanoesp.find(">")) {
    for (int i = 0; i < LenChar; i++) {
      _nanoesp.write(data[i]);
    }
    if (_nanoesp.find("OK\r\n")) {
      _mqttPreviousMillisSend = millis(); //Reset time till ping
      return true; //Short it
    }
  }
  return false;
}

bool sendMqtt(unsigned char data[], int LenChar, char ack) {
  data[1] = LenChar - 2; //Header Len
  _nanoesp.println("AT+CIPSEND=" + String(MQTT_CONNECTION_ID) + "," + String(LenChar));
  if (_nanoesp.find(">")) {
    for (int i = 0; i < LenChar; i++) {
      _nanoesp.write(data[i]);
    }
    if (_nanoesp.find("OK\r\n")) {
      _mqttPreviousMillisSend = millis(); //Reset time till ping
      _nanoesp.find("+IPD");
      _nanoesp.find(":");

      //Get ACK Header
      char buffer[2] ; //Header should be 2!!! 
      _nanoesp.readBytes(buffer, 2);
      if (buffer[0] == ack) {
        return true;
      }
    }
  }
  return false;
}

void utf8(const String& input, unsigned char* output) {
  byte len = input.length();
  output[0] = 0;
  output[1] = len;
  for (int i = 0; i < len ; i++) {
    output[i + 2] = input[i];
  }
}

void charAdd(unsigned char* inputA, int lenA, unsigned char* inputB, int lenB, unsigned char* output) {
  for (int i = 0; i < lenA; i++) {
    output[i] = inputA[i];
  }
  for (int i = 0; i < lenB; i++) {
    output[i + lenA] = inputB[i];
  }
}

