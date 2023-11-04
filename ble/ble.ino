#include <SPIFFS.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>


#define SERVICE_UUID                         "018b53a5-84e5-7b55-9963-f3f664a89f50"
#define STATUS_CHARACTERISTIC_UUID           "abdf5f40-f3cf-4625-a2c7-12de46bc510e"
#define READINGS_CHARACTERISTIC_UUID         "8be38717-63c1-42fe-909e-0af64bfb4c8b"
#define SETTINGS_CHARACTERISTIC_UUID         "41659141-e392-4157-b33d-80269cf2dbe6"
#define SECURESETTINGS_CHARACTERISTIC_UUID   "799c188f-c60f-4a72-9a36-3284922198f0"
#define COMMAND_CHARACTERISTIC_UUID          "a831f156-f70d-479f-b11b-9c2a8e2203de"
#define DATA_OUT_CHARACTERISTIC_UUID         "07c769ae-9de2-4e07-bad4-dc4f52f98790"

BLECharacteristic *charStatus;
BLECharacteristic *charReadings;
BLECharacteristic *charSettings;
BLECharacteristic *charSecureSettings;
BLECharacteristic *charCommand;
BLECharacteristic *charDataOut;
State state = NOT_INITIALIZED;

bool saveWirelessSettings(const char* ssid, const char* pwd);
void setupWirelessAndNTP(const char* ssid, const char* pwd);
void updateState(State newState);

enum State {
  NOT_INITIALIZED = 0, // first run: have to setup wifi connection
  STARTING_UP, // have all required info, warming up
  INITIALIZED_IDLE, // initialized, wating for commands
  RESUMING_PROCESS, // Got an ongoing process, resuming after restart
  STARTING_PROCESS, // Startup sequesce for the process: weighing of raw meat, collecting process settings
  PROCESS_IN_PROGRESS, // Capturing data, waiting for target values to be reached
  PROCESS_FINISHED // Target values reached, informing 
}

class SettingsCallbackHandler: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) {
    charStatus->setValue("A");
  }
};

class CommandCallbackHandler: public BLECharacteristicCallbacks {
  bool parseString(const std::string& input, char buf_ssid[32], char buf_pwd[64]) {
    try {
      memset(buf_ssid, 0, 32);
      memset(buf_pwd, 0, 64);
      size_t end_ssid_len_pos = input.find(':', 3);
      size_t end_pwd_len_pos = input.find(':', end_ssid_len_pos + 1);
      std::string str_ssid_len = input.substr(3, end_ssid_len_pos - 3);
      std::string str_pwd_len = input.substr(end_ssid_len_pos + 1, end_pwd_len_pos - end_ssid_len_pos - 1);
      int ssid_len = std::stoi(str_ssid_len);
      int pwd_len = std::stoi(str_pwd_len);
      std::string ssid = input.substr(end_pwd_len_pos + 1, min(32, ssid_len)); // limit SSID to 32 chars
      std::string pwd = input.substr(end_pwd_len_pos + ssid_len + 1, min(64, pwd_len)); // and PWD to 64
      strcpy(buf_ssid, ssid.c_str());
      strcpy(buf_pwd, pwd.c_str());
      return true;
    } catch (...) {
      return false;
    }
  }

  void onWrite(BLECharacteristic* pCharacteristic) {
    std::string fullValue = pCharacteristic->getValue();
    size_t command_end_pos = fullValue.find(':', 0);
    std::string command;
    if (command_end_pos == -1) {
      command = fullValue;
    } else {
      command = fullValue.substr(0, command_end_pos);
    }

    if (command == "WN") {
      char ssid[32];
      char pwd[64];
      if (parseString(fullValue, ssid, pwd)) {
        saveWirelessSettings(ssid, pwd);

        Serial.printf("Trying to connect wifi with SSID: %s and password: %s\n", ssid, pwd);
        setupWirelessAndNTP(ssid, pwd);
      } else {
        pCharacteristic->setValue("ERR:1"); // Wrong format
      }
    }
  }
};

void createCharacteristics(BLEService* pService) {
  charStatus = pService->createCharacteristic(
    STATUS_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE
  );

  charReadings = pService->createCharacteristic(
    READINGS_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ
  );

  charSettings = pService->createCharacteristic(
    SETTINGS_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE
  );
  charSettings->setCallbacks(new SettingsCallbackHandler());

  charSecureSettings = pService->createCharacteristic(
    SECURESETTINGS_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );

  charCommand = pService->createCharacteristic(
    COMMAND_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  charCommand->setCallbacks(new CommandCallbackHandler());

  charDataOut = pService->createCharacteristic(
    DATA_OUT_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ
  );
}

bool readWirelessSettings() {
  File settings = SPIFFS.open("./settings", FILE_READ);
  if (!settings) {
    Serial.println("Settings can't be read");
    return false;
  }

  String ssid = settings.readStringUntil('\n');
  String pwd = settings.readStringUntil('\n');
  settings.close();

  setupWirelessAndNTP(ssid.c_str(), pwd.c_str());
}

bool saveWirelessSettings(const char* ssid, const char* pwd) {
  File settings = SPIFFS.open("./settings", FILE_WRITE);
  if (!settings) {
    Serial.println("Settings can't be written");
    return false;
  }

  settings.println(ssid);
  settings.println(pwd);

  settings.close();
}

void setupWirelessAndNTP(const char* ssid, const char* pwd) {
  const char* ntpServerName = "pool.ntp.org";
  const int utcOffset = 0; // UTC offset in seconds
  const long updateInterval = 60000; // Update time every minute (60000 milliseconds)
  WiFiUDP ntpUDP;
  NTPClient timeClient(ntpUDP, ntpServerName, utcOffset, updateInterval);

  WiFi.begin(ssid, pwd);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  // Initialize and set up NTP client
  timeClient.begin();
  timeClient.update();

  Serial.print("Time updated: ");
  Serial.println(timeClient.getFormattedTime());
}

void setup() {
  Serial.begin(115200);

  // BLE Stack setup
  BLEDevice::init("Biltong Box Mk I");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  
  createCharacteristics(pService);

  charStatus->setValue("X");
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.println("BLE Setup completed");

  // SPIFFS Setup
  SPIFFS.begin(true);
  if (SPIFFS.exists("settings")) {
    readWirelessSettings();
  }
}

void loop() {
  // put your main code here, to run repeatedly:
  delay(2000);
}