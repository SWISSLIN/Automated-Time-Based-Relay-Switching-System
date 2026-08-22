#include <Wire.h> 
#include <RTClib.h> 
#include <LiquidCrystal_I2C.h> // Library for I2C LCD display
#include <EEPROM.h> // Permanent ON/OFF schedule storage after power cutoff
#include <avr/wdt.h> // Watchdog auto-restart if Arduino freezes

// --- I2C LCD Object (Update address if needed, 0x27 is common) ---
LiquidCrystal_I2C lcd(0x27, 16, 2); // (I2C address, columns, rows)

// LCD anti-flicker cache: rows are sent only when their visible text changes.
char lastLCDLine0[17] = "";
char lastLCDLine1[17] = "";

// LCD auto-recovery: re-initializes the I2C LCD if it goes blank/hangs.
unsigned long previousLCDRecovery = 0;
const unsigned long LCD_RECOVERY_INTERVAL = 300000UL; // 5 minutes

// Feature V1 FIXED: LCD backlight auto OFF.
const unsigned long BACKLIGHT_TIMEOUT_MS = 30000UL; // 30 seconds
unsigned long lastBacklightActivityMillis = 0;
bool lcdBacklightIsOn = true;

// RTC Object - Using DS3231 for accuracy.
RTC_DS3231 rtc;

// ---------------------------------------------------------------------------
// RTC TIME SETTING:
// First upload: keep this as true to set the DS3231 to your computer's time.
// After the correct time appears on LCD: change true to false and upload again.
// Ensure your computer date/time is correct before the first upload.
// ---------------------------------------------------------------------------
const bool SET_RTC_TIME_ON_UPLOAD = false;

// --- Pin Definitions ---
// Confirmed by diagnostic test:
// D3 LOW switches physical Relay Channel 1 ON.
// Therefore D2 is used for physical Relay Channel 2.
const int RELAY1_PIN = 8; // Physical Relay Channel 1 (active LOW)
const int RELAY2_PIN = 9; // Physical Relay Channel 2 (active LOW)

// Button Pins
const int BUTTON1_PIN = 4; // B1: Short press CH1 Setup / Long press CH1 Manual Override / Increase Value
const int BUTTON2_PIN = 5; // B2: Short press CH2 Setup / Long press CH2 Manual Override / Decrease Value
const int BUTTON3_PIN = 6; // B3: Advance Step / Save / Long press Both Channels AUTO

// Buzzer Pin
const int BUZZER_PIN = 2; // Digital pin for the buzzer

// --- Button Debounce Variables ---
const long DEBOUNCE_DELAY = 50;
unsigned long last_button_press_time[3] = {0, 0, 0}; // For BUTTON1, BUTTON2, BUTTON3

// --- Time & State Variables ---

// Structure to define a single ON/OFF schedule entry
struct ScheduleEntry {
    int on_hour;
    int on_minute;
    int on_second; 
    bool on_am; // true = AM, false = PM
    int off_hour;
    int off_minute;
    int off_second; 
    bool off_am;
};

// Three independent ON/OFF times for Channel 1 (Bulb 1)
ScheduleEntry channel1_schedule[] = {
    {00, 00, 00, false, 00, 00, 00, false}, // S1
    {00, 00, 00, false, 00, 00, 00, false}, // S2
    {00, 00, 00, false, 00, 00, 00, false}  // S3
};
const int NUM_CH1_SCHEDULES = sizeof(channel1_schedule) / sizeof(channel1_schedule[0]);

// Three independent ON/OFF times for Channel 2 (Bulb 2)
ScheduleEntry channel2_schedule[] = {
    {00, 00, 00, false, 00, 00, 00, false}, // S1
    {00, 00, 00, false, 00, 00, 00, false}, // S2
    {00, 00, 00, false, 00, 00, 00, false}  // S3
};
const int NUM_CH2_SCHEDULES = sizeof(channel2_schedule) / sizeof(channel2_schedule[0]);

// All three schedule slots operate automatically. B1+B2 mode remains removed.
const int ENABLED_SCHEDULE_SLOTS = 3;

// --- Permanent Schedule Memory in Arduino Nano EEPROM ---
// DS3231 stores real clock time; EEPROM stores manually entered relay timings.
const uint16_t EEPROM_MAGIC = 0x534C;
const uint8_t EEPROM_VERSION = 2; // Version 2 supports three slots per channel.

// Previous memory layout used by the one-slot/two-array version.
// This is kept only to copy the old saved S0 schedule into the new S1 slot.
struct LegacyStoredScheduleDataV1 {
    uint16_t magic;
    uint8_t version;
    ScheduleEntry channel1[2];
    ScheduleEntry channel2[2];
    int selected_index_ch1;
    int selected_index_ch2;
};

struct StoredScheduleData {
    uint16_t magic;
    uint8_t version;
    ScheduleEntry channel1[NUM_CH1_SCHEDULES];
    ScheduleEntry channel2[NUM_CH2_SCHEDULES];
    int selected_index_ch1;
    int selected_index_ch2;
};

// --- Mode Variables ---
enum TimerMode {
    NORMAL_MODE,
    SLOT_SELECT_MODE,
    SET_SCHED_ON_HOUR, 
    SET_SCHED_ON_MINUTE,
    SET_SCHED_ON_SECOND,
    SET_SCHED_ON_AMPM,
    SET_SCHED_OFF_HOUR,
    SET_SCHED_OFF_MINUTE,
    SET_SCHED_OFF_SECOND,
    SET_SCHED_OFF_AMPM,
    CREDITS_MODE 
};
TimerMode current_mode = NORMAL_MODE;

// Temporary variables for Schedule setting
int temp_set_hour;
int temp_set_minute;
int temp_set_second; 
bool temp_set_am;
int active_schedule_channel = 0; // 1 for CH1, 2 for CH2
int active_schedule_index_ch1 = 0; // Slot currently selected for editing CH1 (0=S1, 1=S2, 2=S3)
int active_schedule_index_ch2 = 0; // Slot currently selected for editing CH2 (0=S1, 1=S2, 2=S3)
int running_schedule_index_ch1 = -1; // Slot currently turning CH1 ON; -1 means no slot active
int running_schedule_index_ch2 = -1; // Slot currently turning CH2 ON; -1 means no slot active

// --- Manual ON/OFF Override ---
// Manual override is temporary; after a power restart the saved automatic schedule resumes.
enum ChannelControlMode {
    CHANNEL_AUTO,
    CHANNEL_MANUAL_ON,
    CHANNEL_MANUAL_OFF
};
ChannelControlMode channel1_control_mode = CHANNEL_AUTO;
ChannelControlMode channel2_control_mode = CHANNEL_AUTO;

const unsigned long MANUAL_LONG_PRESS_MS = 2000;

// Feature V4: Relay Rest Time.
// During this time AUTOMATIC schedules are blocked, but MANUAL ON is still allowed
// for emergency operation. Default: 6:00 AM to 6:00 PM daytime rest window.
const bool RELAY_REST_MODE_ENABLED = true;
const int RELAY_REST_START_HOUR_24 = 6;
const int RELAY_REST_START_MINUTE = 0;
const int RELAY_REST_START_SECOND = 0;
const int RELAY_REST_END_HOUR_24 = 18;
const int RELAY_REST_END_MINUTE = 0;
const int RELAY_REST_END_SECOND = 0;

// Feature V5: Daily Auto Restart.
// Arduino restarts once per day at this time.
// Default: 6:05 AM, inside relay rest time, so both bulbs are OFF.
const bool DAILY_AUTO_RESTART_ENABLED = true;
const int DAILY_RESTART_HOUR_24 = 6;
const int DAILY_RESTART_MINUTE = 5;
const int DAILY_RESTART_SECOND = 0;

// Permanent timing erase command: hold B1 + B2 together for 2 seconds.
// This avoids losing all stored timing data from an accidental button touch.
const unsigned long MEMORY_ERASE_HOLD_MS = 2000;
unsigned long memory_erase_start_time = 0;
bool memory_erase_completed = false;

unsigned long normal_button_start_time[3] = {0, 0, 0};
bool normal_button_tracking[3] = {false, false, false};
bool normal_button_long_handled[3] = {false, false, false};

// --- Variables for display control ---
enum DisplayState {
    DISPLAY_CH1_STATUS,
    DISPLAY_CH2_STATUS,
    DISPLAY_ALL_OFF 
};
DisplayState current_display_state = DISPLAY_CH1_STATUS;
unsigned long previousMillisDisplaySwitch = 0;
const long intervalDisplaySwitch = 2000; // Alternate channel status every 2 seconds

// --- Variables for Credits Mode ---
const char* CREDITS_TEXT = " PROJECT BY SWISSLIN RAJ...! FOR MORE PROJECTS CONTACT : 6369907729  ";
unsigned long previousMillisScroll = 0;
const long intervalScroll = 300; // Scroll speed (300ms per character shift)
int scroll_position = 0;


// --- Function Prototypes ---
bool readButton(int pin, int button_index);
bool readThreeButtonsSimultaneously(int pinA, int pinB, int pinC);
void timeIn12HourFormat(int hour24, int minute, bool &amPm, int &hour12);
void setRelayState(int relayPin, bool state, int channelNum);
void playBuzzer(int durationMs);
void invalidateLCDCache();
void clearLCD();
void printLCDLineIfChanged(byte row, const char* text);
void recoverLCDIfNeeded();
void wakeBacklightOnButtonPress();
void handleBacklightAutoOff();
void updateTimeOnLCD();
void displayChannelStatus(int channelNum, int currentDigitalState);
void displayAllChannelsOff();
void handleCreditsMode();
void checkAutomaticControl();
void enterSlotSelectMode(int channel);
void handleSlotSelectMode();
void enterSetScheduleMode(int channel);
void handleSetupMode();
int convert12to24Hour(int hour12, bool amPm);
long getTotalSeconds(int hour24, int minute, int second);
bool isTimeInsideSchedule(long currentSeconds, long onSeconds, long offSeconds);
bool isRelayRestTimeNow();
void handleDailyAutoRestart();
void softwareRestartNow();
bool schedulesOverlap(const ScheduleEntry &a, const ScheduleEntry &b);
bool hasScheduleConflictForChannel(int channel);
void showConflictWarning(int channel);
bool isValidScheduleEntry(const ScheduleEntry &entry);
void saveSchedulesToEEPROM();
bool loadSchedulesFromEEPROM();
bool channelIsManual(int channel);
void resetNormalButtonTracking();
void cycleManualOverride(int channel);
void returnBothChannelsToAuto();
bool handleNormalModeButtonActions();
bool handleMemoryEraseButtons();
void eraseAllStoredSchedules();
void resetScheduleEntry(ScheduleEntry &entry);

// --- Utility Functions ---

int convert12to24Hour(int hour12, bool amPm) {
    if (amPm) { if (hour12 == 12) return 0; return hour12; }
    else { if (hour12 == 12) return 12; return hour12 + 12; }
}

long getTotalSeconds(int hour24, int minute, int second) {
    return (long)hour24 * 3600 + (long)minute * 60 + (long)second;
}

// Returns true for the full interval between ON time and OFF time.
// It also supports a schedule that crosses midnight.
bool isTimeInsideSchedule(long currentSeconds, long onSeconds, long offSeconds) {
    if (onSeconds == offSeconds) {
        return false; // Prevent accidental full-day ON state.
    }

    if (onSeconds < offSeconds) {
        return currentSeconds >= onSeconds && currentSeconds < offSeconds;
    }

    // Example: ON at 11:00 PM and OFF at 06:00 AM.
    return currentSeconds >= onSeconds || currentSeconds < offSeconds;
}


bool schedulesOverlap(const ScheduleEntry &a, const ScheduleEntry &b) {
    // Blank/unconfigured slots do not create conflicts.
    bool aBlank =
        a.on_hour == 0 && a.on_minute == 0 && a.on_second == 0 &&
        a.off_hour == 0 && a.off_minute == 0 && a.off_second == 0;

    bool bBlank =
        b.on_hour == 0 && b.on_minute == 0 && b.on_second == 0 &&
        b.off_hour == 0 && b.off_minute == 0 && b.off_second == 0;

    if (aBlank || bBlank) return false;
    if (!isValidScheduleEntry(a) || !isValidScheduleEntry(b)) return false;

    long aOn = getTotalSeconds(convert12to24Hour(a.on_hour, a.on_am), a.on_minute, a.on_second);
    long aOff = getTotalSeconds(convert12to24Hour(a.off_hour, a.off_am), a.off_minute, a.off_second);
    long bOn = getTotalSeconds(convert12to24Hour(b.on_hour, b.on_am), b.on_minute, b.on_second);
    long bOff = getTotalSeconds(convert12to24Hour(b.off_hour, b.off_am), b.off_minute, b.off_second);

    if (aOn == aOff || bOn == bOff) return false;

    // Overlap check works even if one schedule crosses midnight.
    if (isTimeInsideSchedule(aOn, bOn, bOff)) return true;
    if (isTimeInsideSchedule(bOn, aOn, aOff)) return true;

    return false;
}

bool hasScheduleConflictForChannel(int channel) {
    ScheduleEntry* schedules = (channel == 1) ? channel1_schedule : channel2_schedule;
    int totalSlots = (channel == 1) ? NUM_CH1_SCHEDULES : NUM_CH2_SCHEDULES;

    for (int i = 0; i < totalSlots; i++) {
        for (int j = i + 1; j < totalSlots; j++) {
            if (schedulesOverlap(schedules[i], schedules[j])) {
                return true;
            }
        }
    }

    return false;
}

void showConflictWarning(int channel) {
    clearLCD();
    lcd.backlight();
    lcdBacklightIsOn = true;
    lastBacklightActivityMillis = millis();

    char line0[17];
    snprintf(line0, sizeof(line0), "CH%d TIME CONFLICT", channel);
    printLCDLineIfChanged(0, line0);
    printLCDLineIfChanged(1, "CHECK SLOTS");

    playBuzzer(120);
    delay(150);
    playBuzzer(120);
    delay(150);
    playBuzzer(350);
    delay(1500);
    clearLCD();
}

bool isRelayRestTimeNow() {
    if (!RELAY_REST_MODE_ENABLED) {
        return false;
    }

    DateTime now = rtc.now();
    long currentSeconds = getTotalSeconds(now.hour(), now.minute(), now.second());
    long restStartSeconds = getTotalSeconds(
        RELAY_REST_START_HOUR_24,
        RELAY_REST_START_MINUTE,
        RELAY_REST_START_SECOND
    );
    long restEndSeconds = getTotalSeconds(
        RELAY_REST_END_HOUR_24,
        RELAY_REST_END_MINUTE,
        RELAY_REST_END_SECOND
    );

    return isTimeInsideSchedule(currentSeconds, restStartSeconds, restEndSeconds);
}


void softwareRestartNow() {
    clearLCD();
    lcd.backlight();
    lcdBacklightIsOn = true;
    lastBacklightActivityMillis = millis();

    printLCDLineIfChanged(0, "DAILY RESTART");
    printLCDLineIfChanged(1, "PLEASE WAIT");
    playBuzzer(120);
    delay(400);

    // Force a fast watchdog reset.
    wdt_enable(WDTO_15MS);
    while (true) {
        // Wait for watchdog reset.
    }
}

void handleDailyAutoRestart() {
    if (!DAILY_AUTO_RESTART_ENABLED) {
        return;
    }

    DateTime now = rtc.now();

    // Exact second trigger prevents repeated restart loop after reboot.
    if (now.hour() == DAILY_RESTART_HOUR_24 &&
        now.minute() == DAILY_RESTART_MINUTE &&
        now.second() == DAILY_RESTART_SECOND) {
        softwareRestartNow();
    }
}

// Checks schedule data read from EEPROM before using it.
// A complete 00:00:00 blank slot is allowed as "not yet configured".
bool isValidScheduleEntry(const ScheduleEntry &entry) {
    bool blankEntry =
        entry.on_hour == 0 && entry.on_minute == 0 && entry.on_second == 0 &&
        entry.off_hour == 0 && entry.off_minute == 0 && entry.off_second == 0;

    if (blankEntry) return true;

    bool validOn =
        entry.on_hour >= 1 && entry.on_hour <= 12 &&
        entry.on_minute >= 0 && entry.on_minute <= 59 &&
        entry.on_second >= 0 && entry.on_second <= 59;

    bool validOff =
        entry.off_hour >= 1 && entry.off_hour <= 12 &&
        entry.off_minute >= 0 && entry.off_minute <= 59 &&
        entry.off_second >= 0 && entry.off_second <= 59;

    return validOn && validOff;
}

// Permanently save all three ON/OFF timing slots for both channels.
void saveSchedulesToEEPROM() {
    StoredScheduleData savedData;
    savedData.magic = EEPROM_MAGIC;
    savedData.version = EEPROM_VERSION;

    for (int i = 0; i < NUM_CH1_SCHEDULES; i++) {
        savedData.channel1[i] = channel1_schedule[i];
    }
    for (int i = 0; i < NUM_CH2_SCHEDULES; i++) {
        savedData.channel2[i] = channel2_schedule[i];
    }

    savedData.selected_index_ch1 = active_schedule_index_ch1;
    savedData.selected_index_ch2 = active_schedule_index_ch2;

    EEPROM.put(0, savedData);
    Serial.println("All 3 schedule slots saved permanently in EEPROM.");
}

// Load all saved three-slot timings after power returns.
// If old V1 memory is found, its CH1/CH2 S0 values are copied into new S1.
bool loadSchedulesFromEEPROM() {
    StoredScheduleData savedData;
    EEPROM.get(0, savedData);

    bool validV2 =
        savedData.magic == EEPROM_MAGIC &&
        savedData.version == EEPROM_VERSION &&
        savedData.selected_index_ch1 >= 0 &&
        savedData.selected_index_ch1 < NUM_CH1_SCHEDULES &&
        savedData.selected_index_ch2 >= 0 &&
        savedData.selected_index_ch2 < NUM_CH2_SCHEDULES;

    if (validV2) {
        for (int i = 0; i < NUM_CH1_SCHEDULES; i++) {
            if (!isValidScheduleEntry(savedData.channel1[i])) validV2 = false;
        }
        for (int i = 0; i < NUM_CH2_SCHEDULES; i++) {
            if (!isValidScheduleEntry(savedData.channel2[i])) validV2 = false;
        }
    }

    if (validV2) {
        for (int i = 0; i < NUM_CH1_SCHEDULES; i++) {
            channel1_schedule[i] = savedData.channel1[i];
        }
        for (int i = 0; i < NUM_CH2_SCHEDULES; i++) {
            channel2_schedule[i] = savedData.channel2[i];
        }

        active_schedule_index_ch1 = savedData.selected_index_ch1;
        active_schedule_index_ch2 = savedData.selected_index_ch2;

        Serial.println("All 3 saved schedule slots restored from EEPROM.");
        return true;
    }

    // Attempt to migrate the old power-cut-memory version.
    LegacyStoredScheduleDataV1 legacyData;
    EEPROM.get(0, legacyData);

    bool validV1 =
        legacyData.magic == EEPROM_MAGIC &&
        legacyData.version == 1 &&
        isValidScheduleEntry(legacyData.channel1[0]) &&
        isValidScheduleEntry(legacyData.channel2[0]);

    if (validV1) {
        channel1_schedule[0] = legacyData.channel1[0]; // Previous CH1 S0 becomes CH1 S1.
        channel2_schedule[0] = legacyData.channel2[0]; // Previous CH2 S0 becomes CH2 S1.
        active_schedule_index_ch1 = 0;
        active_schedule_index_ch2 = 0;
        saveSchedulesToEEPROM();
        Serial.println("Previous saved timing upgraded into new S1 slots.");
        return true;
    }

    // First use or invalid memory: create three blank slots safely.
    active_schedule_index_ch1 = 0;
    active_schedule_index_ch2 = 0;
    saveSchedulesToEEPROM();
    Serial.println("No saved timing found. Blank S1-S3 slots created.");
    return false;
}

/**
 * @brief Reads a button state with software debounce.
 */
bool readButton(int pin, int button_index) {
    // Prevent individual presses from triggering if a simultaneous press is ongoing
    if (current_mode == NORMAL_MODE && 
        (digitalRead(BUTTON1_PIN) == LOW && digitalRead(BUTTON2_PIN) == LOW) ||
        (digitalRead(BUTTON1_PIN) == LOW && digitalRead(BUTTON2_PIN) == LOW && digitalRead(BUTTON3_PIN) == LOW)) {
        return false;
    }
    
    if (digitalRead(pin) == LOW) {
        if (millis() - last_button_press_time[button_index] > DEBOUNCE_DELAY) {
            last_button_press_time[button_index] = millis();
            return true; 
        }
    } else {
        last_button_press_time[button_index] = 0;
    }
    return false;
}


/**
 * @brief NEW: Checks for simultaneous press of all three buttons (B1, B2, B3).
 */
bool readThreeButtonsSimultaneously(int pinA, int pinB, int pinC) {
    if (digitalRead(pinA) == LOW && digitalRead(pinB) == LOW && digitalRead(pinC) == LOW) {
        // Use index 0 for debounce
        if (millis() - last_button_press_time[0] > DEBOUNCE_DELAY) {
            last_button_press_time[0] = millis();
            return true;
        }
    }
    return false;
}


void timeIn12HourFormat(int hour24, int minute, bool &amPm, int &hour12) {
    if (hour24 == 0) { hour12 = 12; amPm = true; }      
    else if (hour24 == 12) { hour12 = 12; amPm = false; } 
    else if (hour24 > 12) { hour12 = hour24 - 12; amPm = false; } 
    else { hour12 = hour24; amPm = true; }           
}

void setRelayState(int relayPin, bool state, int channelNum) {
    // Confirmed relay operation: ACTIVE LOW.
    // state = true  -> relay ON  -> output LOW
    // state = false -> relay OFF -> output HIGH
    digitalWrite(relayPin, state ? LOW : HIGH);
}

void playBuzzer(int durationMs) {
    digitalWrite(BUZZER_PIN, HIGH); 
    delay(durationMs);          
    digitalWrite(BUZZER_PIN, LOW);
}

// --- Erase All Stored ON/OFF Timing Data ---

void resetScheduleEntry(ScheduleEntry &entry) {
    entry.on_hour = 0;
    entry.on_minute = 0;
    entry.on_second = 0;
    entry.on_am = false;
    entry.off_hour = 0;
    entry.off_minute = 0;
    entry.off_second = 0;
    entry.off_am = false;
}

void eraseAllStoredSchedules() {
    // Remove all S1, S2 and S3 stored times from both channels.
    for (int i = 0; i < NUM_CH1_SCHEDULES; i++) {
        resetScheduleEntry(channel1_schedule[i]);
    }
    for (int i = 0; i < NUM_CH2_SCHEDULES; i++) {
        resetScheduleEntry(channel2_schedule[i]);
    }

    // Cancel a partially edited timing and return safely to AUTO mode.
    active_schedule_index_ch1 = 0;
    active_schedule_index_ch2 = 0;
    running_schedule_index_ch1 = -1;
    running_schedule_index_ch2 = -1;
    active_schedule_channel = 0;
    channel1_control_mode = CHANNEL_AUTO;
    channel2_control_mode = CHANNEL_AUTO;
    current_mode = NORMAL_MODE;
    resetNormalButtonTracking();

    // Active-LOW relays: false makes both channels OFF immediately.
    setRelayState(RELAY1_PIN, false, 1);
    setRelayState(RELAY2_PIN, false, 2);

    // Save empty timing slots in EEPROM, permanently replacing the old timings.
    saveSchedulesToEEPROM();

    clearLCD();
    printLCDLineIfChanged(0, "MEMORY ERASED");
    printLCDLineIfChanged(1, "ALL TIMES CLEAR");
    playBuzzer(100);
    delay(100);
    playBuzzer(250);
    delay(1200);
    clearLCD();
}

// This is checked before all normal/setting screen button operations.
// It therefore also works while selecting a slot or while entering ON/OFF time.
bool handleMemoryEraseButtons() {
    bool erasePressed =
        digitalRead(BUTTON1_PIN) == LOW &&
        digitalRead(BUTTON2_PIN) == LOW &&
        digitalRead(BUTTON3_PIN) == HIGH; // Keeps B1+B2+B3 credits separate.

    if (erasePressed) {
        if (memory_erase_start_time == 0) {
            memory_erase_start_time = millis();
            memory_erase_completed = false;
        }

        if (!memory_erase_completed &&
            millis() - memory_erase_start_time >= MEMORY_ERASE_HOLD_MS) {
            memory_erase_completed = true;
            eraseAllStoredSchedules();
        }

        // Do not let B1/B2 increase/decrease or select anything during this hold.
        return true;
    }

    // It cannot erase again until B1 and B2 are released and pressed again.
    memory_erase_start_time = 0;
    memory_erase_completed = false;
    return false;
}

// --- LCD Anti-Flicker Functions ---

void invalidateLCDCache() {
    lastLCDLine0[0] = '\0';
    lastLCDLine1[0] = '\0';
}

void clearLCD() {
    lcd.clear();
    invalidateLCDCache();
}

void printLCDLineIfChanged(byte row, const char* text) {
    char paddedLine[17];
    char* storedLine = (row == 0) ? lastLCDLine0 : lastLCDLine1;

    snprintf(paddedLine, sizeof(paddedLine), "%-16.16s", text);

    if (strcmp(storedLine, paddedLine) != 0) {
        lcd.setCursor(0, row);
        lcd.print(paddedLine);
        strcpy(storedLine, paddedLine);
    }
}

void recoverLCDIfNeeded() {
    if (millis() - previousLCDRecovery >= LCD_RECOVERY_INTERVAL) {
        previousLCDRecovery = millis();

        lcd.init();
        if (lcdBacklightIsOn) lcd.backlight();
        else lcd.noBacklight();
        invalidateLCDCache();

        // Rewrite current time line after LCD recovery.
        if (current_mode == NORMAL_MODE) {
            updateTimeOnLCD();
        }
    }
}


void wakeBacklightOnButtonPress() {
    bool anyButtonPressed =
        digitalRead(BUTTON1_PIN) == LOW ||
        digitalRead(BUTTON2_PIN) == LOW ||
        digitalRead(BUTTON3_PIN) == LOW;

    if (anyButtonPressed) {
        lastBacklightActivityMillis = millis();

        if (!lcdBacklightIsOn) {
            lcd.backlight();
            lcdBacklightIsOn = true;
            invalidateLCDCache();
        }
    }
}

void handleBacklightAutoOff() {
    wakeBacklightOnButtonPress();

    // Keep backlight ON while selecting slot, setting time, or viewing credits.
    if (current_mode != NORMAL_MODE) {
        lastBacklightActivityMillis = millis();
        return;
    }

    if (lcdBacklightIsOn &&
        millis() - lastBacklightActivityMillis >= BACKLIGHT_TIMEOUT_MS) {
        lcd.noBacklight();
        lcdBacklightIsOn = false;
    }
}

// --- Manual Override Functions ---

bool channelIsManual(int channel) {
    return (channel == 1) ? (channel1_control_mode != CHANNEL_AUTO)
                          : (channel2_control_mode != CHANNEL_AUTO);
}

void resetNormalButtonTracking() {
    for (int i = 0; i < 3; i++) {
        normal_button_start_time[i] = 0;
        normal_button_tracking[i] = false;
        normal_button_long_handled[i] = false;
    }
}

void cycleManualOverride(int channel) {
    ChannelControlMode* mode = (channel == 1) ? &channel1_control_mode : &channel2_control_mode;

    // Each long press cycles AUTO -> MANUAL ON -> MANUAL OFF -> AUTO.
    if (*mode == CHANNEL_AUTO) {
        *mode = CHANNEL_MANUAL_ON;
    } else if (*mode == CHANNEL_MANUAL_ON) {
        *mode = CHANNEL_MANUAL_OFF;
    } else {
        *mode = CHANNEL_AUTO;
    }

    clearLCD();
    lcd.print("CH");
    lcd.print(channel);
    lcd.print(" CONTROL");
    lcd.setCursor(0, 1);

    if (*mode == CHANNEL_MANUAL_ON) {
        lcd.print("MANUAL ON");
    } else if (*mode == CHANNEL_MANUAL_OFF) {
        lcd.print("MANUAL OFF");
    } else {
        lcd.print("AUTO MODE");
    }

    playBuzzer(180);
    delay(700);
    clearLCD();
}

void returnBothChannelsToAuto() {
    channel1_control_mode = CHANNEL_AUTO;
    channel2_control_mode = CHANNEL_AUTO;

    clearLCD();
    lcd.print("ALL CH AUTO MODE");
    lcd.setCursor(0, 1);
    lcd.print("TIMER ACTIVE");
    playBuzzer(250);
    delay(900);
    clearLCD();
}

// Normal display controls:
// Short B1/B2 = set schedule; hold B1/B2 for 2 seconds = manual override.
// Hold B3 for 2 seconds = return both relays to automatic timer control.
bool handleNormalModeButtonActions() {
    bool b1Pressed = digitalRead(BUTTON1_PIN) == LOW;
    bool b2Pressed = digitalRead(BUTTON2_PIN) == LOW;
    bool b3Pressed = digitalRead(BUTTON3_PIN) == LOW;

    int pressedCount = (b1Pressed ? 1 : 0) + (b2Pressed ? 1 : 0) + (b3Pressed ? 1 : 0);

    // The original two/three-button combinations are handled before this function.
    if (pressedCount > 1) {
        resetNormalButtonTracking();
        return false;
    }

    if (pressedCount == 1) {
        int buttonIndex = b1Pressed ? 0 : (b2Pressed ? 1 : 2);

        for (int i = 0; i < 3; i++) {
            if (i != buttonIndex) {
                normal_button_start_time[i] = 0;
                normal_button_tracking[i] = false;
                normal_button_long_handled[i] = false;
            }
        }

        if (!normal_button_tracking[buttonIndex]) {
            normal_button_tracking[buttonIndex] = true;
            normal_button_long_handled[buttonIndex] = false;
            normal_button_start_time[buttonIndex] = millis();
        }

        if (!normal_button_long_handled[buttonIndex] &&
            millis() - normal_button_start_time[buttonIndex] >= MANUAL_LONG_PRESS_MS) {
            normal_button_long_handled[buttonIndex] = true;
            if (buttonIndex == 0) {
                cycleManualOverride(1);
            } else if (buttonIndex == 1) {
                cycleManualOverride(2);
            } else {
                returnBothChannelsToAuto();
            }
            return true;
        }

        return false;
    }

    // On release, a short B1/B2 press opens that channel's S1/S2/S3 selection screen.
    if (normal_button_tracking[0] && !normal_button_long_handled[0]) {
        resetNormalButtonTracking();
        enterSlotSelectMode(1);
        return true;
    }
    if (normal_button_tracking[1] && !normal_button_long_handled[1]) {
        resetNormalButtonTracking();
        enterSlotSelectMode(2);
        return true;
    }

    resetNormalButtonTracking();
    return false;
}

// --- Display Functions ---

void updateTimeOnLCD() {
    DateTime now = rtc.now();
    int hour12;
    bool amPm;
    char timeLine[17];

    timeIn12HourFormat(now.hour(), now.minute(), amPm, hour12);
    snprintf(timeLine, sizeof(timeLine), "Time:%2d:%02d:%02d %c",
             hour12, now.minute(), now.second(), amPm ? 'A' : 'P');

    printLCDLineIfChanged(0, timeLine);
}

/**
 * @brief Displays the current ON/OFF status of a specific channel, including the current schedule index.
 */
void displayChannelStatus(int channelNum, int currentDigitalState) {
    bool relayIsOn = (currentDigitalState == LOW); // Active-LOW relay
    bool manualMode = channelIsManual(channelNum);
    int runningSlot = (channelNum == 1) ? running_schedule_index_ch1 : running_schedule_index_ch2;
    char statusLine[17];

    if (manualMode) {
        snprintf(statusLine, sizeof(statusLine), "CH%d MAN:%s",
                 channelNum, relayIsOn ? "ON" : "OFF");
    } else if (relayIsOn && runningSlot >= 0) {
        snprintf(statusLine, sizeof(statusLine), "CH%d AUTO:ON S%d",
                 channelNum, runningSlot + 1);
    } else {
        snprintf(statusLine, sizeof(statusLine), "CH%d AUTO:%s",
                 channelNum, relayIsOn ? "ON" : "OFF");
    }

    printLCDLineIfChanged(1, statusLine);
}

/**
 * @brief Displays the "ALL CHANNELS OFF" message on the bottom line.
 */
void displayAllChannelsOff() {
    printLCDLineIfChanged(1, "ALL CHANNELS OFF");
}

/**
 * @brief NEW: Handles the scrolling text display for credits.
 */
void handleCreditsMode() {
    unsigned long currentMillis = millis();

    // Scroll logic
    if (currentMillis - previousMillisScroll >= intervalScroll) {
        previousMillisScroll = currentMillis;

        lcd.setCursor(0, 1);
        
        // Print the scrolling window
        for (int i = 0; i < 16; i++) {
            int charIndex = scroll_position + i;
            if (charIndex < strlen(CREDITS_TEXT)) {
                lcd.print(CREDITS_TEXT[charIndex]);
            } else {
                lcd.print(' ');
            }
        }

        scroll_position++;

        // Check if the entire string has scrolled off the screen
        if (scroll_position > strlen(CREDITS_TEXT)) {
            scroll_position = 0; // Reset scroll position
            
            // Exit Credits Mode after one full cycle
            current_mode = NORMAL_MODE;
            clearLCD();
            return; 
        }
    }

    // Always show time on top line while credits scroll
    updateTimeOnLCD();
}


// --- Core Logic ---

/**
 * @brief Checks the current time against all schedules and sets relays.
 */
void checkAutomaticControl() {
    DateTime now = rtc.now();
    long currentTotalSeconds = getTotalSeconds(now.hour(), now.minute(), now.second());

    bool channel1ShouldBeOn = false;
    bool channel2ShouldBeOn = false;
    running_schedule_index_ch1 = -1;
    running_schedule_index_ch2 = -1;

    // Channel 1 becomes ON whenever current time is inside any CH1 schedule period.
    for (int i = 0; i < NUM_CH1_SCHEDULES; i++) {
        ScheduleEntry entry = channel1_schedule[i];
        long onTotalSeconds = getTotalSeconds(
            convert12to24Hour(entry.on_hour, entry.on_am),
            entry.on_minute,
            entry.on_second
        );
        long offTotalSeconds = getTotalSeconds(
            convert12to24Hour(entry.off_hour, entry.off_am),
            entry.off_minute,
            entry.off_second
        );

        if (isTimeInsideSchedule(currentTotalSeconds, onTotalSeconds, offTotalSeconds)) {
            channel1ShouldBeOn = true;
            if (running_schedule_index_ch1 < 0) {
                running_schedule_index_ch1 = i;
            }
        }
    }

    // Channel 2 becomes ON whenever current time is inside any CH2 schedule period.
    for (int i = 0; i < NUM_CH2_SCHEDULES; i++) {
        ScheduleEntry entry = channel2_schedule[i];
        long onTotalSeconds = getTotalSeconds(
            convert12to24Hour(entry.on_hour, entry.on_am),
            entry.on_minute,
            entry.on_second
        );
        long offTotalSeconds = getTotalSeconds(
            convert12to24Hour(entry.off_hour, entry.off_am),
            entry.off_minute,
            entry.off_second
        );

        if (isTimeInsideSchedule(currentTotalSeconds, onTotalSeconds, offTotalSeconds)) {
            channel2ShouldBeOn = true;
            if (running_schedule_index_ch2 < 0) {
                running_schedule_index_ch2 = i;
            }
        }
    }

    // Manual override has priority over the automatic schedule.
    if (channel1_control_mode == CHANNEL_MANUAL_ON) {
        channel1ShouldBeOn = true;
    } else if (channel1_control_mode == CHANNEL_MANUAL_OFF) {
        channel1ShouldBeOn = false;
    }

    if (channel2_control_mode == CHANNEL_MANUAL_ON) {
        channel2ShouldBeOn = true;
    } else if (channel2_control_mode == CHANNEL_MANUAL_OFF) {
        channel2ShouldBeOn = false;
    }

    // Feature V4 modified: Relay Rest Time only blocks AUTOMATIC schedules.
    // Manual ON/OFF override is allowed during the rest window for emergencies.
    // Therefore:
    //   AUTO        -> forced OFF during rest time
    //   MANUAL ON   -> relay can still be turned ON
    //   MANUAL OFF  -> relay remains OFF
    if (isRelayRestTimeNow()) {
        if (channel1_control_mode == CHANNEL_AUTO) {
            channel1ShouldBeOn = false;
            running_schedule_index_ch1 = -1;
        }

        if (channel2_control_mode == CHANNEL_AUTO) {
            channel2ShouldBeOn = false;
            running_schedule_index_ch2 = -1;
        }
    }

    // Active-LOW relay module: true = physical relay ON, false = physical relay OFF.
    static bool previousChannel1State = false;
    static bool previousChannel2State = false;
    static bool firstAutomaticCheck = true;

    setRelayState(RELAY1_PIN, channel1ShouldBeOn, 1);
    setRelayState(RELAY2_PIN, channel2ShouldBeOn, 2);

    // Beep only when a relay state changes; do not beep repeatedly in the ON interval.
    if (!firstAutomaticCheck) {
        if (channel1ShouldBeOn != previousChannel1State) {
            playBuzzer(100);
        }
        if (channel2ShouldBeOn != previousChannel2State) {
            playBuzzer(100);
        }
    }

    previousChannel1State = channel1ShouldBeOn;
    previousChannel2State = channel2ShouldBeOn;
    firstAutomaticCheck = false;
}

// --- Three-Slot Selection Functions ---

void enterSlotSelectMode(int channel) {
    active_schedule_channel = channel;
    current_mode = SLOT_SELECT_MODE;

    clearLCD();
    lcd.print("CH");
    lcd.print(channel);
    lcd.print(" SELECT SLOT");
    lcd.setCursor(0, 1);
    lcd.print("B1/B2:S");
    lcd.print(((channel == 1) ? active_schedule_index_ch1 : active_schedule_index_ch2) + 1);
    lcd.print(" B3:OK");
    playBuzzer(100);
}

void handleSlotSelectMode() {
    int* selectedIndex = (active_schedule_channel == 1)
                           ? &active_schedule_index_ch1
                           : &active_schedule_index_ch2;

    // B1 selects next slot: S1 -> S2 -> S3 -> S1.
    if (readButton(BUTTON1_PIN, 0)) {
        (*selectedIndex)++;
        if (*selectedIndex >= ENABLED_SCHEDULE_SLOTS) {
            *selectedIndex = 0;
        }
        playBuzzer(50);
    }

    // B2 selects previous slot: S1 -> S3 -> S2 -> S1.
    if (readButton(BUTTON2_PIN, 1)) {
        (*selectedIndex)--;
        if (*selectedIndex < 0) {
            *selectedIndex = ENABLED_SCHEDULE_SLOTS - 1;
        }
        playBuzzer(50);
    }

    // B3 confirms selected slot and starts ON/OFF editing.
    if (readButton(BUTTON3_PIN, 2)) {
        int selectedChannel = active_schedule_channel;
        playBuzzer(120);
        enterSetScheduleMode(selectedChannel);
        return;
    }

    char slotTopLine[17];
    char slotBottomLine[17];
    snprintf(slotTopLine, sizeof(slotTopLine), "CH%d SELECT SLOT", active_schedule_channel);
    snprintf(slotBottomLine, sizeof(slotBottomLine), "B1/B2:S%d B3:OK", *selectedIndex + 1);
    printLCDLineIfChanged(0, slotTopLine);
    printLCDLineIfChanged(1, slotBottomLine);
}

// --- Schedule Setup Functions ---

/**
 * @brief Enters the schedule setup mode for the specified channel, loading existing values from the selected index.
 */
void enterSetScheduleMode(int channel) {
    active_schedule_channel = channel;
    int index = (channel == 1) ? active_schedule_index_ch1 : active_schedule_index_ch2;
    ScheduleEntry* active_schedule = (channel == 1) ? &channel1_schedule[index] : &channel2_schedule[index];

    // Load existing schedule into temporary variables for editing (Start with ON Time)
    temp_set_hour = active_schedule->on_hour;
    temp_set_minute = active_schedule->on_minute;
    temp_set_second = active_schedule->on_second;
    temp_set_am = active_schedule->on_am;

    // For a blank slot, start manual editing from 12:00:00 AM.
    if (temp_set_hour < 1 || temp_set_hour > 12) {
        temp_set_hour = 12;
        temp_set_minute = 0;
        temp_set_second = 0;
        temp_set_am = true;
    }

    current_mode = SET_SCHED_ON_HOUR;
    
    clearLCD();
    lcd.print("CH");
    lcd.print(channel);
    lcd.print(" S"); lcd.print(index + 1);
    lcd.print(" ON Hour:");
    
    lcd.setCursor(0, 1);
    lcd.print(temp_set_hour);
    lcd.print(temp_set_am ? " AM" : " PM");
    playBuzzer(100);
}


/**
 * @brief Handles button presses and LCD updates while in schedule setting mode (B1=Inc, B2=Dec, B3=Advance).
 */
void handleSetupMode() {
    
    // Get the pointer to the schedule entry being edited
    int index = (active_schedule_channel == 1) ? active_schedule_index_ch1 : active_schedule_index_ch2;
    ScheduleEntry* sched_ptr = (active_schedule_channel == 1) ? &channel1_schedule[index] : &channel2_schedule[index];
    
    // =================================================================
    // BUTTON 1 (D4): INCREASE VALUE
    // =================================================================
    if (readButton(BUTTON1_PIN, 0)) {
        playBuzzer(50);
        
        if (current_mode == SET_SCHED_ON_HOUR || current_mode == SET_SCHED_OFF_HOUR) { 
            temp_set_hour++;
            if (temp_set_hour > 12) temp_set_hour = 1; 
        }
        else if (current_mode == SET_SCHED_ON_MINUTE || current_mode == SET_SCHED_OFF_MINUTE) { 
            temp_set_minute++;
            if (temp_set_minute > 59) temp_set_minute = 0; 
        }
        else if (current_mode == SET_SCHED_ON_SECOND || current_mode == SET_SCHED_OFF_SECOND) { 
            temp_set_second++;
            if (temp_set_second > 59) temp_set_second = 0; 
        }
        else if (current_mode == SET_SCHED_ON_AMPM || current_mode == SET_SCHED_OFF_AMPM) { 
            temp_set_am = !temp_set_am;
        }
    }

    // =================================================================
    // BUTTON 2 (D5): DECREASE VALUE
    // =================================================================
    if (readButton(BUTTON2_PIN, 1)) {
        playBuzzer(50);
        
        if (current_mode == SET_SCHED_ON_HOUR || current_mode == SET_SCHED_OFF_HOUR) { 
            temp_set_hour--;
            if (temp_set_hour < 1) temp_set_hour = 12; 
        }
        else if (current_mode == SET_SCHED_ON_MINUTE || current_mode == SET_SCHED_OFF_MINUTE) { 
            temp_set_minute--;
            if (temp_set_minute < 0) temp_set_minute = 59; 
        }
        else if (current_mode == SET_SCHED_ON_SECOND || current_mode == SET_SCHED_OFF_SECOND) { 
            temp_set_second--;
            if (temp_set_second < 0) temp_set_second = 59; 
        }
        else if (current_mode == SET_SCHED_ON_AMPM || current_mode == SET_SCHED_OFF_AMPM) { 
            temp_set_am = !temp_set_am;
        }
    }

    // =================================================================
    // BUTTON 3 (D6): ADVANCE / CONFIRM / SAVE
    // =================================================================
    if (readButton(BUTTON3_PIN, 2)) {
        playBuzzer(150);
        
        // --- Confirmation and Advancement Logic (8 Steps) ---
        if (current_mode == SET_SCHED_ON_HOUR) {
            sched_ptr->on_hour = temp_set_hour;
            current_mode = SET_SCHED_ON_MINUTE;
        } 
        else if (current_mode == SET_SCHED_ON_MINUTE) {
            sched_ptr->on_minute = temp_set_minute;
            current_mode = SET_SCHED_ON_SECOND;
        } 
        else if (current_mode == SET_SCHED_ON_SECOND) { 
            sched_ptr->on_second = temp_set_second;
            current_mode = SET_SCHED_ON_AMPM;
        } 
        else if (current_mode == SET_SCHED_ON_AMPM) {
            // Step 4: Saves ON Time and immediately loads values for OFF Time
            sched_ptr->on_am = temp_set_am;
            
            // Load existing OFF schedule into temp variables for immediate editing
            temp_set_hour = sched_ptr->off_hour;
            temp_set_minute = sched_ptr->off_minute;
            temp_set_second = sched_ptr->off_second;
            temp_set_am = sched_ptr->off_am;

            // For a blank slot, start manual editing from 12:00:00 AM.
            if (temp_set_hour < 1 || temp_set_hour > 12) {
                temp_set_hour = 12;
                temp_set_minute = 0;
                temp_set_second = 0;
                temp_set_am = true;
            }

            current_mode = SET_SCHED_OFF_HOUR; // Auto-advance to OFF Hour
        } 
        else if (current_mode == SET_SCHED_OFF_HOUR) {
            sched_ptr->off_hour = temp_set_hour;
            current_mode = SET_SCHED_OFF_MINUTE;
        } 
        else if (current_mode == SET_SCHED_OFF_MINUTE) {
            sched_ptr->off_minute = temp_set_minute;
            current_mode = SET_SCHED_OFF_SECOND;
        } 
        else if (current_mode == SET_SCHED_OFF_SECOND) { 
            sched_ptr->off_second = temp_set_second;
            current_mode = SET_SCHED_OFF_AMPM;
        } 
        else if (current_mode == SET_SCHED_OFF_AMPM) {
            // Step 8: Saves OFF Time and exits setup
            sched_ptr->off_am = temp_set_am;

            // Permanently store the newly entered ON/OFF timing.
            int savedChannel = active_schedule_channel;
            bool conflictFound = hasScheduleConflictForChannel(savedChannel);
            saveSchedulesToEEPROM();

            // FINISH SETUP & SAVE
            current_mode = NORMAL_MODE;
            active_schedule_channel = 0;
            clearLCD();

            if (conflictFound) {
                showConflictWarning(savedChannel);
            } else {
                lcd.print("CH"); lcd.print(savedChannel); lcd.print(" S"); lcd.print(index + 1); lcd.print(" SAVED");
                lcd.setCursor(0, 1);
                lcd.print("NO CONFLICT");
                delay(1500);
                clearLCD();
            }
            return;
        }
        
        // --- Update LCD Prompt for the Next Step ---
        clearLCD();
        lcd.print("CH"); lcd.print(active_schedule_channel); 
        lcd.print(" S"); lcd.print(index + 1); lcd.print(" ");

        bool is_on = (current_mode <= SET_SCHED_ON_AMPM); 

        lcd.print(is_on ? "ON " : "OFF ");
        
        if (current_mode == SET_SCHED_ON_HOUR || current_mode == SET_SCHED_OFF_HOUR) {
            lcd.print("Hour:");
        } else if (current_mode == SET_SCHED_ON_MINUTE || current_mode == SET_SCHED_OFF_MINUTE) {
            lcd.print("Minute:");
        } else if (current_mode == SET_SCHED_ON_SECOND || current_mode == SET_SCHED_OFF_SECOND) {
            lcd.print("Second:");
        } else if (current_mode == SET_SCHED_ON_AMPM || current_mode == SET_SCHED_OFF_AMPM) {
            lcd.print("AM/PM:");
        }
    }

    // --- Update LCD value only when its text actually changes ---
    if (current_mode != NORMAL_MODE && current_mode != SLOT_SELECT_MODE && current_mode != CREDITS_MODE) {
        char settingLine[17];

        if (current_mode == SET_SCHED_ON_HOUR || current_mode == SET_SCHED_OFF_HOUR) {
            snprintf(settingLine, sizeof(settingLine), "%d %s", temp_set_hour, temp_set_am ? "AM" : "PM");
        } else if (current_mode == SET_SCHED_ON_MINUTE || current_mode == SET_SCHED_OFF_MINUTE) {
            snprintf(settingLine, sizeof(settingLine), "%02d", temp_set_minute);
        } else if (current_mode == SET_SCHED_ON_SECOND || current_mode == SET_SCHED_OFF_SECOND) {
            snprintf(settingLine, sizeof(settingLine), "%02d", temp_set_second);
        } else {
            snprintf(settingLine, sizeof(settingLine), "%s", temp_set_am ? "AM" : "PM");
        }

        printLCDLineIfChanged(1, settingLine);
    }
}

// --- Setup Function (Runs once on power-up or reset) ---
void setup() {
    Serial.begin(115200);

    // --- I2C LCD Initialization ---
    // LCD is initialized first because RTC setup messages are displayed on it.
    lcd.init();
    lcd.backlight();
    lcdBacklightIsOn = true;
    lastBacklightActivityMillis = millis();
    clearLCD();

    if (!rtc.begin()) {
        Serial.println("Couldn't find RTC module. Check wiring!");
        lcd.print("RTC NOT FOUND!");
        lcd.setCursor(0, 1);
        lcd.print("CHECK WIRING");
        while (1);
    }

    // First upload only: set DS3231 to the computer's date/time at compile time.
    if (SET_RTC_TIME_ON_UPLOAD) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        Serial.println("RTC updated to computer compilation time.");
        lcd.print("RTC TIME SET!");
        lcd.setCursor(0, 1);
        lcd.print("SET FLAG FALSE");
        delay(2000);
        clearLCD();
    }
    // IMPORTANT: Never set the RTC again automatically during normal startup.
    // If backup power fails, show a warning but do NOT rewrite the upload time.
    else if (rtc.lostPower()) {
        Serial.println("WARNING: RTC reports lost power. Check coin-cell battery, holder and RTC module.");
        lcd.print("RTC BATTERY ERR");
        lcd.setCursor(0, 1);
        lcd.print("CHECK MODULE");
        delay(2000);
        clearLCD();
    }

    // Restore manually entered ON/OFF timing after total power cutoff.
    bool previousTimesFound = loadSchedulesFromEEPROM();
    clearLCD();
    if (previousTimesFound) {
        lcd.print("TIMES RESTORED");
        lcd.setCursor(0, 1);
        lcd.print("FROM MEMORY");
    } else {
        lcd.print("SET TIMES ONCE");
        lcd.setCursor(0, 1);
        lcd.print("MEMORY READY");
    }
    delay(1200);
    clearLCD();

    // Startup splash screen
    lcd.print("SYSTEM ONLINE"); delay(500);
    lcd.print("."); delay(500); lcd.print("."); delay(500); lcd.print("."); delay(1500);
    clearLCD();

    // Set pin modes
    pinMode(RELAY1_PIN, OUTPUT);
    pinMode(RELAY2_PIN, OUTPUT);
    pinMode(BUTTON1_PIN, INPUT_PULLUP);
    pinMode(BUTTON2_PIN, INPUT_PULLUP);
    pinMode(BUTTON3_PIN, INPUT_PULLUP);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Initialize relays to OFF
    setRelayState(RELAY1_PIN, false, 1);
    setRelayState(RELAY2_PIN, false, 2);

    // Watchdog ON: if Arduino freezes, it restarts automatically within about 8 seconds.
    wdt_enable(WDTO_8S);
}

// --- Loop Function (Runs repeatedly) ---
void loop() {
    handleBacklightAutoOff();

    // Feature V5: restart Arduino once per day for long-time stability.
    handleDailyAutoRestart();
    // Watchdog reset: proves the program is still running.
    // If the program freezes, Arduino auto-restarts.
    wdt_reset();

    // LCD recovery: helps if only the I2C LCD goes blank/hangs.
    recoverLCDIfNeeded();

    // Hold B1 + B2 for 2 seconds to erase every stored ON/OFF time.
    // Works in NORMAL display, SLOT SELECT, and ON/OFF SETTING screens.
    if (current_mode != CREDITS_MODE && handleMemoryEraseButtons()) {
        delay(10);
        return;
    }

    if (current_mode == NORMAL_MODE) {
        // === Button Handling in NORMAL_MODE: Enter Setup Modes ===
        
        // B1 + B2 + B3 Simultaneous Press: Enter Credits Mode (Highest Priority)
        if (readThreeButtonsSimultaneously(BUTTON1_PIN, BUTTON2_PIN, BUTTON3_PIN)) {
            resetNormalButtonTracking();
            current_mode = CREDITS_MODE;
            clearLCD();
            scroll_position = 0; // Reset scroll position
            return;
        }
        // B1 + B2 mode removed: pressing B1 and B2 together does nothing.
        // B1 + B2 held for 2 seconds erases all saved ON/OFF timings.
        // Single-button actions:
        // Short B1/B2 = select S1/S2/S3, then edit schedule
        // Long B1/B2  = manual override for that channel
        // Long B3     = return both channels to AUTO mode
        if (handleNormalModeButtonActions()) {
            return;
        }

        // Continue with automatic control
        checkAutomaticControl();

        // Update the top line (time)
        updateTimeOnLCD();

        // === Display Logic to Show Current Relay Status Immediately ===
        unsigned long currentMillis = millis();
        bool ch1_on = digitalRead(RELAY1_PIN) == LOW;
        bool ch2_on = digitalRead(RELAY2_PIN) == LOW;

        bool show_ch1_status = ch1_on || channelIsManual(1);
        bool show_ch2_status = ch2_on || channelIsManual(2);

        if (show_ch1_status && show_ch2_status) {
            // Alternate when both channels are ON or manually controlled.
            if (currentMillis - previousMillisDisplaySwitch >= intervalDisplaySwitch) {
                previousMillisDisplaySwitch = currentMillis;
                if (current_display_state == DISPLAY_CH1_STATUS) {
                    current_display_state = DISPLAY_CH2_STATUS;
                } else {
                    current_display_state = DISPLAY_CH1_STATUS;
                }
            }
        } else if (show_ch1_status) {
            current_display_state = DISPLAY_CH1_STATUS;
        } else if (show_ch2_status) {
            current_display_state = DISPLAY_CH2_STATUS;
        } else {
            current_display_state = DISPLAY_ALL_OFF;
        }

        // Update the bottom line based on the determined state
        if (isRelayRestTimeNow()) {
            printLCDLineIfChanged(1, "RELAY REST MODE");
        } else if (current_display_state == DISPLAY_CH1_STATUS) {
            displayChannelStatus(1, digitalRead(RELAY1_PIN));
    
    } else if (current_display_state == DISPLAY_CH2_STATUS) {
            displayChannelStatus(2, digitalRead(RELAY2_PIN));
        } else if (current_display_state == DISPLAY_ALL_OFF) {
            displayAllChannelsOff();
        }

    } else if (current_mode == SLOT_SELECT_MODE) {
        // Select S1, S2 or S3 for the chosen channel.
        handleSlotSelectMode();
    } else if (current_mode == CREDITS_MODE) {
        // Handle scrolling credits display
        handleCreditsMode();
    } else { 
        // In schedule setting mode:
        handleSetupMode();
    }

    delay(10); 
}