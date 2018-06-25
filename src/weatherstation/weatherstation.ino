#include <NanoESP.h>
#include <NanoESP_HTTP.h>
#include <Thread.h>
#include "arduino_secrets.h"

const int WIFI_RECONNECT_ATTEMPTS = 10;
const int WIFI_RECONNECT_BACKOFF = 15; //seconds
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
Thread _sensorThread = Thread();

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
  _sensorThread.onRun(readSensorData);
  _sensorThread.setInterval(1000);

  connectWifi();
  readSensorData();

  _nanoesp.startTcpServer(HTTP_SERVER_PORT);
}

void loop() {
  // put your main code here, to run repeatedly:
  if (_wifiThread.shouldRun()) {
    _wifiThread.run(); //This will block everything if reconnecting isn't possible
  }
  if (_sensorThread.shouldRun()) {
    _sensorThread.run();
  }

  processHttpRequest();
  delay(500);
}

void checkWifiConnection() {
  Serial.println("Checking Wifi connection");
  if (!_nanoesp.wifiConnected()) {
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

