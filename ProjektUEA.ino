#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <LittleFS.h>

#define HALL_PIN 34

#define OPTO_DRIVE 4
#define TOP_LIMIT 25
#define LIGHT_LVL 26

#define CONF_FILE_NAME "/conf.bin"

// timing 
uint32_t interval = 10UL;
uint32_t lastMillis;
uint32_t currMillis;

byte updateInProgress = false;

enum AppStates {STOPPED = 0, STOPPING, MOVE_UP, MOVE_DOWN, OPEN, CLOSE}; 
AppStates appState;

bool FSMounted;

// stepper motor control interface
inline void motorSTOP();
inline void motorStepUP();
inline void motorStepDOWN();
int motorPosition;
int closePosition;

inline void applicationUpdate();

const char* ssid = "ESP_roleta";
const char* password = "password1";

inline void configureOTA();
inline void configureWebServer();

// values in ADC resolution range (0-4095)
int lightThreshold = 2047;
int hysteresis = 50;
int limitSWThreshold = 2233;

WebServer server(80);

void setup() {
  Serial.begin(115200);
  Serial.println("Booting");

  FSMounted = false;
  if(LittleFS.begin())
    FSMounted = true;
/* 
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
*/

  WiFi.mode(WIFI_MODE_AP);
  if (!WiFi.softAP(ssid, password)) {
    Serial.println("Soft AP creation failed.");
  }


  configureOTA();
  ArduinoOTA.begin();

  configureWebServer();
  server.begin();
  
  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.softAPIP());

  pinMode(16, OUTPUT);
  pinMode(17, OUTPUT);
  pinMode(18, OUTPUT);
  pinMode(19, OUTPUT);

  pinMode(HALL_PIN, INPUT);
  //pinMode(TOP_LIMIT, INPUT);
  adcAttachPin(TOP_LIMIT);
  Serial.println(analogRead(TOP_LIMIT));

  // PWM for optical limit switch
  ledcAttachPin(OPTO_DRIVE, 0);
  ledcSetup(0, 1000, 8); //freq 1kHz, res 8bit
  ledcWrite(0, 127); //duty cycle approx. 50%

  // TODO filesystem config read
  closePosition = 1000;
  if(FSMounted)
  {
    File conf;
    const int INTSIZE = sizeof(closePosition);
    if(LittleFS.exists(CONF_FILE_NAME))
    {
      conf = LittleFS.open(CONF_FILE_NAME, "r");
      if(conf)
      {
        conf.readBytes(reinterpret_cast<char*>(&closePosition), INTSIZE);
        conf.close();
      }
    }
    else  //if config file does not exist, create one with default data
    {
      conf = LittleFS.open(CONF_FILE_NAME, "w");
      if(conf)
      {
        conf.write(reinterpret_cast<uint8_t*>(&closePosition), INTSIZE);
        conf.close();
      }
    }
  }
}


void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  
  if(!updateInProgress) //run application only when update is NOT in progress
  {
    applicationUpdate();
    delay(0);
  }
}


inline void configureOTA()
{
  ArduinoOTA.setHostname("esp8266roleta");
  
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
      updateInProgress = true;
      motorSTOP();
    } else { // U_FS
      type = "filesystem";
      LittleFS.end();
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);
  });
  
  ArduinoOTA.onEnd([]() {
    updateInProgress = false;
    Serial.println("\nEnd");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
  });
}

const char TEXT_HTML[] PROGMEM = "text/html";
const char INDEX[] PROGMEM = u8"<!DOCTYPE html><html><head>"
      "<title>Roleta ESP</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\"><meta charset=\"UTF-8\">"
      "<script>function request(endpoint,method,body=''){var obj={method: method};if(body.length)obj['body']=body;"
      "fetch(endpoint,obj).then((response)=>{var status=document.getElementById(\"status\");if(response.ok){status.innerHTML=\"OK\";"
      "status.style.color=\"LimeGreen\";}else{status.innerHTML=\"ERROR\";status.style.color=\"OrangeRed\";}return response.text()"
      "}).then((data)=>{var status=document.getElementById(\"status\");status.innerHTML+=\" \"+data;});}</script>"
      "</head><body><h1>Sterowanie automatyczne</h1><p><button onclick=\"request('/open','POST')\">Odsuń</button>"
      "<button onclick=\"request('/close','POST')\">Zasuń</button></p><h1>Sterowanie ręczne</h1>"
      "<p><button onclick=\"request('/move_up','POST')\">Podnoś</button>"
      "<button onclick=\"request('/stop','POST')\">Zatrzymaj</button>"
      "<button onclick=\"request('/move_down','POST')\">Opuszczaj</button></p><h1>Konfiguracja</h1><p>"
      "<button onclick=\"request('/save_position','POST')\">Zapisz pozycje</button>"
      "<button onclick=\"request('/reset','POST')\">RESET</button>"
      "<button onclick=\"request('/test_error', 'POST')\">test error</button></p>"
      "<p>Ustaw okres ruchu: <input type='number' id='intrvl'><button onclick=\"request('/interval','POST',document.getElementById('intrvl').value)\">"
      "Ustaw</button></p><p><button onclick=\"request('/interval','GET')\">Okres ruchu</button>"
      "<button onclick=\"request('/motor_position','GET')\">Pozycja aktualna</button>"
      "<button onclick=\"request('/close_position','GET')\">Pozycja zapisana</button>"
      "<button onclick=\"request('/fs_state','GET')\">System plików</button></p>"
      "<h3>Status ostatniej operacji</h3><p id=\"status\"></p></body></html>";

const char TEXT_PLAIN[] PROGMEM = "text/plain";
const char ERROR_FILEWRITE[] PROGMEM = u8"Nie można zapisać pliku";
const char ERROR_SETINTERVAL[] PROGMEM = u8"Nie można dokonać konwersji danych na liczbę całkowitą";

inline void configureWebServer()
{
  server.on("/move_up", HTTP_POST, [](){  
    appState = MOVE_UP; 
    server.send(200);
  });
    
  server.on("/open", HTTP_POST, [](){ 
    appState = OPEN;
    server.send(200); 
  });
    
  server.on("/move_down", HTTP_POST, [](){ 
    appState = MOVE_DOWN; 
    server.send(200);
  });
    
  server.on("/close", HTTP_POST, [](){ 
    appState = CLOSE; 
    server.send(200);
  });
    
  server.on("/stop", HTTP_POST, [](){ 
    appState = STOPPING; 
    server.send(200);
  });

  server.on("/motor_position", HTTP_GET, [](){
    server.send(200, FPSTR(TEXT_PLAIN), String(motorPosition).c_str());
  });
  
  server.on("/app_state", HTTP_GET, [](){ 
    server.send(200, FPSTR(TEXT_PLAIN), String(appState).c_str()); 
  }); 
  
  server.on("/close_position", HTTP_GET, [](){ 
    server.send(200, FPSTR(TEXT_PLAIN), String(closePosition).c_str()); 
  });

  server.on("/save_position", HTTP_POST, [](){
    closePosition = motorPosition;
    bool succes = false;
    if(FSMounted) 
    {
      File conf = LittleFS.open(CONF_FILE_NAME, "w");
      if(conf)
      {
        conf.write(reinterpret_cast<uint8_t*>(&closePosition), sizeof(closePosition));
        conf.close();
        succes = true;
      }
    }

    if(succes)
      server.send(200);
    else
      server.send_P(500, TEXT_PLAIN, ERROR_FILEWRITE); 
  });

  server.on("/",  [](){
    server.send_P(200, TEXT_HTML, INDEX);
  });

  server.on("/test_error", [](){
    server.send_P(500, TEXT_PLAIN, ERROR_FILEWRITE);
  });

  server.on("/reset", HTTP_POST, [](){
    server.send(200);
    LittleFS.end();
    ESP.restart();
  });

  server.on("/fs_state", HTTP_GET, [](){
    server.send(200, FPSTR(TEXT_PLAIN), String(FSMounted).c_str());
  });

  server.on("/interval", HTTP_GET, [](){
    server.send(200, FPSTR(TEXT_PLAIN), String(interval).c_str());
  });

  server.on("/interval", HTTP_POST, [](){
    uint32_t temp = server.arg("plain").toInt();
    if(temp != 0) {
      interval = temp;
      server.send(200);
    }
    else
      server.send_P(500, TEXT_PLAIN, ERROR_SETINTERVAL);
  });
}

// stepper motor control

//constants for stepper control 
const uint32_t clearMask = 0xF0000;
const uint32_t sequenceMask[4] = {0x90000, 0xC0000, 0x60000, 0x30000};

//index of sequence mask
uint32_t sequenceIndex = 0;

inline void motorSTOP() 
{
  REG_WRITE(GPIO_OUT_W1TC_REG, clearMask);
}

inline void motorStepUP() 
{
  sequenceIndex = (sequenceIndex + 1) % 4;
  REG_WRITE(GPIO_OUT_W1TC_REG, clearMask);
  REG_WRITE(GPIO_OUT_W1TS_REG, sequenceMask[sequenceIndex]);
  --motorPosition;
}

inline void motorStepDOWN() 
{
  sequenceIndex = (sequenceIndex + 3) % 4;
  REG_WRITE(GPIO_OUT_W1TC_REG, clearMask);
  REG_WRITE(GPIO_OUT_W1TS_REG, sequenceMask[sequenceIndex]);
  ++motorPosition;
}

inline void applicationUpdate()
{
  int limitSwitch = analogRead(TOP_LIMIT);
  int lightSensor = analogRead(LIGHT_LVL);

  currMillis = millis();
  if((uint32_t)(currMillis - lastMillis) > interval)
  {
    if((limitSwitch > limitSWThreshold + hysteresis) && (lightSensor > lightThreshold + hysteresis)) {
      appState = OPEN;
    }

    if((limitSwitch < limitSwitch - hysteresis) && (lightSensor < lightThreshold - hysteresis)) {
      appState = CLOSE;
    }

    switch(appState)
    {
      case OPEN:
        if( limitSwitch < limitSWThreshold - hysteresis)
        {
          appState = STOPPING;
          motorPosition = 0;
        }
      case MOVE_UP:
        motorStepUP();
        break;
        
      case CLOSE:
        if(motorPosition >= closePosition)
          appState = STOPPING;
      case MOVE_DOWN:
        motorStepDOWN();
        break;
      
      case STOPPING:
        motorSTOP();
        appState = STOPPED;
        break;
    }
    
    lastMillis = currMillis;
  }
}