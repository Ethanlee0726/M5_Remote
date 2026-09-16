#include <M5StickCPlus.h>
#include <WiFi.h>
#include <esp_now.h>

RTC_TimeTypeDef RTC_TimeStruct;

enum Mode {
  NORMAL,
  SET_HOUR,
  SET_MINUTE
};

Mode currentMode = NORMAL;

bool isRunning = false;

// TODO: Replace with ESP32 Sensor MAC address
uint8_t receiverMac[] = {0xD0, 0xCF, 0x13, 0x41, 0x1B, 0x80};

const unsigned long COUNTDOWN_DURATION_MS = 30000;
unsigned long countdownStartMillis = 0;
unsigned long remainingSeconds = 0;

unsigned long bPressStart = 0;
bool bHolding = false;
bool longPressTriggered = false;

void onDataSent(const uint8_t *macAddress, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Send success");
  } else {
    Serial.println("Send failed");
  }
}

void sendCommand(const char *command) {
  Serial.print("Sending: ");
  Serial.println(command);

  esp_err_t result = esp_now_send(
      receiverMac,
      reinterpret_cast<const uint8_t *>(command),
      strlen(command));

  if (result != ESP_OK) {
    Serial.printf("ESP-NOW send error: %d\n", result);
  }
}

void showNormal() {
  M5.Rtc.GetTime(&RTC_TimeStruct);

  M5.Lcd.fillScreen(BLACK);

  // 時間
  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.printf("%02d:%02d:%02d",
                RTC_TimeStruct.Hours,
                RTC_TimeStruct.Minutes,
                RTC_TimeStruct.Seconds);

  // 狀態
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 55);
  M5.Lcd.print("STATUS: ");

  if (isRunning) {
    M5.Lcd.println("RUNNING");

    M5.Lcd.setCursor(10, 80);
    M5.Lcd.printf("REMAINING: %02lu s", remainingSeconds);

    M5.Lcd.setCursor(10, 105);
    M5.Lcd.println("BTN A: STOP");
  } else {
    M5.Lcd.println("STANDBY");

    M5.Lcd.setCursor(10, 80);
    M5.Lcd.println("BTN A: START");
  }
}

void showSetHour() {
  M5.Lcd.fillScreen(BLACK);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.println("SET HOUR");

  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(10, 45);
  M5.Lcd.printf("%02d:%02d",
                RTC_TimeStruct.Hours,
                RTC_TimeStruct.Minutes);
}

void showSetMinute() {
  M5.Lcd.fillScreen(BLACK);

  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 10);
  M5.Lcd.println("SET MIN");

  M5.Lcd.setTextSize(3);
  M5.Lcd.setCursor(10, 45);
  M5.Lcd.printf("%02d:%02d",
                RTC_TimeStruct.Hours,
                RTC_TimeStruct.Minutes);
}

void setup() {
  M5.begin();
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
  } else {
    esp_now_register_send_cb(onDataSent);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("ESP-NOW peer add failed");
    }
  }

  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(BLACK);

  showNormal();
}

void loop() {
  M5.update();

  // =====================
  // A 鍵
  // =====================
  if (M5.BtnA.wasPressed()) {

    // 正常模式：START / STOP
    if (currentMode == NORMAL) {
      if (isRunning) {
        isRunning = false;
        remainingSeconds = 0;
        sendCommand("STOP");
      } else {
        isRunning = true;
        countdownStartMillis = millis();
        remainingSeconds = COUNTDOWN_DURATION_MS / 1000;
        sendCommand("START");
      }

      showNormal();
    }

    // 設定小時
    else if (currentMode == SET_HOUR) {
      RTC_TimeStruct.Hours++;

      if (RTC_TimeStruct.Hours >= 24) {
        RTC_TimeStruct.Hours = 0;
      }

      showSetHour();
    }

    // 設定分鐘
    else if (currentMode == SET_MINUTE) {
      RTC_TimeStruct.Minutes++;

      if (RTC_TimeStruct.Minutes >= 60) {
        RTC_TimeStruct.Minutes = 0;
      }

      showSetMinute();
    }
  }

  // =====================
  // B 鍵按下
  // =====================
  if (M5.BtnB.wasPressed()) {
    bPressStart = millis();
    bHolding = true;
    longPressTriggered = false;
  }

  // =====================
  // B 長按
  // =====================
  if (bHolding && M5.BtnB.isPressed()) {

    if ((millis() - bPressStart >= 1000) &&
        !longPressTriggered) {

      longPressTriggered = true;

      // 正常模式才允許進入時間設定
      if (currentMode == NORMAL) {

        M5.Rtc.GetTime(&RTC_TimeStruct);

        currentMode = SET_HOUR;

        showSetHour();
      }
    }
  }

  // =====================
  // B 放開
  // =====================
  if (M5.BtnB.wasReleased()) {

    if (!longPressTriggered) {

      // 設定小時 → 設定分鐘
      if (currentMode == SET_HOUR) {

        currentMode = SET_MINUTE;

        showSetMinute();
      }

      // 設定分鐘 → 儲存
      else if (currentMode == SET_MINUTE) {

        RTC_TimeStruct.Seconds = 0;

        M5.Rtc.SetTime(&RTC_TimeStruct);

        currentMode = NORMAL;

        showNormal();
      }
    }

    bHolding = false;
  }

  // =====================
  // 每秒更新時間
  // =====================
  if (currentMode == NORMAL) {

    static unsigned long lastUpdate = 0;
    unsigned long now = millis();

    if (isRunning) {
      unsigned long elapsedMillis = now - countdownStartMillis;

      if (elapsedMillis >= COUNTDOWN_DURATION_MS) {
        isRunning = false;
        remainingSeconds = 0;
        showNormal();
        lastUpdate = now;
        return;
      }

      remainingSeconds = (COUNTDOWN_DURATION_MS - elapsedMillis + 999) / 1000;
    }

    if (now - lastUpdate >= 1000) {

      lastUpdate = now;

      showNormal();
    }
  }
}


