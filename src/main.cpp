#include <M5StickC.h>
#include <BluetoothSerial.h>
#include "imu/ImuReader.h"
#include "imu/AverageCalc.h"
#include "input/ButtonCheck.h"
#include "input/ButtonData.h"
#include "session/SessionData.h"
#include "prefs/Settings.h"

#define TASK_DEFAULT_CORE_ID 1
#define TASK_STACK_DEPTH 4096UL
#define TASK_NAME_IMU "IMUTask"
#define TASK_NAME_SESSION "SessionTask"
#define TASK_NAME_BUTTON "ButtonTask"
#define TASK_SLEEP_IMU 5 // = 1000[ms] / 200[Hz]
#define TASK_SLEEP_SESSION 40 // = 1000[ms] / 25[Hz]
#define TASK_SLEEP_BUTTON 1 // = 1000[ms] / 1000[Hz]
#define MUTEX_DEFAULT_WAIT 1000UL

static void ImuLoop(void* arg);
static void SessionLoop(void* arg);
static void ButtonLoop(void* arg);

imu::ImuReader* imuReader;
BluetoothSerial btSpp;
input::ButtonCheck button;

imu::ImuData imuData;
input::ButtonData btnData;
bool hasButtonUpdate = false;
static SemaphoreHandle_t imuDataMutex = NULL;
static SemaphoreHandle_t btnDataMutex = NULL;

bool gyroOffsetInstalled = false;
imu::AverageCalcXYZ gyroAve;
prefs::Settings settingPref;

void UpdateLcd() {
  M5.Lcd.setCursor(40, 0);
  if (gyroOffsetInstalled) {
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.println("AxisOrange");
  } else {
    M5.Lcd.fillScreen(GREEN);
    M5.Lcd.println("GyroOffset");
  }
}

void setup() {
  M5.begin();
  // read settings
  float gyroOffset[3] = { 0.0F };
  settingPref.begin();
  //settingPref.clear(); // to reinstall gyro offset by only m5stickc remove commentout
  gyroOffsetInstalled = settingPref.readGyroOffset(gyroOffset);
  settingPref.finish();
  // lcd
  M5.Lcd.setRotation(3);
  M5.Lcd.setTextSize(1);
  UpdateLcd();
  // imu
  imuReader = new imu::ImuReader(M5.Imu);
  imuReader->initialize();
  if (gyroOffsetInstalled) {
    imuReader->writeGyroOffset(gyroOffset[0], gyroOffset[1], gyroOffset[2]);
  }
  // bluetooth serial
  btSpp.begin("AxisOrange");
  // task
  imuDataMutex = xSemaphoreCreateMutex();
  btnDataMutex = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(ImuLoop, TASK_NAME_IMU, TASK_STACK_DEPTH, 
    NULL, 2, NULL, TASK_DEFAULT_CORE_ID);
  xTaskCreatePinnedToCore(SessionLoop, TASK_NAME_SESSION, TASK_STACK_DEPTH, 
    NULL, 1, NULL, TASK_DEFAULT_CORE_ID);
  xTaskCreatePinnedToCore(ButtonLoop, TASK_NAME_BUTTON, TASK_STACK_DEPTH, 
    NULL, 1, NULL, TASK_DEFAULT_CORE_ID);
}

void loop() { /* Do Nothing */ }

static void ImuLoop(void* arg) {
  while (1) {
    uint32_t entryTime = millis();
    if (xSemaphoreTake(imuDataMutex, MUTEX_DEFAULT_WAIT) == pdTRUE) {
      imuReader->update();
      imuReader->read(imuData);
      if (!gyroOffsetInstalled) {
        if (!gyroAve.push(imuData.gyro[0], imuData.gyro[1], imuData.gyro[2])) {
          float x = gyroAve.averageX();
          float y = gyroAve.averageY();
          float z = gyroAve.averageZ();
          // set offset
          imuReader->writeGyroOffset(x, y, z);
          // save offset
          float offset[] = {x, y, z};
          settingPref.begin();
          settingPref.writeGyroOffset(offset);
          settingPref.finish();
          gyroOffsetInstalled = true;
          gyroAve.reset();
          UpdateLcd();
        }
      }
    }
    xSemaphoreGive(imuDataMutex);
    // idle
    int32_t sleep = TASK_SLEEP_IMU - (millis() - entryTime);
    vTaskDelay((sleep > 0) ? sleep : 0);
  }
}

static void SessionLoop(void* arg) {
  static session::SessionData imuSessionData(session::DataDefineImu);
  static session::SessionData btnSessionData(session::DataDefineButton);
  while (1) {
    uint32_t entryTime = millis();
    if (gyroOffsetInstalled) {
      // imu
      if (xSemaphoreTake(imuDataMutex, MUTEX_DEFAULT_WAIT) == pdTRUE) {
        imuSessionData.write((uint8_t*)&imuData, imu::ImuDataLen);
        btSpp.write((uint8_t*)&imuSessionData, imuSessionData.length());
      }
      xSemaphoreGive(imuDataMutex);
      // button
      if (xSemaphoreTake(btnDataMutex, MUTEX_DEFAULT_WAIT) == pdTRUE) {
        if (hasButtonUpdate) {
          btnSessionData.write((uint8_t*)&btnData, input::ButtonDataLen);
          btSpp.write((uint8_t*)&btnSessionData, btnSessionData.length());
          hasButtonUpdate = false;
        }
      }
      xSemaphoreGive(btnDataMutex);
    }
    // idle
    int32_t sleep = TASK_SLEEP_SESSION - (millis() - entryTime);
    vTaskDelay((sleep > 0) ? sleep : 0);
  }
}

static void ButtonLoop(void* arg) {
  uint8_t btnFlag = 0;
  while (1) {
    uint32_t entryTime = millis();
    M5.update();
    if (button.containsUpdate(M5, btnFlag)) {
        for (int i = 0; i < INPUT_BTN_NUM; i++) {
          if (xSemaphoreTake(btnDataMutex, MUTEX_DEFAULT_WAIT) == pdTRUE) {
            btnData.timestamp = millis();
            btnData.btnBits = btnFlag;
            hasButtonUpdate = true;
          }
          xSemaphoreGive(btnDataMutex);
        }
    }
    // idle
    int32_t sleep = TASK_SLEEP_BUTTON - (millis() - entryTime);
    vTaskDelay((sleep > 0) ? sleep : 0);
  }
}
