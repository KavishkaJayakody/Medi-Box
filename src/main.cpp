#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <vector>
#include <cmath>

// OLED Display settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// DHT22 settings
#define DHTPIN 15
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

//Servosettings
Servo motor;

// Parameters
float minAngle = 30.0;
float motorAngleFac = 0.75;
float Tmed = 30.00;
int ts = 5;
int tu = 120;

// Button pins
#define SELECT_BTN 5
#define UP_BTN 4
#define DOWN_BTN 2
#define BACK_BTN 16

// State
bool isScheduledON = false;
unsigned long scheduledOnTime;
unsigned long lastSampleTime = 0;
unsigned long lastSendTime = 0;
std::vector<float> lightSamples;

char tempAr[6];
char lightAr[6];


//LDR anf motor pin
#define LDR 35
#define MOTOR 26

// Buzzer and LED pins
#define BUZZER_PIN 14
#define LED_PIN 17

// WiFi credentials
const char* ssid = "Wokwi-GUEST";
const char* password = "";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// NTP settings
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");

// Menu states
enum MenuState {
  IDLE,
  MAIN_MENU,
  SET_TIMEZONE,
  SET_ALARM,
  VIEW_ALARMS,
  DELETE_ALARM
};
MenuState currentState = IDLE;
int selectedMenuItem = 0;
int editingValue = 0;
bool isEditing = false;

// Global variables
int timezone_offset = 0;
struct Alarm {
  bool active;
  int hour;
  int minute;
};
Alarm alarms[2] = {{false, 0, 0}, {false, 0, 0}};
bool alarm_ringing = false;
unsigned long snooze_start = 0;
bool snooze_active = false;

// Button debouncing
unsigned long lastButtonPress = 0;
const int DEBOUNCE_DELAY = 200;

// Function prototypes
void displayIdle();
void displayMainMenu();
void displaySetTimezone();
void displaySetAlarm();
void displayViewAlarms();
void displayDeleteAlarm();
void handleButtons();
void checkEnvironment();
void checkAlarms();

void receiveCallback(char* topic, byte* payload, unsigned int length);
void connectToBroker();
float readLightIntensity();
float computeAverage(const std::vector<float>& samples);
void updateTemperature();
void AdjustServoMotor(float I, float T);
void checkSchedule();
void buzzerOn(bool on);


void setup() {

  Serial.begin(115200);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;); // Don't proceed if OLED fails
  }
  display.clearDisplay();
  display.display();

  dht.begin();

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(SELECT_BTN, INPUT_PULLUP);
  pinMode(UP_BTN, INPUT_PULLUP);
  pinMode(DOWN_BTN, INPUT_PULLUP);
  pinMode(BACK_BTN, INPUT_PULLUP);

  motor.attach(MOTOR, 500, 2400);
  motor.write(minAngle);

  WiFi.begin(ssid, password);
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Connecting to WiFi");
  display.display();

  delay(1000);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    display.print(".");
    display.display();
  }
  display.println("\nConnected!");
  display.display();
  delay(1000);


  mqttClient.setServer("test.mosquitto.org", 1883);
  Serial.println("MQTT server set");
  mqttClient.setCallback(receiveCallback);
  Serial.println("MQTT callback set");

  timeClient.begin();
  timeClient.setTimeOffset(timezone_offset * 3600);
}

void loop() {

  if (!mqttClient.connected()) connectToBroker();
    mqttClient.loop();
  static unsigned long lastDisplayUpdate = 0;
  static unsigned long lastEnvCheck = 0;
  unsigned long now = millis();

  handleButtons();

  if (millis() - lastDisplayUpdate >= 1000) {
    if (!alarm_ringing) {
      switch (currentState) {
        case IDLE:
          displayIdle();
          break;
        case MAIN_MENU:
          displayMainMenu();
          break;
        case SET_TIMEZONE:
          displaySetTimezone();
          break;
        case SET_ALARM:
          displaySetAlarm();
          break;
        case VIEW_ALARMS:
          displayViewAlarms();
          break;
        case DELETE_ALARM:
          displayDeleteAlarm();
          break;
      }
    }
    lastDisplayUpdate = millis();
  }


  if (now - lastSampleTime >= ts * 1000) {
    lastSampleTime = now;
    float intensity = readLightIntensity();
    //Serial.printf("Light Intensity: %.2f\n", intensity);
    lightSamples.push_back(intensity);
  }
  //Serial.println("lightsamples: updated");

  if (now - lastSendTime >= tu * 1000) {//*1000
    lastSendTime = now;
    float avgIntensity = computeAverage(lightSamples);
    lightSamples.clear();
    updateTemperature();

    mqttClient.publish("MEDIBOX-DATA-LIGHT", lightAr);
    mqttClient.publish("MEDIBOX-DATA-TEMP", tempAr);

    float T = atof(tempAr);

    AdjustServoMotor(avgIntensity, T);
  }



  if (millis() - lastEnvCheck >= 2000) {
    checkEnvironment();
    lastEnvCheck = millis();
  }

  checkAlarms();
}

// Function definitions

void displayIdle() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  
  timeClient.update();
  int current_hour = timeClient.getHours();
  int current_minute = timeClient.getMinutes();
  int current_second = timeClient.getSeconds();
  
  display.setTextSize(2);
  display.setCursor(0, 10);
  if (current_hour < 10) display.print("0");
  display.print(current_hour);
  display.print(":");
  if (current_minute < 10) display.print("0");
  display.print(current_minute);
  display.print(":");
  if (current_second < 10) display.print("0");
  display.println(current_second);
  
  display.setTextSize(1);
  display.setCursor(0, 55);
  display.println("Press SELECT for menu");
  
  display.display();
}

void displayMainMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  
  const char* menuItems[] = {
    "Set timezone",
    "Set alarm",
    "View alarms",
    "Delete alarm"
  };
  
  for (int i = 0; i < 4; i++) {
    if (i == selectedMenuItem) display.print("> ");
    else display.print("  ");
    display.println(menuItems[i]);
  }
  display.display();
}

void displaySetTimezone() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Set Timezone");
  display.println();
  display.print("UTC ");
  if (timezone_offset >= 0) display.print("+");
  display.println(String(timezone_offset));
  display.println("\nUP/DOWN to change");
  display.println("SELECT to confirm");
  display.println("BACK to cancel");
  display.display();
}

void displaySetAlarm() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Set Alarm");
  display.println();
  if (!isEditing) {
    display.print("> Hour: ");
    display.println(editingValue);
    display.println("UP/DOWN to change");
    display.println("SELECT to set minute");
  } else {
    display.println("Hour: " + String(alarms[selectedMenuItem].hour));
    display.print("> Minute: ");
    display.println(editingValue);
    display.println("UP/DOWN to change");
    display.println("SELECT to confirm");
  }
  display.println("BACK to cancel");
  display.display();
}

void displayViewAlarms() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Active Alarms:");
  display.println();
  
  bool hasAlarms = false;
  for (int i = 0; i < 2; i++) {
    if (alarms[i].active) {
      hasAlarms = true;
      display.print(i + 1);
      display.print(". ");
      display.print(alarms[i].hour);
      display.print(":");
      if (alarms[i].minute < 10) display.print("0");
      display.println(alarms[i].minute);
    }
  }
  
  if (!hasAlarms) {
    display.println("No active alarms");
  }
  
  display.println("\nBACK to return");
  display.display();
}

void displayDeleteAlarm() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Delete Alarm");
  display.println();
  
  for (int i = 0; i < 2; i++) {
    if (i == selectedMenuItem) display.print("> ");
    else display.print("  ");
    if (alarms[i].active) {
      display.print(alarms[i].hour);
      display.print(":");
      if (alarms[i].minute < 10) display.print("0");
      display.println(alarms[i].minute);
    } else {
      display.println("(empty)");
    }
  }
  
  display.println("\nSELECT to delete");
  display.println("BACK to cancel");
  display.display();
}

void handleButtons() {
  if (millis() - lastButtonPress < DEBOUNCE_DELAY) return;
  
  bool selectPressed = digitalRead(SELECT_BTN) == LOW;
  bool upPressed = digitalRead(UP_BTN) == LOW;
  bool downPressed = digitalRead(DOWN_BTN) == LOW;
  bool backPressed = digitalRead(BACK_BTN) == LOW;
  
  if (!selectPressed && !upPressed && !downPressed && !backPressed) return;
  
  lastButtonPress = millis();
  
  switch (currentState) {
    case IDLE:
      if (selectPressed) {
        currentState = MAIN_MENU;
        selectedMenuItem = 0;
      }
      break;
    case MAIN_MENU:
      if (upPressed && selectedMenuItem > 0) selectedMenuItem--;
      if (downPressed && selectedMenuItem < 3) selectedMenuItem++;
      if (selectPressed) {
        switch (selectedMenuItem) {
          case 0:
            currentState = SET_TIMEZONE;
            editingValue = timezone_offset;
            break;
          case 1:
            currentState = SET_ALARM;
            selectedMenuItem = 0;
            editingValue = 0;
            isEditing = false;
            break;
          case 2: 
            currentState = VIEW_ALARMS;
            break;
          case 3:
            currentState = DELETE_ALARM;
            selectedMenuItem = 0;
            break;
        }
      }
      if (backPressed) currentState = IDLE;
      break;
    case SET_TIMEZONE:
      if (upPressed) editingValue++;
      if (downPressed) editingValue--;
      if (selectPressed) {
        timezone_offset = editingValue;
        timeClient.setTimeOffset(timezone_offset * 3600);
        currentState = MAIN_MENU;
      }
      if (backPressed) currentState = MAIN_MENU;
      break;
    case SET_ALARM:
      if (!isEditing) {
        if (upPressed && editingValue < 23) editingValue++;
        if (downPressed && editingValue > 0) editingValue--;
        if (selectPressed) {
          alarms[selectedMenuItem].hour = editingValue;
          editingValue = 0;
          isEditing = true;
        }
      } else {
        if (upPressed && editingValue < 59) editingValue++;
        if (downPressed && editingValue > 0) editingValue--;
        if (selectPressed) {
          alarms[selectedMenuItem].minute = editingValue;
          alarms[selectedMenuItem].active = true;
          currentState = MAIN_MENU;
        }
      }
      if (backPressed) currentState = MAIN_MENU;
      break;
    case VIEW_ALARMS:
      if (backPressed) currentState = MAIN_MENU;
      break;
    case DELETE_ALARM:
      if (upPressed && selectedMenuItem > 0) selectedMenuItem--;
      if (downPressed && selectedMenuItem < 1) selectedMenuItem++;
      if (selectPressed) {
        alarms[selectedMenuItem].active = false;
        currentState = MAIN_MENU;
      }
      if (backPressed) currentState = MAIN_MENU;
      break;
  }
}

void checkEnvironment() {
  float temp = dht.readTemperature();
  Serial.print(temp);
  float humidity = dht.readHumidity();
  
  if (isnan(temp) || isnan(humidity)) return;

  bool temp_warning = (temp < 24 || temp > 32);
  bool humidity_warning = (humidity < 65 || humidity > 80);

  if (temp_warning || humidity_warning) {
    digitalWrite(LED_PIN, HIGH);
    if (currentState == MAIN_MENU) {
      display.clearDisplay();
      display.setCursor(0, 40);
      if (temp_warning) display.println("Temp Warning!");
      if (humidity_warning) display.println("Humidity Warning!");
      display.display();
    }
  } else {
    digitalWrite(LED_PIN, LOW);
  }
}

void checkAlarms() {
  if (snooze_active && (millis() - snooze_start >= 300000)) {
    snooze_active = false;
    alarm_ringing = true;
  }

  timeClient.update();
  int current_hour = timeClient.getHours();
  int current_minute = timeClient.getMinutes();

  for (int i = 0; i < 2; i++) {
    if (alarms[i].active && 
        current_hour == alarms[i].hour && 
        current_minute == alarms[i].minute && 
        !alarm_ringing && !snooze_active) {
      alarm_ringing = true;
      break;
    }
  }

  if (alarm_ringing) {
    tone(BUZZER_PIN, 1000);
    display.clearDisplay();
    display.setCursor(0, 20);
    display.println("ALARM!");
    display.println("Press SELECT to");
    display.println("stop/snooze");
    display.display();
    
    if (digitalRead(SELECT_BTN) == LOW) {
      alarm_ringing = false;
      snooze_active = true;
      snooze_start = millis();
      noTone(BUZZER_PIN);
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0,0);
      display.println("Alarm snoozed");
      display.println("for 5 minutes");
      display.display();
      delay(2000);
    }
  }
}

float readLightIntensity() {
  float val = analogRead(LDR);
  float intensity = 1.0 - (val - 32.0) / (4063.0 - 32.0);
  intensity = constrain(intensity, 0.0, 1.0);
  return intensity;
}

float computeAverage(const std::vector<float>& samples) {
  if (samples.empty()) return 0.0;
  float sum = 0;
  for (float v : samples) sum += v;
  float avg = sum / samples.size();
  String(avg, 2).toCharArray(lightAr, 6);
  return avg;
}

void updateTemperature() {
  float temp = dht.readTemperature();
  if (!isnan(temp)) {
    String(temp, 2).toCharArray(tempAr, 6);
  } else {
    strcpy(tempAr, "0.00");
  }
}

void AdjustServoMotor(float I, float T) {
  if (I < 0.0 || I > 1.0 || T <= 0 || ts <= 0 || tu <= 0) return;

  double angle = minAngle + (180 - minAngle) * I * motorAngleFac * log((float)ts / (float)tu) * (T / Tmed);
  angle = constrain(angle, minAngle, 180);
  //Serial.println(angle);
  motor.write(angle);
  Serial.printf("ts: %.2f | tu: %.2f | Tmed: %.2f", ts, tu, Tmed);
  Serial.printf("Motor Angle: %.2f | I: %.2f | T: %.2f\n", angle, I, T);
}

// ============ MQTT ============

void connectToBroker() {
  while (!mqttClient.connected()) {
    Serial.println("Connecting to MQTT...");
    if (mqttClient.connect("ESP32-MEDIBOX")) {
      Serial.println("Connected to broker");

      mqttClient.subscribe("MEDIBOX-DATA-MAIN-ON-OFF");
      mqttClient.subscribe("MEDIBOX-DATA-SCH-ON");
      mqttClient.subscribe("MEDIBOX-DATA-MINIMUM-ANG");
      mqttClient.subscribe("MEDIBOX-DATA-CONTROL-FAC");
      mqttClient.subscribe("MEDIBOX-DATA-TMED");
      mqttClient.subscribe("MEDIBOX-DATA-TS");
      mqttClient.subscribe("MEDIBOX-DATA-TU");
    } else {
      Serial.print("MQTT connect failed: ");
      Serial.println(mqttClient.state());
      delay(3000);
    }
  }
}

void receiveCallback(char* topic, byte* payload, unsigned int length) {
  payload[length] = '\0';
  float value = atof((char*)payload);

  if (strcmp(topic, "MEDIBOX-DATA-MAIN-ON-OFF") == 0) {
    buzzerOn(payload[0] == '1');
  } else if (strcmp(topic, "MEDIBOX-DATA-SCH-ON") == 0) {
    if (payload[0] == 'N') {
      isScheduledON = false;
    } else {
      isScheduledON = true;
      scheduledOnTime = atol((char*)payload);
    }
  } else if (strcmp(topic, "MEDIBOX-DATA-MINIMUM-ANG") == 0) {
    minAngle = value;
  } else if (strcmp(topic, "MEDIBOX-DATA-CONTROL-FAC") == 0) {
    motorAngleFac = value;
  } else if (strcmp(topic, "MEDIBOX-DATA-TMED") == 0) {
    if (value > 0) Tmed = value;
  } else if (strcmp(topic, "MEDIBOX-DATA-TS") == 0) {
    if (value > 0) ts = value;
  } else if (strcmp(topic, "MEDIBOX-DATA-TU") == 0) {
    if (value > 0) tu = value;
  }

  Serial.printf("Received %s = %.2f\n", topic, value);
}

void buzzerOn(bool on) {
  if (on) tone(BUZZER_PIN, 256);
  else noTone(BUZZER_PIN);
}

unsigned long getTime() {
  timeClient.update();
  return timeClient.getEpochTime();
}

void checkSchedule() {
  if (isScheduledON) {
    unsigned long currentTime = getTime();
    if (currentTime > scheduledOnTime) {
      buzzerOn(true);
      isScheduledON = false;
      mqttClient.publish("MEDIBOX-DATA-MAIN-ON-OFF-ESP", "1");
      mqttClient.publish("MEDIBOX-DATA-SCH-ESP-ON", "0");
    }
  }
}

