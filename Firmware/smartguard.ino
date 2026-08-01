#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_INA219.h>
#include <SoftwareSerial.h>

#define TINY_GSM_MODEM_SIM800   // MUST come before TinyGsmClient.h include

#include <TinyGsmClient.h>
#include <PubSubClient.h>

// ==========================================
// USER CONFIGURATIONS
// ==========================================
const char EMERGENCY_PHONE[] = "+256787833543"; 
const char INSTALLED_LOCATION[] = "Plot 12, Kampala Road, Kampala, Uganda"; 

// Dynamic Safety Thresholds
const int MQ2_THRESHOLD = 520;             
const int FLAME_DANGER_THRESHOLD = 100;    

// Pin Allocations
const int MQ2_PIN = A0;
const int ACS712_PIN = A1;
const int FLAME_ANALOG_PIN = A2; 
const int BUZZER_PIN = 8;
const int SHORT_BUTTON_PIN = 12; 

// Objects
LiquidCrystal_I2C lcd(0x27, 16, 2); 
Adafruit_INA219 ina219;
SoftwareSerial gsmSerial(2, 3); // RX, TX

// --- Cellular & MQTT Objects ---
TinyGsm modem(gsmSerial);
TinyGsmClient client(modem);
PubSubClient mqttClient(client);

// MTN UGANDA APN SETTINGS
const char apn[]  = "internet"; 
const char user[] = "";         
const char pass[] = "";         

// --- 2-TIER BROKER FALLBACK (trimmed from 4 to save flash) ---
const char* mqtt_servers[] = {
  "test.mosquitto.org",   // PRIMARY
  "broker.emqx.io"        // FALLBACK
};
const int NUM_BROKERS = 2;
int current_broker_index = 0;

const int mqtt_port = 1883;
const char* mqtt_topic_alerts = "smartguard/alerts";
const char* mqtt_topic_commands = "smartguard/commands";
const char* mqtt_client_id = "smartguard_kla_01";

// Component Online/Offline Flags
bool gsmOnline = false;
bool ina219Online = false;
bool mq2Online = false;
bool acs712Online = false;
bool flameOnline = false;

// Alert Logic
bool hazardDetected = false;
const char* hazardType = ""; 
unsigned long lastSensorReadTime = 0;
const unsigned long sensorInterval = 1000; 

bool smsSent = false;
bool callPlaced = false;
unsigned long hazardStartTime = 0;
unsigned long lastGsmErrorTime = 0;
const unsigned long GSM_ERROR_INTERVAL = 5000;

// Global sensor values for MQTT publishing
int currentGas = 0;
int currentFlame = 1023;
unsigned long lastMqttPublish = 0;

// NEW: how often to publish telemetry while system is SAFE (ms)
const unsigned long TELEMETRY_INTERVAL = 5000;

void setup() {
  Serial.begin(9600);
  gsmSerial.begin(9600);
  
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(SHORT_BUTTON_PIN, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(F("SYSTEM STARTUP")); 
  lcd.setCursor(0, 1);
  lcd.print(F("DIAGNOSTICS..."));
  delay(1500);

  runHardwareSelfTest();
  
  if (gsmOnline) {
    Serial.println(F("Initializing GPRS & MQTT..."));
    lcd.clear();
    lcd.print(F("Connecting GPRS..."));
    
    if (modem.gprsConnect(apn, user, pass) == 1) {
      Serial.println(F("[ OK ] GPRS Connected."));
      
      delay(3000); // let PDP context stabilize
      
      if (modem.isGprsConnected()) {
        Serial.println(F("[ OK ] GPRS data path verified."));
      } else {
        Serial.println(F("[WARN] GPRS connected but data path unclear."));
      }
      
      int signalQuality = checkSignalQuality();
      Serial.print(F("[INFO] Signal Quality (CSQ): "));
      Serial.println(signalQuality);
      
      if (signalQuality < 10) {
        Serial.println(F("[WARN] Weak signal - timeouts may occur"));
        lcd.setCursor(0, 1);
        lcd.print(F("Weak Signal!"));
        delay(2000);
      }
      
      mqttClient.setSocketTimeout(30);
      mqttClient.setServer(mqtt_servers[current_broker_index], mqtt_port);
      mqttClient.setCallback(mqttCallback);
      
      reconnectMQTT();
    } else {
      Serial.println(F("[ERR!] GPRS Connection Failed. Check Data Bundle."));
      lcd.setCursor(0, 1);
      lcd.print(F("GPRS Failed"));
    }
  }
}

void loop() {
  if (gsmOnline && !mqttClient.connected()) {
    reconnectMQTT();
  }
  if (gsmOnline) {
    mqttClient.loop();
  }

  unsigned long currentMillis = millis();

  if (currentMillis - lastSensorReadTime >= sensorInterval) {
    lastSensorReadTime = currentMillis;
    runDiagnosticLoop();
  }

  if (hazardDetected) {
    executeAlarmSequence(currentMillis);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    smsSent = false;
    callPlaced = false;
  }
}

// ==========================================
// SIGNAL QUALITY CHECK FUNCTION
// ==========================================
int checkSignalQuality() {
  gsmSerial.println(F("AT+CSQ"));
  delay(1000);
  
  String response = gsmSerial.readString();
  
  int csqIndex = response.indexOf("+CSQ:");
  if (csqIndex >= 0) {
    int commaIndex = response.indexOf(',', csqIndex);
    if (commaIndex > csqIndex) {
      String rssiStr = response.substring(csqIndex + 5, commaIndex);
      return rssiStr.toInt();
    }
  }
  return -1;
}

// ==========================================
// RIGOROUS HARDWARE SELF-TEST
// ==========================================
void runHardwareSelfTest() {
  Serial.println(F("\n=== BEGINNING HARDWARE DIAGNOSTIC CHECKS ==="));
  lcd.clear();
  lcd.print(F("Testing Sensors.."));

  if (ina219.begin()) {
    ina219Online = true;
    Serial.println(F("[ OK ] INA219: Communication established."));
  } else {
    ina219Online = false;
    Serial.println(F("[ERR!] INA219: Sensor NOT detected! Check I2C wiring."));
  }

  int rawMQ2 = analogRead(MQ2_PIN);
  int rawACS = analogRead(ACS712_PIN);
  int rawFlame = analogRead(FLAME_ANALOG_PIN);

  if (rawMQ2 > 10 && rawMQ2 < 1015) { mq2Online = true; Serial.println(F("[ OK ] MQ-2 Sensor: Wired & ready.")); } 
  else { mq2Online = false; Serial.println(F("[ERR!] MQ-2 Sensor: Faulty.")); }

  if (rawACS > 400 && rawACS < 600) { acs712Online = true; Serial.println(F("[ OK ] ACS712 Sensor: Wired & ready.")); } 
  else { acs712Online = false; Serial.println(F("[ERR!] ACS712 Sensor: Faulty.")); }

  if (rawFlame > 5 && rawFlame < 1020) { flameOnline = true; Serial.println(F("[ OK ] Flame Sensor: Wired & ready.")); } 
  else { flameOnline = false; Serial.println(F("[ERR!] Flame Sensor: Faulty.")); }

  Serial.println(F("Testing GSM SIM800L..."));
  lcd.clear();
  lcd.print(F("Checking GSM..."));
  
  gsmSerial.println(F("AT"));
  delay(1000);
  if (gsmSerial.available() > 0) {
    String response = gsmSerial.readString();
    if (response.indexOf("OK") != -1) {
      Serial.println(F("[ OK ] GSM module responding."));
      gsmSerial.println(F("AT+CPIN?"));
      delay(1000);
      response = gsmSerial.readString();
      if (response.indexOf("READY") != -1) {
        Serial.println(F("[ OK ] SIM card unlocked & ready."));
        gsmSerial.println(F("AT+CREG?"));
        delay(1000);
        response = gsmSerial.readString();
        if (response.indexOf("+CREG: 0,1") != -1 || response.indexOf("+CREG: 0,5") != -1) {
          gsmOnline = true;
          Serial.println(F("[ OK ] Network Registration: Connected."));
        } else {
          Serial.println(F("[ERR!] GSM: No network signal registration."));
        }
      } else {
        Serial.println(F("[ERR!] GSM: SIM Card missing or locked."));
      }
    }
  } else {
    Serial.println(F("[ERR!] GSM: No communication. Verify RX/TX Pins."));
  }

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Sensors: "));
  lcd.print(mq2Online && flameOnline && acs712Online ? F("OK") : F("ERR"));
  lcd.setCursor(0, 1);
  lcd.print(F("GSM Network: "));
  lcd.print(gsmOnline ? F("READY") : F("FAIL"));
  delay(3000);
  
  if (gsmOnline) {
    gsmSerial.println(F("AT+CMGF=1")); 
    delay(200);
    gsmSerial.println(F("AT+CLIP=1"));
    delay(200);
  }
}

// ==========================================
// REAL-TIME SENSOR DIAGNOSTIC ROUTINE
// ==========================================
void runDiagnosticLoop() {
  currentGas = 0;
  currentFlame = 1023; 

  Serial.print(F("SYS_DIAG: "));

  if (mq2Online) {
    currentGas = analogRead(MQ2_PIN);
    Serial.print(F("Gas: ")); Serial.print(currentGas); Serial.print(F(" | "));
  }

  if (flameOnline) {
    currentFlame = analogRead(FLAME_ANALOG_PIN);
    Serial.print(F("Flame: ")); Serial.print(currentFlame); Serial.print(F(" | "));
  }

  bool shortButtonPressed = (digitalRead(SHORT_BUTTON_PIN) == LOW);
  Serial.print(F("Short Circuit: "));
  Serial.println(shortButtonPressed ? F("YES") : F("NO"));

  if (flameOnline && currentFlame <= FLAME_DANGER_THRESHOLD) { 
    triggerHazard("FIRE");
  } else if (mq2Online && currentGas > MQ2_THRESHOLD) {
    triggerHazard("GAS/SMOKE");
  } else if (shortButtonPressed) {
    triggerHazard("SHORT CIRCUIT");
  } else {
    hazardDetected = false;
    hazardType = "";
    displaySafeStatus(currentGas);

    // NEW: publish periodic telemetry so the app can track readings
    // even when the system is in a SAFE state (not just during hazards)
    unsigned long now = millis();
    if (gsmOnline && (now - lastMqttPublish > TELEMETRY_INTERVAL)) {
      publishMQTTAlert("NORMAL", currentGas, currentFlame);
      lastMqttPublish = now;
    }
  }
}

// ==========================================
// ALARM DISPATCH SYSTEM
// ==========================================
void triggerHazard(const char* type) {
  if (!hazardDetected) {
    hazardStartTime = millis(); 
    publishMQTTAlert(type, currentGas, currentFlame); 
  }
  hazardDetected = true;
  hazardType = type;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("DANGER! DANGER!"));
  lcd.setCursor(0, 1);
  lcd.print(type);
  lcd.print(F(" DETECTED"));
}

void executeAlarmSequence(unsigned long currentMillis) {
  digitalWrite(BUZZER_PIN, HIGH);

  if (currentMillis - lastMqttPublish > 5000) {
    publishMQTTAlert(hazardType, currentGas, currentFlame);
    lastMqttPublish = currentMillis;
  }

  if (gsmOnline) {
    if (!smsSent) {
      sendSMSAlert();
      smsSent = true;
    }
    if (smsSent && !callPlaced && (currentMillis - hazardStartTime > 10000)) {
      placeEmergencyCall();
      callPlaced = true;
    }
  } else {
    if (currentMillis - lastGsmErrorTime > GSM_ERROR_INTERVAL) {
      Serial.println(F("[CRITICAL] Hazard active but GSM is OFFLINE!"));
      lastGsmErrorTime = currentMillis;
    }
  }
}

// ==========================================
// SCREEN UTILITIES
// ==========================================
void displaySafeStatus(int gas) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("SYS SAFE | G:"));
  if (mq2Online) lcd.print(gas);
  else lcd.print(F("NA"));
  lcd.setCursor(0, 1);
  lcd.print(F("SYS: OK"));
}

// ==========================================
// SMS AND DIALING ROUTINES
// ==========================================
void sendSMSAlert() {
  Serial.println(F("Sending Emergency SMS..."));
  gsmSerial.print(F("AT+CMGS=\""));
  gsmSerial.print(EMERGENCY_PHONE);
  gsmSerial.println(F("\""));
  delay(1000);
  gsmSerial.print(F("CRITICAL EMERGENCY ALERT!\nHazard: ")); 
  gsmSerial.print(hazardType); 
  gsmSerial.print(F(" detected.\nLocation: ")); 
  gsmSerial.print(INSTALLED_LOCATION); 
  gsmSerial.print(F("\nAction Required: Immediate Evacuation!"));
  delay(500);
  gsmSerial.write(26); 
  delay(3000);
  Serial.println(F("SMS Process complete."));
}

void placeEmergencyCall() {
  Serial.println(F("Placing Voice Call..."));
  gsmSerial.print(F("ATD"));
  gsmSerial.print(EMERGENCY_PHONE);
  gsmSerial.println(F(";")); 
  delay(100);
  Serial.println(F("Call initiated."));
}

// ==========================================
// MQTT FUNCTIONS (broker fallback + timeout fix)
// ==========================================
void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print(F("Attempting MQTT connection to "));
    Serial.println(mqtt_servers[current_broker_index]);
    
    mqttClient.setServer(mqtt_servers[current_broker_index], mqtt_port);
    
    if (mqttClient.connect(mqtt_client_id)) {
      Serial.println(F("connected!"));
      mqttClient.subscribe(mqtt_topic_commands, 1);
      lcd.setCursor(0, 1);
      lcd.print(F("MQTT Connected! "));
      current_broker_index = 0; 
    } else {
      Serial.print(F("failed, rc="));
      Serial.print(mqttClient.state());
      Serial.println(F(" switching broker..."));
      
      current_broker_index = (current_broker_index + 1) % NUM_BROKERS;
      delay(5000);
    }
  }
}

// NEW: manual JSON build (removes ArduinoJson dependency to save flash)
void publishMQTTAlert(const char* status, int gas, int flame) {
  if (!mqttClient.connected()) return;

  char jsonBuffer[128];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
           "{\"status\":\"%s\",\"gas\":%d,\"flame\":%d,\"timestamp\":%lu}",
           status, gas, flame, millis());

  if (mqttClient.publish(mqtt_topic_alerts, jsonBuffer)) {
    Serial.print(F("Published MQTT: "));
    Serial.println(jsonBuffer);
  }
}

// Listen for commands from the Flutter App
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print(F("MQTT Command Received: "));
  Serial.println(message);

  if (message.indexOf("silence_buzzer") >= 0) {
    Serial.println(F(">>> REMOTE SILENCE TRIGGERED <<<"));
    digitalWrite(BUZZER_PIN, LOW);
    hazardDetected = false;
    hazardType = "";
    smsSent = false;   
    callPlaced = false;
    
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("ALARM SILENCED"));
    lcd.setCursor(0, 1);
    lcd.print(F("Via Mobile App"));
    delay(3000);
    
    publishMQTTAlert("SILENCED", currentGas, currentFlame);
  }
}
