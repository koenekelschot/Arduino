#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

//////////////////////
// WiFi Definitions //
//////////////////////
const char WiFiSSID[] = "SomeSSID";
const char WiFiPSK[] = "SomePassword";

WiFiServer server(80);

/////////////////////
// Pin Definitions //
/////////////////////
const unsigned int LED_PIN = 5; // Thing's onboard, blue LED
const unsigned int PUMP1_PIN = 4;
const unsigned int PUMP2_PIN = 15;

//////////////////////////////////
// Basic Definitions for uptime //
/////////////////////////////////
long Days = 0;
int Hours = 0;
int Minutes = 0;
int Seconds = 0;
int HighMillis = 0;
int Rollovers = 0;

void setup() 
{
  initHardware();
  connectWiFi();
  server.begin();
  setupMDNS();
}

void loop() 
{
  // Check if a client has connected
  WiFiClient client = server.available();
  if (!client) {
    return;
  }

  // Read the first line of the request
  String req = client.readStringUntil('\r');
  //if (req.length() == 0 || req == "\r" || req == "\n") {
  //  req = client.readStringUntil('\r');    
  //}
  Serial.println(req);
  client.flush();

  //parse the request
  if (req.indexOf("POST /") == -1) {
    //Invalid method
    String response = "HTTP/1.1 405 Method Not Allowed\r\n";
    response += "Content-Type: application/json\r\n\r\n";
    response += "{error: {code: 405, message: \"Method not allowed. Try POST.\"}\n";
    
    sendResponse(client, response);
    return;
  }
  
  if (req.indexOf("/pump/1/on") != -1) 
  {
    digitalWrite(PUMP1_PIN, HIGH);
  }
  else if (req.indexOf("/pump/1/off") != -1) 
  {
    digitalWrite(PUMP1_PIN, LOW);
  }
  else if (req.indexOf("/pump/2/on") != -1) 
  {
    digitalWrite(PUMP2_PIN, HIGH);
  }
  else if (req.indexOf("/pump/2/off") != -1) 
  {
    digitalWrite(PUMP2_PIN, LOW);
  } 
  else 
  {
    //Invalid url
    String response = "HTTP/1.1 404 Not Found\r\n";
    response += "Content-Type: application/json\r\n\r\n";
    response += "{error: {code: 404, message: \"Not found. Try /pump/(1|2)/(on|off).\"}\n";
    
    sendResponse(client, response);
    return;
  }

  //Valid request
  String response = "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: application/json\r\n\r\n";
  response += "{status:{pump1:\"";
  response += (digitalRead(PUMP1_PIN) == HIGH ? "on" : "off");
  response += "\",pump2:\"";
  response += (digitalRead(PUMP2_PIN) == HIGH ? "on" : "off");
  response += "\"},uptime:";
  response += printUptime();
  response += "}\n";
  
  sendResponse(client, response);

  // The client will actually be disconnected 
  // when the function returns and 'client' object is detroyed
}

void sendResponse(WiFiClient client, String response) {
  client.flush();
  client.print(response);
  client.flush();
  delay(1);
  Serial.println("Client disonnected");
}

void connectWiFi()
{
  byte ledStatus = LOW;
  Serial.println();
  Serial.println("Connecting to: " + String(WiFiSSID));
  // Set WiFi mode to station (as opposed to AP or AP_STA)
  WiFi.mode(WIFI_STA);

  // WiFI.begin([ssid], [passkey]) initiates a WiFI connection
  // to the stated [ssid], using the [passkey] as a WPA, WPA2,
  // or WEP passphrase.
  WiFi.begin(WiFiSSID, WiFiPSK);

  // Use the WiFi.status() function to check if the ESP8266
  // is connected to a WiFi network.
  while (WiFi.status() != WL_CONNECTED)
  {
    // Blink the LED
    digitalWrite(LED_PIN, ledStatus); // Write LED high/low
    ledStatus = (ledStatus == HIGH) ? LOW : HIGH;

    // Delays allow the ESP8266 to perform critical tasks
    // defined outside of the sketch. These tasks include
    // setting up, and maintaining, a WiFi connection.
    delay(100);
    // Potentially infinite loops are generally dangerous.
    // Add delays -- allowing the processor to perform other
    // tasks -- wherever possible.
  }
  ledStatus = HIGH;
  Serial.println("WiFi connected");  
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void setupMDNS()
{
  // Call MDNS.begin(<domain>) to set up mDNS to point to
  // "<domain>.local"
  if (!MDNS.begin("thing")) 
  {
    Serial.println("Error setting up MDNS responder!");
    while(1) { 
      delay(1000);
    }
  }
  Serial.println("mDNS responder started");
}

void initHardware()
{
  Serial.begin(9600);
  //pinMode(DIGITAL_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  pinMode(PUMP1_PIN, OUTPUT);
  pinMode(PUMP2_PIN, OUTPUT);
  // Don't need to set ANALOG_PIN as input, 
  // that's all it can be.
}

void uptime() {
  //Makes a count of the total up time since last start

  //Making note of an expected rollover
  if (millis() >= 3000000000)
  {
    HighMillis = 1;
  }
  
  //Making note of actual rollover
  if (millis() <= 100000 && HighMillis == 1)
  {
    Rollovers++;
    HighMillis = 0;
  }

  long secsUp = millis() / 1000;
  Seconds = secsUp % 60;
  Minutes = (secsUp / 60) % 60;
  Hours = (secsUp / (60 * 60)) % 24;
  Days = (Rollovers * 50) + (secsUp / (60 * 60 * 24));  //First portion takes care of a rollover [around 50 days]
}

String printUptime() {
  uptime();
  String uptime = "{";
  uptime += "days:";
  uptime += Days;
  uptime += ",hours:";
  uptime += Hours;
  uptime += ",minutes:";
  uptime += Minutes;
  uptime += ",seconds:";
  uptime += Seconds;
  uptime += "}";
  return uptime;
}

