#include <M5StickCPlus.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

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

const unsigned long COUNTDOWN_DURATION_MS = 60000;
const unsigned long ERROR_DISPLAY_DURATION_MS = 5000;
const unsigned long RECEIVE_CALLBACK_DEBUG_INTERVAL_MS = 2000;
unsigned long countdownStartMillis = 0;
unsigned long remainingSeconds = 0;
volatile bool errorReceived = false;
bool showingError = false;
unsigned long errorStartMillis = 0;
esp_err_t recvCbResult = ESP_ERR_ESPNOW_NOT_INIT;
unsigned long lastRecvCbDebugMillis = 0;

unsigned long bPressStart = 0;
bool bHolding = false;
bool longPressTriggered = false;

void printMacLine(const char *label, const uint8_t *macAddress) {
  Serial.printf("%s%02X:%02X:%02X:%02X:%02X:%02X\n",
                label,
                macAddress[0], macAddress[1], macAddress[2],
                macAddress[3], macAddress[4], macAddress[5]);
}

void printPacketDetails(const uint8_t *data, size_t length) {
  Serial.printf("Length: %u\n", static_cast<unsigned>(length));
  Serial.print("Data: ");
  for (size_t i = 0; i < length; ++i) {
    const uint8_t value = data[i];
    Serial.write((value >= 32 && value <= 126) ? value : '.');
  }
  Serial.println();

  Serial.print("Raw:");
  for (size_t i = 0; i < length; ++i) {
    Serial.printf(" %02X", data[i]);
  }
  Serial.println();
}

void onDataSent(const uint8_t *macAddress, esp_now_send_status_t status) {
  Serial.printf("[%lu ms] TX callback\n", millis());
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Status: SUCCESS");
  } else {
    Serial.println("Status: FAILED");
  }
}

void onDataRecv(const uint8_t *macAddress, const uint8_t *incomingData, int len) {
  Serial.printf("[%lu ms] RX packet\n", millis());
  printMacLine("Sender MAC: ", macAddress);
  printPacketDetails(incomingData, static_cast<size_t>(len));

  if (len == 4 && memcmp(incomingData, "TEST", 4) == 0) {
    Serial.printf("[%lu ms] CONNECTION TEST PASSED\n", millis());
  }

  if (len == 5 && memcmp(incomingData, "ERROR", 5) == 0) {
    Serial.printf("[%lu ms] RX ERROR recognized\n", millis());
    Serial.println("State -> STANDBY");
    errorReceived = true;
  }
}

void sendCommand(const char *command) {
  Serial.printf("[%lu ms] TX %s\n", millis(), command);
  printMacLine("Target MAC: ", receiverMac);
  printPacketDetails(reinterpret_cast<const uint8_t *>(command), strlen(command));
  Serial.printf("Channel: %d\n", WiFi.channel());
  Serial.printf("Peer exists: %s\n",
                esp_now_is_peer_exist(receiverMac) ? "YES" : "NO");

  esp_err_t result = esp_now_send(
      receiverMac,
      reinterpret_cast<const uint8_t *>(command),
      strlen(command));

  Serial.printf("esp_now_send return: %d\n", result);

  if (result != ESP_OK) {
    Serial.printf("[%lu ms] ESP-NOW send error: %d\n", millis(), result);
  }
}

void sendTimestampedCommand(const char *eventName) {
  RTC_DateTypeDef date;
  RTC_TimeTypeDef time;
  M5.Rtc.GetDate(&date);
  M5.Rtc.GetTime(&time);

  char command[32] = {};
  snprintf(command, sizeof(command),
           "%s,%04u%02u%02u,%02u%02u%02u",
           eventName,
           date.Year, date.Month, date.Date,
           time.Hours, time.Minutes, time.Seconds);
  sendCommand(command);
}

bool isLeapYear(uint16_t year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint8_t daysInMonth(uint16_t year, uint8_t month) {
  static const uint8_t days[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if (month == 2 && isLeapYear(year)) {
    return 29;
  }

  return days[month - 1];
}

bool isValidRtcDate(const RTC_DateTypeDef &date) {
  if (date.Year < 2024 || date.Month < 1 || date.Month > 12) {
    return false;
  }

  return date.Date >= 1 && date.Date <= daysInMonth(date.Year, date.Month);
}

uint8_t getWeekDay(uint16_t year, uint8_t month, uint8_t day) {
  static const uint8_t monthOffsets[] = {
      0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

  if (month < 3) {
    --year;
  }

  return static_cast<uint8_t>(
      (year + year / 4 - year / 100 + year / 400 +
       monthOffsets[month - 1] + day) % 7);
}

bool getBuildDateTime(RTC_DateTypeDef *date, RTC_TimeTypeDef *time) {
  char monthName[4] = {};
  int day = 0;
  int year = 0;
  int hours = 0;
  int minutes = 0;
  int seconds = 0;

  if (sscanf(__DATE__, "%3s %d %d", monthName, &day, &year) != 3 ||
      sscanf(__TIME__, "%d:%d:%d", &hours, &minutes, &seconds) != 3) {
    return false;
  }

  static const char *monthNames[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  uint8_t month = 0;
  for (uint8_t i = 0; i < 12; ++i) {
    if (strcmp(monthName, monthNames[i]) == 0) {
      month = i + 1;
      break;
    }
  }

  if (month == 0 || year < 2024 || day < 1 ||
      day > daysInMonth(static_cast<uint16_t>(year), month) ||
      hours < 0 || hours > 23 || minutes < 0 || minutes > 59 ||
      seconds < 0 || seconds > 59) {
    return false;
  }

  date->Year = static_cast<uint16_t>(year);
  date->Month = month;
  date->Date = static_cast<uint8_t>(day);
  date->WeekDay = getWeekDay(date->Year, date->Month, date->Date);

  time->Hours = static_cast<uint8_t>(hours);
  time->Minutes = static_cast<uint8_t>(minutes);
  time->Seconds = static_cast<uint8_t>(seconds);
  return true;
}

void printRtcDateTime(const char *label,
                     const RTC_DateTypeDef &date,
                     const RTC_TimeTypeDef &time) {
  Serial.printf("%s%04u-%02u-%02u %02u:%02u:%02u\n",
                label,
                date.Year, date.Month, date.Date,
                time.Hours, time.Minutes, time.Seconds);
}

void initializeRtcFromBuildTime() {
  RTC_DateTypeDef currentDate = {};
  RTC_TimeTypeDef currentTime = {};
  M5.Rtc.GetDate(&currentDate);
  M5.Rtc.GetTime(&currentTime);
  printRtcDateTime("RTC current: ", currentDate, currentTime);

  if (isValidRtcDate(currentDate)) {
    Serial.println("RTC date valid; keeping current RTC.");
    return;
  }

  RTC_DateTypeDef buildDate = {};
  RTC_TimeTypeDef buildTime = {};
  if (!getBuildDateTime(&buildDate, &buildTime)) {
    Serial.println("RTC invalid, but compile date/time could not be parsed.");
    return;
  }

  M5.Rtc.SetDate(&buildDate);
  M5.Rtc.SetTime(&buildTime);
  Serial.println("RTC date invalid; initialized from compile date/time.");

  M5.Rtc.GetDate(&currentDate);
  M5.Rtc.GetTime(&currentTime);
  printRtcDateTime("RTC initialized: ", currentDate, currentTime);
}

int getBatteryPercentage() {
  const float batteryVoltage = M5.Axp.GetBatVoltage();
  const float minimumVoltage = 3.0f;
  const float maximumVoltage = 4.2f;

  if (batteryVoltage <= minimumVoltage) {
    return 0;
  }

  if (batteryVoltage >= maximumVoltage) {
    return 100;
  }

  const int percentage = static_cast<int>(
      ((batteryVoltage - minimumVoltage) /
       (maximumVoltage - minimumVoltage)) * 100.0f + 0.5f);

  if (percentage < 0) {
    return 0;
  }

  if (percentage > 100) {
    return 100;
  }

  return percentage;
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

  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(190, 10);
  M5.Lcd.printf("%d%%", getBatteryPercentage());
}

void showRecordError() {
  M5.Lcd.fillScreen(BLACK);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(10, 55);
  M5.Lcd.println("RECORD ERROR");
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
  initializeRtcFromBuildTime();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_err_t channelResult =
      esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[%lu ms] WiFi channel set result: %d\n",
                millis(), channelResult);
  Serial.println("WiFi Sleep: DISABLED");
  String m5Mac = WiFi.macAddress();
  m5Mac.toUpperCase();
  Serial.printf("[%lu ms] M5 STA MAC: %s\n", millis(), m5Mac.c_str());
  Serial.printf("[%lu ms] M5 WiFi Channel: %d\n", millis(), WiFi.channel());

  if (esp_now_init() != ESP_OK) {
    Serial.printf("[%lu ms] ESP-NOW init failed\n", millis());
  } else {
    Serial.printf("[%lu ms] ESP-NOW init OK\n", millis());
    esp_now_register_send_cb(onDataSent);
    recvCbResult = esp_now_register_recv_cb(onDataRecv);
    Serial.printf("Receive callback register result: %d\n", recvCbResult);
    if (recvCbResult == ESP_OK) {
      Serial.println("Receive callback: OK");
    } else {
      Serial.println("Receive callback: FAILED");
    }

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMac, 6);
    peerInfo.channel = 1;
    peerInfo.ifidx = WIFI_IF_STA;
    peerInfo.encrypt = false;

    printMacLine("Target MAC: ", receiverMac);
    Serial.printf("Peer channel: %d\n", peerInfo.channel);
    Serial.println("Peer interface: WIFI_IF_STA");

    if (esp_now_is_peer_exist(receiverMac)) {
      esp_err_t deleteResult = esp_now_del_peer(receiverMac);
      Serial.printf("Existing peer delete result: %d\n", deleteResult);
      if (deleteResult != ESP_OK && deleteResult != ESP_ERR_ESPNOW_NOT_FOUND) {
        Serial.println("ESP-NOW existing peer delete failed");
      }
    }

    esp_err_t peerResult = esp_now_add_peer(&peerInfo);
    Serial.printf("Peer add result: %d\n", peerResult);
    Serial.printf("Peer exists: %s\n",
                  esp_now_is_peer_exist(receiverMac) ? "YES" : "NO");

    if (peerResult != ESP_OK) {
      Serial.println("ESP-NOW peer add failed");
    }
  }

  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(BLACK);

  showNormal();
}

void loop() {
  M5.update();

  unsigned long now = millis();
  if (now - lastRecvCbDebugMillis >= RECEIVE_CALLBACK_DEBUG_INTERVAL_MS) {
    lastRecvCbDebugMillis = now;
    Serial.printf("Receive callback register result: %d\n", recvCbResult);
    if (recvCbResult == ESP_OK) {
      Serial.println("Receive callback: OK");
    } else {
      Serial.println("Receive callback: FAILED");
    }
  }

  if (errorReceived) {
    errorReceived = false;
    isRunning = false;
    remainingSeconds = 0;
    currentMode = NORMAL;
    showingError = true;
    errorStartMillis = millis();
    showRecordError();
  }

  if (showingError) {
    if (millis() - errorStartMillis >= ERROR_DISPLAY_DURATION_MS) {
      showingError = false;
      currentMode = NORMAL;
      showNormal();
    } else {
      return;
    }
  }

  // =====================
  // A 鍵
  // =====================
  if (M5.BtnA.wasPressed()) {

    // 正常模式：START / STOP
    if (currentMode == NORMAL) {
      if (isRunning) {
        isRunning = false;
        remainingSeconds = 0;
        sendTimestampedCommand("STOP");
      } else {
        isRunning = true;
        countdownStartMillis = millis();
        remainingSeconds = COUNTDOWN_DURATION_MS / 1000;
        sendTimestampedCommand("START");
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


