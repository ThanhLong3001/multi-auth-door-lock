#include "FingerprintDoor.h"

// =========================================================================
// CẤU HÌNH MẠNG VÀ BLYNK
// =========================================================================
char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = "Xiaomi_24CC";
char pass[] = "18022005";

// Cấu hình NTP Client để lấy thời gian thực (Múi giờ GMT+7)
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600);

// Định nghĩa các Virtual Pin trên giao diện Blynk
#define VPIN_LOCK_STATUS   V0 // Trạng thái khóa cửa
#define VPIN_LOG           V1 // Widget Terminal hiển thị lịch sử
#define VPIN_REMOTE_UNLOCK V2 // Nút nhấn mở khóa từ xa
#define VPIN_DUMP_LOG      V3 // Nút nhấn yêu cầu xuất lịch sử ra Terminal

BlynkTimer timer;
void checkBlynkConnection();

// Cấu hình bộ nhớ lưu trữ lịch sử mở cửa (tối đa 50 bản ghi)
Preferences prefs_log;
const int MAX_LOG_ENTRIES = 50;

// =========================================================================
// CẤU HÌNH PHẦN CỨNG (PINOUT & ĐỊA CHỈ)
// =========================================================================
#define RELAY_PIN   0    // Chân điều khiển Relay mở khóa
#define RED_LED     26   // LED báo trạng thái lỗi/sai
#define BUZZER_PIN  27   // Còi chíp báo động/thông báo

// LCD I2C
#define LCD_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// Keypad ma trận 4x4 giao tiếp qua IC PCF8574 (I2C)
#define PCF_KEYPAD_ADDR 0x20
const char keymap[4][4] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
const uint8_t ROW_PINS[4] = {0,1,2,3};
const uint8_t COL_PINS[4] = {4,5,6,7};

// Cảm biến RFID RC522 (Giao tiếp SPI)
#define SS_PIN    5
#define RST_PIN   4
#define SCK_PIN   18
#define MOSI_PIN  23
#define MISO_PIN  19
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Cảm biến vân tay (Giao tiếp UART2)
#define FINGERPRINT_RX 16
#define FINGERPRINT_TX 17
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// =========================================================================
// CÁC BIẾN TOÀN CỤC & TRẠNG THÁI HỆ THỐNG
// =========================================================================
const int MAX_LEN = 4;
unsigned long UNLOCK_TIME_MS = 3000; // Thời gian mở khóa (3 giây)
unsigned long lastFingerScanMs = 0;
unsigned long lastRfidScanMs = 0;
const unsigned long SENSOR_SCAN_INTERVAL = 250; // Chu kỳ quét cảm biến

Preferences prefs_rfid;        
Preferences prefs_cfg;         
String MASTER_PIN = "1234";    // Mật khẩu mặc định
const int MAX_UIDS = 50;       // Số lượng thẻ RFID tối đa có thể lưu

// Máy trạng thái (State Machine) của màn hình/menu
enum SystemState {
  STATE_NORMAL,             // Màn hình chính
  STATE_ADMIN_LOGIN,        // Nhập mã PIN Admin
  STATE_ADMIN_MENU,         // Menu quản trị
  STATE_CHANGE_PASS,        // Đổi mã PIN
  STATE_MANAGE_RFID_MENU,   // Menu quản lý RFID
  STATE_RFID_ADD,           // Thêm thẻ RFID
  STATE_RFID_DELETE,        // Xóa thẻ RFID
  STATE_MANAGE_FINGER_MENU, // Menu quản lý vân tay
  STATE_FINGER_ADD,         // Thêm vân tay
  STATE_FINGER_DELETE       // Xóa vân tay
};
SystemState currentState = STATE_NORMAL;

// Biến lưu trữ thao tác phím và trạng thái mở cửa
String inputBuffer = "";
bool keyIsDown = false;
int lastStableRaw = -1;
bool adminArmed = false; 
bool isUnlocked = false;
unsigned long unlockDeadline = 0;
int failCount = 0; // Đếm số lần nhập sai liên tiếp

// Cấu hình thời gian chờ tắt màn hình và reset nhập PIN
unsigned long lastActionMs = 0;
bool lcdIsOn = true;
const unsigned long LCD_TIMEOUT = 15000; 

unsigned long pinInputStartMs = 0; 
const unsigned long PIN_INPUT_TIMEOUT = 10000;     

// =========================================================================
// KHAI BÁO HÀM (FUNCTION PROTOTYPES)
// =========================================================================
void showHomeScreen();
void displayMessage(String line1, String line2, int delay_ms);
void displayAdminMenu();
void displayManageRFIDMenu();
void displayManageFingerMenu();
void showMaskedInput();
void handleKeypad();
void processKeyPress(char key);
void handleConfirmPress();
void unlockDoor(String msg, bool isRfid = false); 
void lockDoor();
void wakeLCD();
void manage_RFID_add();
void saveUnlockLog(String uid);

static inline void pcfWrite(uint8_t b) { Wire.beginTransmission(PCF_KEYPAD_ADDR); Wire.write(b); Wire.endTransmission(); }
static inline uint8_t pcfRead() { Wire.requestFrom((int)PCF_KEYPAD_ADDR, 1); return Wire.available() ? Wire.read() : 0xFF; }
int scanKeyRaw();
char rawToChar(int raw);

String readCard();
bool isUIDStored(const String &uid);
void addUID(const String &uid);
void deleteUIDByIndex(int index);
String uidToHexString(byte *uid, byte len);

uint8_t getFingerprintEnroll(uint8_t id);
int getFingerprintIDez();
uint8_t deleteFingerprint(uint8_t id);

// =========================================================================
// CÁC HÀM TIỆN ÍCH (CẢNH BÁO, ÂM THANH)
// =========================================================================

// Phát tiếng bíp ngắn
void beepShort(int ms = 200) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

// Báo hiệu sai mã/thẻ/vân tay 1 lần
void indicateFailOnce() {
  digitalWrite(RED_LED, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(250);
  digitalWrite(BUZZER_PIN, LOW);
  delay(250);
  digitalWrite(RED_LED, LOW);
}

// Kích hoạt báo động 10 giây khi sai 3 lần liên tiếp
void triggerAlarm10s() {
  Serial.println("🚨 Sai 3 lan! Bao dong 10s!");
  if (Blynk.connected()) {
    Blynk.logEvent("pin_fail_alarm", "Canh bao: Nhap sai PIN 3 lan!"); 
  }
  lcd.clear(); lcd.print("Bao dong 10s!");
  digitalWrite(RED_LED, HIGH);
  unsigned long t0 = millis();
  
  // Vòng lặp báo động, giữ kết nối Blynk và Watchdog
  while (millis() - t0 < 10000) {
    Blynk.run(); 
    digitalWrite(BUZZER_PIN, HIGH);
    esp_task_wdt_reset();
    handleKeypad(); 
    delay(10);
  }
  
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(RED_LED, LOW);
  failCount = 0; 
  showHomeScreen();
}

// =========================================================================
// HÀM SETUP (KHỞI TẠO HỆ THỐNG)
// =========================================================================
void fingerprintDoorSetup() {
  Serial.begin(115200);
  Wire.begin();

  // Khởi tạo các chân IO
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  // Khởi tạo LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.print("He thong khoi dong");
  lcd.setCursor(0, 1);
  lcd.print("Dang ket noi WiFi...");
  
  // Khởi tạo WiFi, Blynk và NTP
  WiFi.begin(ssid, pass);
  Blynk.config(auth); 
  timer.setInterval(5000L, checkBlynkConnection); // Định kỳ kiểm tra kết nối
  timeClient.begin();

  // Cấu hình Watchdog Timer (Reset ESP nếu bị treo quá 10s)
  const esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 10000,
      .idle_core_mask = 0,
      .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  pcfWrite(0xFF); // Khởi tạo I2C Keypad

  // Khởi tạo RFID (SPI)
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max); 
  SPI.setFrequency(1000000); 

  // Khởi tạo bộ nhớ NVS (Preferences)
  prefs_rfid.begin("rfid", false);
  prefs_cfg.begin("cfg",   false);
  prefs_log.begin("log_unlock", false);
  MASTER_PIN = prefs_cfg.getString("masterPIN", "1234"); // Đọc mã PIN lưu trữ

  // Khởi tạo cảm biến vân tay
  mySerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  finger.begin(57600);
  if (!finger.verifyPassword()) {
    Serial.println("WARNING: Khong tim thay cam bien van tay!");
    lcd.clear(); lcd.print("Loi cam bien VT");
    delay(900);
  }

  showHomeScreen();
  lastActionMs = millis();
  lcdIsOn = true;
  
  // Chờ và hiển thị trạng thái kết nối WiFi
  int wifi_retries = 10;
  while(WiFi.status() != WL_CONNECTED && wifi_retries > 0) {
    delay(500);
    wifi_retries--;
  }
  if(WiFi.status() == WL_CONNECTED) {
    displayMessage("WiFi Connected", "", 500);
  } else {
    displayMessage("WiFi Failed", "Offline Mode", 500);
  }
}

// =========================================================================
// HÀM LOOP (VÒNG LẶP CHÍNH)
// =========================================================================
void fingerprintDoorLoop() {
  Blynk.run();
  timer.run();
  esp_task_wdt_reset(); // Reset Watchdog liên tục để tránh reboot

  // Kiểm tra và tự động khóa cửa khi hết thời gian
  if (isUnlocked && millis() > unlockDeadline) {
    lockDoor();
  }

  // Quét cảm biến (Vân tay, RFID) khi đang ở màn hình chính
  if (currentState == STATE_NORMAL) {
    
    // 1. Quét Vân tay
    if (millis() - lastFingerScanMs > SENSOR_SCAN_INTERVAL) {
      lastFingerScanMs = millis();
      int fingerID = getFingerprintIDez();
      if (fingerID > 0) { // Vân tay đúng
        wakeLCD();
        Serial.printf("Fingerprint OK (ID=%d)\n", fingerID);
        unlockDoor("Van tay", false);
        String fingerprintLog = "Van tay ID: " + String(fingerID);
        saveUnlockLog(fingerprintLog); // Ghi log
        adminArmed = false;
      } else if (fingerID == 0) { // Vân tay sai
        wakeLCD();
        Serial.println("Fingerprint not matched!");
        displayMessage("Van tay khong hop le", "", 600);
        failCount++;
        indicateFailOnce();
        if (failCount >= 5) { triggerAlarm10s(); } // Cảnh báo quá 5 lần sai vân tay
      }
    }

    // 2. Quét thẻ RFID
    if (millis() - lastRfidScanMs > SENSOR_SCAN_INTERVAL + 50) {
      lastRfidScanMs = millis();
      String uid = readCard();
      if (uid != "") {
        if (isUIDStored(uid)) { // Thẻ hợp lệ
          wakeLCD();
          Serial.printf("RFID OK: %s\n", uid.c_str());
          unlockDoor(uid, true);
          adminArmed = false;
        } else { // Thẻ không hợp lệ
          displayMessage("The khong hop le", "", 800);
          indicateFailOnce();
        }
      }
    }
  }

  handleKeypad(); // Quét bàn phím liên tục

  // Phục hồi bus I2C nếu Keypad bị treo
  Wire.beginTransmission(PCF_KEYPAD_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("⚠️ I2C keypad not responding, reinit bus...");
    Wire.begin();
    pcfWrite(0xFF);
  }

  // Tự động tắt đèn nền LCD sau LCD_TIMEOUT
  if (lcdIsOn && millis() - lastActionMs > LCD_TIMEOUT) {
    if (currentState != STATE_NORMAL) {
      showHomeScreen(); // Thoát menu nếu đang dở dang
    }
    lcd.noBacklight();
    lcdIsOn = false;
  }
}

// =========================================================================
// HÀM HIỂN THỊ LCD
// =========================================================================
void showHomeScreen() {
  inputBuffer = "";
  currentState = STATE_NORMAL;
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Xin moi xac thuc");
  lcd.setCursor(0,1); lcd.print("PIN:");
  wakeLCD();
}

void wakeLCD() {
  lastActionMs = millis();
  if (!lcdIsOn) {
    lcd.backlight();
    lcdIsOn = true;
  }
}

void displayMessage(String line1, String line2, int delay_ms) {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print(line1);
  lcd.setCursor(0,1); lcd.print(line2);
  wakeLCD();

  // Custom delay để không chặn Blynk và Watchdog
  unsigned long start = millis();
  while (millis() - start < (unsigned long)delay_ms) {
    Blynk.run(); 
    handleKeypad();  
    esp_task_wdt_reset();
    delay(5);
  }

  if (!isUnlocked && currentState == STATE_NORMAL) showHomeScreen();
}

void showMaskedInput() {
  const int startCol = 5;
  lcd.setCursor(startCol, 1);
  String masked = "";
  for (size_t i = 0; i < inputBuffer.length(); i++) masked += "*";
  lcd.print(masked);
  
  // Xóa các ký tự thừa phía sau
  int remain = 16 - startCol - masked.length();
  while (remain-- > 0) lcd.print(' ');
}

// =========================================================================
// HÀM ĐIỀU KHIỂN CỬA (RELAY)
// =========================================================================
void unlockDoor(String msg, bool isRfid) {
  displayMessage("Xac thuc OK!", "", 250); 

  // Gửi thông báo và log lên Blynk
  if (Blynk.connected()) {
    Blynk.virtualWrite(VPIN_LOCK_STATUS, 1); 
    String logMessageBase = isRfid ? ("RFID UID: " + msg) : msg;
    String liveLog = logMessageBase;

    // Lấy thời gian từ NTP để dán nhãn (Timestamp)
    timeClient.update();
    if (timeClient.isTimeSet()) {
        int currentHour = timeClient.getHours();
        int currentMinute = timeClient.getMinutes();
        unsigned long utcEpochTime = timeClient.getEpochTime();
        time_t localEpochTime = utcEpochTime + (7 * 3600);
        struct tm *ptm = gmtime(&localEpochTime);

        int currentDay = ptm->tm_mday;
        int currentMonth = ptm->tm_mon + 1;
        int currentYear = ptm->tm_year + 1900;

        char timestampBuffer[20];
        snprintf(timestampBuffer, sizeof(timestampBuffer), "%02d-%02d-%04d %02d:%02d", 
                 currentDay, currentMonth, currentYear, currentHour, currentMinute);
        liveLog = String(timestampBuffer) + " " + logMessageBase;
    } else {
        Serial.println("CẢNH BÁO: Chưa lấy được giờ NTP, gửi log không có timestamp.");
    }
    
    Blynk.virtualWrite(VPIN_LOG, liveLog + "\n");
    if (isRfid) {
      Blynk.logEvent("rfid_unlock"); 
      saveUnlockLog(logMessageBase);
    }
  }

  // Kích hoạt Relay mở cửa
  digitalWrite(RELAY_PIN, HIGH);       
  digitalWrite(GREEN_LED, HIGH);       
  isUnlocked = true;
  unlockDeadline = millis() + UNLOCK_TIME_MS;
  displayMessage("CUA DA MO", "Xin moi vao", UNLOCK_TIME_MS - 150);
  digitalWrite(GREEN_LED, LOW);
  failCount = 0; // Đặt lại bộ đếm lỗi
}

void lockDoor() {
  digitalWrite(RELAY_PIN, LOW); // Tắt Relay        
  isUnlocked = false;
  Serial.println("Door locked");
  showHomeScreen();
  
  if (Blynk.connected()) {
    Blynk.virtualWrite(VPIN_LOCK_STATUS, 0); 
  }
  
  // Khởi tạo lại RFID để tránh treo module do nhiễu từ Relay
  Serial.println("Re-initializing RFID module...");
  mfrc522.PCD_Init();
  mfrc522.PCD_SetAntennaGain(mfrc522.RxGain_max); 
}

// =========================================================================
// HÀM XỬ LÝ KEYPAD (I2C PCF8574)
// =========================================================================
int scanKeyRaw() {
  uint8_t val = 0xFF;
  for (uint8_t r = 0; r < 4; r++) {
    val = 0xFF & ~(1 << ROW_PINS[r]);
    pcfWrite(val);
    delayMicroseconds(100);  
    uint8_t in = pcfRead();
    for (uint8_t c = 0; c < 4; c++) {
      if (!(in & (1 << COL_PINS[c]))) {
        pcfWrite(0xFF);
        delayMicroseconds(100);
        return r * 4 + c; // Trả về tọa độ phím
      }
    }
  }
  pcfWrite(0xFF);
  return -1;
}

char rawToChar(int raw) {
  if (raw < 0) return 0;
  return keymap[raw / 4][raw % 4];
}

// Chống dội phím (Debounce) và nhận diện phím nhấn
void handleKeypad() {
  static unsigned long lastChangeMs = 0;
  static int currRaw = -1;
  int raw = scanKeyRaw();
  
  if (raw != currRaw) {
    currRaw = raw;
    lastChangeMs = millis();
  }
  if (millis() - lastChangeMs > 70) {
    if (!keyIsDown && currRaw >= 0) {
      keyIsDown = true;
      lastStableRaw = currRaw;
      char k = rawToChar(lastStableRaw);
      processKeyPress(k); // Gọi hàm xử lý logic phím
    } else if (keyIsDown && currRaw < 0) {
      keyIsDown = false;
      lastStableRaw = -1;
    }
  }
  
  // Xóa buffer nếu nhập dở dang quá lâu
  if ((currentState == STATE_NORMAL || currentState == STATE_ADMIN_LOGIN) &&
      inputBuffer.length() > 0 &&
      millis() - pinInputStartMs > PIN_INPUT_TIMEOUT) {
    inputBuffer = "";
    showHomeScreen();
  }
}

// Xử lý logic khi một phím cụ thể được nhấn tùy theo State hiện tại
void processKeyPress(char key) {
  wakeLCD();
  Serial.print("Key: "); Serial.println(key);

  // Phím '*': Nút xóa/quay lại
  if (key == '*') {
    if ((currentState == STATE_FINGER_ADD || currentState == STATE_FINGER_DELETE) && inputBuffer.length() > 0) {
      inputBuffer.remove(inputBuffer.length() - 1);
      lcd.setCursor(0, 1); lcd.print("                ");  
      lcd.setCursor(0, 1); lcd.print(inputBuffer);        
      return;
    }
    if (currentState == STATE_NORMAL) {
      adminArmed = true; // Sẵn sàng vào chế độ Admin
      lcd.setCursor(0,0); lcd.print("Nhan # de vao  ");
      lcd.setCursor(0,1); lcd.print("Che do Admin    ");
    } else if (currentState != STATE_ADMIN_LOGIN) {
      // Logic quay lại menu trước đó
      if (currentState == STATE_MANAGE_RFID_MENU || currentState == STATE_MANAGE_FINGER_MENU || currentState == STATE_CHANGE_PASS) {
        currentState = STATE_ADMIN_MENU;
        displayAdminMenu();
      } else if (currentState == STATE_RFID_ADD || currentState == STATE_RFID_DELETE) {
        currentState = STATE_MANAGE_RFID_MENU;
        displayManageRFIDMenu();
      } else if (currentState == STATE_FINGER_ADD || currentState == STATE_FINGER_DELETE) {
        currentState = STATE_MANAGE_FINGER_MENU;
        displayManageFingerMenu();
      } else {
        showHomeScreen();
      }
    }
    return;
  }

  // Phím '#': Nút Xác nhận (Enter)
  if (key == '#') {
    if (adminArmed && currentState == STATE_NORMAL) {
      adminArmed = false;
      currentState = STATE_ADMIN_LOGIN;
      inputBuffer = "";
      lcd.clear();
      lcd.setCursor(0,0); lcd.print("Che do Admin");
      lcd.setCursor(0,1); lcd.print("PIN: ");
      return;
    }
    handleConfirmPress();
    return;
  }

  // Phím số 0-9
  if (key >= '0' && key <= '9') {
    if (currentState == STATE_FINGER_ADD || currentState == STATE_FINGER_DELETE) {
      if (inputBuffer.length() < 3) { // Nhập ID vân tay (max 3 số)       
        inputBuffer += key;
        lcd.setCursor(0,1); lcd.print("                ");        
        lcd.setCursor(0,1); lcd.print(inputBuffer);              
      }
      return;
    }
    if (currentState == STATE_NORMAL || currentState == STATE_ADMIN_LOGIN || currentState == STATE_CHANGE_PASS) {
      if (inputBuffer.length() == 0) pinInputStartMs = millis();
      if ((int)inputBuffer.length() < MAX_LEN) {
        inputBuffer += key;
        showMaskedInput();
        pinInputStartMs = millis();
        
        // Tự động xác thực khi đủ số ký tự
        if (currentState == STATE_NORMAL && inputBuffer.length() == MAX_LEN) {
          if (inputBuffer.equals(MASTER_PIN)) {
            unlockDoor("PIN", false);
            saveUnlockLog("PIN"); 
          } else {
            failCount++;
            String triesLeftMsg = "Con lai " + String(3 - failCount) + " lan";
            displayMessage("Sai PIN!", triesLeftMsg, 600);
            indicateFailOnce();
            if (failCount >= 3) { triggerAlarm10s(); }
          }
          inputBuffer = "";
        } else if (currentState == STATE_ADMIN_LOGIN && inputBuffer.length() == MAX_LEN) {
          if (inputBuffer.equals(MASTER_PIN)) {
            failCount = 0;
            currentState = STATE_ADMIN_MENU;
            displayAdminMenu();
          } else {
            failCount++;
            String triesLeftMsg = "Con lai " + String(3 - failCount) + " lan";
            displayMessage("Sai PIN!", triesLeftMsg, 600);
            indicateFailOnce();
            if (failCount >= 3) { triggerAlarm10s(); }
            showHomeScreen();
          }
          inputBuffer = "";
        }
      }
      return; 
    }
  }

  // Phím điều hướng trong Menu
  if (currentState == STATE_ADMIN_MENU) {
    if (key == '1') {
      currentState = STATE_MANAGE_FINGER_MENU; displayManageFingerMenu();
    } else if (key == '2') {
      currentState = STATE_MANAGE_RFID_MENU; displayManageRFIDMenu();
    } else if (key == '3') {
      currentState = STATE_CHANGE_PASS; inputBuffer = "";
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Doi PIN");
      lcd.setCursor(0,1); lcd.print("PIN: ");
    }
    return;
  }

  if (currentState == STATE_MANAGE_RFID_MENU) {
    if (key == '1') {
      currentState = STATE_RFID_ADD; manage_RFID_add();
    } else if (key == '2') {
      currentState = STATE_RFID_DELETE; inputBuffer = "";
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Xoa The RFID");
      lcd.setCursor(0,1); lcd.print("STT: ");
    }
    return;
  }

  if (currentState == STATE_MANAGE_FINGER_MENU) {
    if (key == '1') {
      currentState = STATE_FINGER_ADD; inputBuffer = "";
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Them Van Tay(ID)");
      lcd.setCursor(0,1); lcd.print("                "); 
      return;
    } else if (key == '2') {
      currentState = STATE_FINGER_DELETE; inputBuffer = "";
      lcd.clear(); lcd.setCursor(0,0); lcd.print("Xoa Van Tay(ID)");
      lcd.setCursor(0,1); lcd.print("                "); 
      return;
    }
  }

  inputBuffer += key;
  showMaskedInput();
}

// Xử lý logic khi nhấn phím '#' (Xác nhận)
void handleConfirmPress() {
  wakeLCD();
  switch (currentState) {
    case STATE_NORMAL: {
      if (inputBuffer.equals(MASTER_PIN)) {
        unlockDoor("PIN",false); saveUnlockLog("PIN");
      } else {
        failCount++;
        String triesLeftMsg = "Con lai " + String(3 - failCount) + " lan";
        displayMessage("Sai PIN!", triesLeftMsg, 600); indicateFailOnce();
        if (failCount >= 3) { triggerAlarm10s(); }
      }
      break;
    }
    case STATE_ADMIN_LOGIN: {
      if (inputBuffer.equals(MASTER_PIN)) {
        failCount = 0; currentState = STATE_ADMIN_MENU; displayAdminMenu();
      } else {
        failCount++;
        String triesLeftMsg = "Con lai " + String(3 - failCount) + " lan";
        displayMessage("Sai PIN!", triesLeftMsg, 600); indicateFailOnce();
        if (failCount >= 3) { triggerAlarm10s(); }
        showHomeScreen();
      }
      break;
    }
    case STATE_CHANGE_PASS: {
      if ((int)inputBuffer.length() == MAX_LEN) {
        MASTER_PIN = inputBuffer;
        prefs_cfg.putString("masterPIN", MASTER_PIN);
        displayMessage("Doi PIN OK", "", 600);
        currentState = STATE_ADMIN_MENU; displayAdminMenu();
      } else {
        displayMessage("PIN phai 4 so", "", 600);
      }
      break;
    }
    case STATE_RFID_DELETE: {
      int idx = inputBuffer.toInt();
      uint16_t count = prefs_rfid.getUShort("count", 0);
      if (idx < 0 || idx >= count) {
        displayMessage("STT khong hop le!", "", 800);
      } else {
        deleteUIDByIndex(idx);
        displayMessage("Xoa thanh cong!", "STT: " + String(idx), 800);
      }
      currentState = STATE_MANAGE_RFID_MENU; displayManageRFIDMenu();
      break;
    }
    case STATE_FINGER_ADD: {
      int id = inputBuffer.toInt();
      if (id < 1 || id > 127) {
        displayMessage("ID khong hop le", "Thu lai (1-127)", 800);
      } else {
        displayMessage("ID: " + String(id), "Dat ngon tay...", 0);
        getFingerprintEnroll(id);
      }
      currentState = STATE_MANAGE_FINGER_MENU; displayManageFingerMenu();
      break;
    }
    case STATE_FINGER_DELETE: {
      int id = inputBuffer.toInt();
      if (id < 1 || id > 127) {
        displayMessage("ID khong hop le", "Thu lai (1-127)", 800);
      } else {
        uint8_t p = finger.loadModel(id);
        if (p == FINGERPRINT_OK) {
          p = finger.deleteModel(id);
          if (p == FINGERPRINT_OK) { displayMessage("Xoa thanh cong", "ID: " + String(id), 800); } 
          else { displayMessage("Xoa that bai", "Loi giao tiep/flash", 900); }
        } else {
          displayMessage("Xoa that bai", "ID khong ton tai", 900);
        }
      }
      currentState = STATE_MANAGE_FINGER_MENU; displayManageFingerMenu();
      break;
    }
    default: break;
  }
  inputBuffer = "";
} 

// =========================================================================
// HÀM HIỂN THỊ MENU LCD
// =========================================================================
void displayAdminMenu() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("MENU ADMIN");
  lcd.setCursor(0,1); lcd.print("1.VT 2.RFID 3.PIN");
}

void displayManageRFIDMenu() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("QL The RFID");
  lcd.setCursor(0,1); lcd.print("1.Them  2.Xoa");
}

void displayManageFingerMenu() {
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("QL Van Tay");
  lcd.setCursor(0,1); lcd.print("1.Them  2.Xoa");
}

// =========================================================================
// HÀM XỬ LÝ RFID (RC522)
// =========================================================================
String uidToHexString(byte *uid, byte len) {
  String s = "";
  for (byte i = 0; i < len; i++) {
    if (uid[i] < 0x10) s += "0";
    s += String(uid[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void resetRFID() {
  Serial.println("⚠️  Reinit RC522...");
  mfrc522.PCD_Reset();
  mfrc522.PCD_Init();
  delay(50);
}

// Đọc thẻ RFID, trả về mã HEX dạng String
String readCard() {
  static int readFailCount = 0;
  if (!mfrc522.PICC_IsNewCardPresent()) return "";
  if (!mfrc522.PICC_ReadCardSerial()) {
    if (++readFailCount >= 3) { resetRFID(); readFailCount = 0; }
    return "";
  }
  readFailCount = 0;
  String hexUID = uidToHexString(mfrc522.uid.uidByte, mfrc522.uid.size);
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
  delay(3);  
  return hexUID;
}

bool isUIDStored(const String &uid) {
  uint16_t count = prefs_rfid.getUShort("count", 0);
  for (uint16_t i = 0; i < count; i++) {
    String key = "u" + String(i);
    if (prefs_rfid.getString(key.c_str(), "").equalsIgnoreCase(uid)) return true;
  }
  return false;
}

void addUID(const String &uid) {
  if (isUIDStored(uid)) return;
  uint16_t count = prefs_rfid.getUShort("count", 0);
  if (count >= MAX_UIDS) return;
  String key = "u" + String(count);
  prefs_rfid.putString(key.c_str(), uid);
  prefs_rfid.putUShort("count", count + 1);
  Serial.println("Added UID: " + uid);
}

void deleteUIDByIndex(int index) {
  uint16_t count = prefs_rfid.getUShort("count", 0);
  if (index < 0 || index >= count) return;
  for (uint16_t j = index; j < count - 1; j++) {
    String curK = "u" + String(j);
    String nxtK = "u" + String(j + 1);
    prefs_rfid.putString(curK.c_str(), prefs_rfid.getString(nxtK.c_str(), ""));
  }
  prefs_rfid.remove(("u" + String(count - 1)).c_str());
  prefs_rfid.putUShort("count", count - 1);
  Serial.println("Deleted UID at index: " + String(index));
}

// Đăng ký thẻ RFID mới
void manage_RFID_add() {
  displayMessage("Them The RFID", "Quet the...", 0);
  esp_task_wdt_delete(NULL); // Tạm dừng watchdog khi đang đợi quét thẻ
  unsigned long start = millis();
  while (millis() - start < 8000) {
    Blynk.run();
    String uid = readCard();
    if (uid != "") {
      if (isUIDStored(uid)) {
        displayMessage("The da ton tai", "", 900);
      } else {
        addUID(uid);
        displayMessage("Them thanh cong", "UID: " + uid.substring(0,8), 1000);
      }
      esp_task_wdt_add(NULL); 
      currentState = STATE_MANAGE_RFID_MENU; displayManageRFIDMenu();
      return;
    }
    handleKeypad();
    delay(5);
  }
  esp_task_wdt_add(NULL); // Khôi phục watchdog
  displayMessage("Het thoi gian", "Thu lai", 900);
  currentState = STATE_MANAGE_RFID_MENU; displayManageRFIDMenu();
}

// =========================================================================
// HÀM XỬ LÝ VÂN TAY
// =========================================================================

// Đăng ký vân tay mới theo ID
uint8_t getFingerprintEnroll(uint8_t id) {
  int p = -1;
  unsigned long start = millis();
  
  // Bước 1: Quét lần 1
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    Blynk.run(); esp_task_wdt_reset();
    if (millis() - start > 10000) {
      displayMessage("Het thoi gian", "", 800);
      return FINGERPRINT_TIMEOUT;
    }
    if (p == FINGERPRINT_NOFINGER) delay(100);
    else if (p != FINGERPRINT_OK) { displayMessage("Doc anh loi","Thu lai...", 600); }
  }
  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) { displayMessage("Xu ly loi","Thu lai...", 800); return p; }

  // Yêu cầu nhấc ngón tay ra
  displayMessage("Nha ngon tay ra","...", 700);
  do { 
    p = finger.getImage(); 
    Blynk.run(); esp_task_wdt_reset();
  } while (p != FINGERPRINT_NOFINGER);

  // Bước 2: Quét lần 2
  displayMessage("Dat lai ngon tay","cung vi tri...", 600);
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    Blynk.run(); esp_task_wdt_reset();
    if (p == FINGERPRINT_NOFINGER) delay(100);
    else if (p != FINGERPRINT_OK) { displayMessage("Doc anh loi","Thu lai...", 600); }
  }
  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) { displayMessage("Xu ly loi","Thu lai...", 800); return p; }

  // Bước 3: Tạo và lưu model
  p = finger.createModel();
  if (p != FINGERPRINT_OK) { displayMessage("Van tay khong khop","Thu lai tu dau", 1000); return p; }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK) {
    displayMessage("Them thanh cong!","ID: " + String(id), 900);
  } else {
    displayMessage("Loi luu tru","", 800);
  }
  return p;
}

// Kiểm tra vân tay có khớp với dữ liệu đã lưu không
int getFingerprintIDez() {
  int p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) return -2;        
  if (p != FINGERPRINT_OK) return -1;
  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return -1;
  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK) return 0; // Không khớp                
  return finger.fingerID;            // Khớp, trả về ID               
}

uint8_t deleteFingerprint(uint8_t id) {
  return finger.deleteModel(id);
}

// =========================================================================
// CÁC HÀM XỬ LÝ BLYNK (CALLBACK)
// =========================================================================
BLYNK_CONNECTED() { 
  Blynk.syncAll(); 
  Serial.println("Blynk Connected!");
}

// Nút nhấn V2: Mở cửa từ xa qua App
BLYNK_WRITE(VPIN_REMOTE_UNLOCK) {
  int value = param.asInt();
  if (value == 1 && !isUnlocked) {
    Serial.println("Remote unlock command received!");
    unlockDoor("Remote", false);
    saveUnlockLog("Remote");
    Blynk.virtualWrite(VPIN_REMOTE_UNLOCK, 0); // Reset nút nhấn
  }
}

// Nút nhấn V3: Xuất toàn bộ lịch sử (Log) lên Terminal
BLYNK_WRITE(VPIN_DUMP_LOG) {
  int value = param.asInt();
  if (value == 1) {
    Serial.println("Đang đọc Log và gửi lên Blynk...");
    Blynk.virtualWrite(VPIN_LOG, "clr"); // Xóa màn hình Terminal
    Blynk.virtualWrite(VPIN_LOG, "--- Lich Su Mo Cua ---\n");
    
    uint16_t count = prefs_log.getUShort("log_count", 0);
    uint16_t index = prefs_log.getUShort("log_index", 0);

    if (count == 0) {
      Blynk.virtualWrite(VPIN_LOG, "Log trong.\n");
      return;
    }

    uint16_t start_index = (count < MAX_LOG_ENTRIES) ? 0 : index;

    // Đọc log dạng xoay vòng tròn (circular buffer)
    for (int i = 0; i < count; i++) {
      uint16_t current_entry_index = (start_index + i) % MAX_LOG_ENTRIES;
      String log_entry = prefs_log.getString(("L" + String(current_entry_index)).c_str(), "");
      Blynk.virtualWrite(VPIN_LOG, String(i + 1) + ": " + log_entry + "\n");
      delay(10); // Chống ngập luồng gửi Blynk
    }
    
    Blynk.virtualWrite(VPIN_LOG, "--------------------\n");
    Serial.println("Đã gửi Log xong.");
  }
}

// =========================================================================
// HÀM LƯU LOG & ĐỒNG BỘ THỜI GIAN
// =========================================================================

// Lưu lịch sử mở cửa vào NVS (Preferences) với định dạng xoay vòng tròn
void saveUnlockLog(String uid) {
  timeClient.update();
  
  // Nếu chưa lấy được giờ NTP, ghi log dạng raw
  if (!timeClient.isTimeSet()) {
     Serial.println("CẢNH BÁO: Chưa lấy được giờ NTP, không thể thêm timestamp vào log.");
     prefs_log.putString(("L" + String(prefs_log.getUShort("log_index", 0))).c_str(), uid);
     uint16_t index = prefs_log.getUShort("log_index", 0);
     index = (index + 1) % MAX_LOG_ENTRIES;
     prefs_log.putUShort("log_index", index);
     uint16_t count = prefs_log.getUShort("log_count", 0);
      if (count < MAX_LOG_ENTRIES) { count++; prefs_log.putUShort("log_count", count); }
     return;
  }

  // Đã có giờ NTP, tạo chuỗi định dạng DD-MM-YYYY HH:MM
  int currentHour = timeClient.getHours();
  int currentMinute = timeClient.getMinutes();
  unsigned long utcEpochTime = timeClient.getEpochTime();
  time_t localEpochTime = utcEpochTime + (7 * 3600);
  struct tm *ptm = gmtime(&localEpochTime);
  
  char timestampBuffer[20];
  snprintf(timestampBuffer, sizeof(timestampBuffer), "%02d-%02d-%04d %02d:%02d", 
           ptm->tm_mday, ptm->tm_mon + 1, ptm->tm_year + 1900, currentHour, currentMinute);

  String logEntry = String(timestampBuffer) + " " + uid;
  Serial.println("Dang luu vao Log: " + logEntry);
  
  // Lưu log vào vị trí index hiện tại
  uint16_t index = prefs_log.getUShort("log_index", 0);
  prefs_log.putString(("L" + String(index)).c_str(), logEntry);
  
  // Tăng vòng chỉ số
  index = (index + 1) % MAX_LOG_ENTRIES;
  prefs_log.putUShort("log_index", index);
  
  // Tăng biến đếm tổng lượng log đã lưu
  uint16_t count = prefs_log.getUShort("log_count", 0);
  if (count < MAX_LOG_ENTRIES) {
    count++;
    prefs_log.putUShort("log_count", count);
  }
}

// Kiểm tra và giữ kết nối WiFi/Blynk
void checkBlynkConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!Blynk.connected()) {
      Serial.println("WiFi connected, connecting to Blynk...");
      Blynk.connect(3000); 
      if(Blynk.connected()) {
        Serial.println("Blynk reconnected!");
      } else {
        Serial.println("Blynk connection failed.");
      }
    }
  } else {
    Serial.println("WiFi disconnected, trying to reconnect...");
    WiFi.reconnect();
  }
}