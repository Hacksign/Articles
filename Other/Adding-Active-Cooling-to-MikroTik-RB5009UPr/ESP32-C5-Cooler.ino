/**
 * XIAO ESP32C5 - Multi-task Sensor & Fan Controller (With Persistent Thresholds)
 *
 * Hardware Assumptions (PLEASE VERIFY):
 * - AHT20 I2C: SDA=D4(GPIO22), SCL=D5(GPIO23) [XIAO Default]
 * - Fan PWM Output: D6 (GPIO2)
 * - Fan Tachometer Input: D7 (GPIO3)
 *
 * Dependencies:
 * - Adafruit AHTX0
 * - ArduinoJson
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_AHTX0.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// ==================== PIN DEFINITIONS ====================
#define PIN_FAN_PWM     25    // D6 on XIAO ESP32C5 - CHANGE IF NEEDED
#define PIN_FAN_TACH    7     // D7 on XIAO ESP32C5 - CHANGE IF NEEDED
#define FAN_PWM_FREQ    25000
#define FAN_PWM_RES     8
#define FAN_PWM_CHANNEL 0

// ==================== WIFI CONFIGURATION ====================
const char* DEVICE_NAME   = "ESP32-C5-Cooler";
const char* WIFI_SSID = "YOUR WIRELESS SSID";           // ← CHANGE THIS
const char* WIFI_PASSWORD = "YOUR WIRELESS PASSWORD";   // ← CHANGE THIS
static volatile bool g_wifiConnected = false;
static unsigned long g_lastReconnectAttempt = 0;
static uint32_t g_reconnectDelayMs = 1000;
#define MAX_RECONNECT_DELAY_MS 60000

const int HTTP_PORT       = 80;

// ==================== GLOBAL SHARED DATA ====================
struct SharedData {
    float temperature = NAN;
    float humidity    = NAN;
    uint8_t fanTargetPercent = 0;
    float fanRPM = 0.0f;
    bool ahtValid = false;
    bool fanManualMode = false;
    // [MODIFIED] Thresholds are now part of shared data with defaults
    uint8_t lowThreshold = 40;
    uint8_t lowTargetPercent = 0;
    uint8_t mediumThreshold = 50;
    uint8_t mediumTargetPercent = 50;
    uint8_t highThreshold = 60;
    uint8_t highTargetPercent = 100;
};

static SharedData g_data;
static SemaphoreHandle_t g_mutex = nullptr;
static Preferences prefs;

// ==================== PERIPHERALS ====================
static Adafruit_AHTX0 aht;
static WebServer server(HTTP_PORT);

// ==================== TACHOMETER VARIABLES ====================
static volatile unsigned long tachPulseCount = 0;
static volatile unsigned long tachLastMicros = 0;

// ==================== FORWARD DECLARATIONS ====================
void taskA_AHTRead(void* param);
void taskB_FanControl(void* param);
void taskC_FanFeedback(void* param);
void taskD_HTTPServer(void* param);
void setupHTTPRoutes();
float calculateRPM();
void setFanPWM(uint8_t percent);
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);
void handleWiFiReconnect();
void loadThresholdsFromNVS();
void saveThresholdsToNVS();

// ============================================================
//                        SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== XIAO ESP32C5 Multi-Task Controller ===");

    // Create mutex
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) {
        Serial.println("FATAL: Failed to create mutex!");
        while (1) delay(1000);
    }

    loadThresholdsFromNVS();

    // --- Initialize AHT20 ---
    Wire.begin();
    if (!aht.begin()) {
        Serial.println("ERROR: AHT20 initialization failed! Check wiring.");
    } else {
        Serial.println("OK: AHT20 initialized successfully.");
    }

    // --- Initialize Fan PWM ---
    ledcAttachChannel(PIN_FAN_PWM, FAN_PWM_FREQ, FAN_PWM_RES, FAN_PWM_CHANNEL);
    setFanPWM(0);
    Serial.printf("OK: Fan PWM initialized on GPIO%d\n", PIN_FAN_PWM);

    // --- Initialize Fan Tachometer ---
    pinMode(PIN_FAN_TACH, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_FAN_TACH), []{
        tachPulseCount++;
    }, FALLING);
    tachLastMicros = micros();
    Serial.printf("OK: Fan Tach initialized on GPIO%d\n", PIN_FAN_TACH);

    // --- Initialize WiFi ---
    Serial.printf("[WiFi] Connecting to '%s'...\n", WIFI_SSID);
    WiFi.setHostname(DEVICE_NAME);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.onEvent(onWiFiEvent);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    g_lastReconnectAttempt = millis();

    // --- Setup HTTP Routes ---
    setupHTTPRoutes();
    server.begin();
    Serial.printf("OK: HTTP Server started on port %d\n", HTTP_PORT);

    // --- Create FreeRTOS Tasks ---
    xTaskCreatePinnedToCore(taskA_AHTRead, "TaskA_AHT", 4096, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(taskB_FanControl, "TaskB_FanCtrl", 4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(taskC_FanFeedback, "TaskC_FanFB", 4096, nullptr, 2, nullptr, 0);
    xTaskCreatePinnedToCore(taskD_HTTPServer, "TaskD_HTTP", 8192, nullptr, 1, nullptr, 0);

    Serial.println("All tasks created. System running.\n");
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}

// ==================== NVS PERSISTENCE ==================== // [NEW SECTION]

/**
 * Load thresholds from NVS. If keys don't exist, keep struct defaults.
 */
void loadThresholdsFromNVS() {
    prefs.begin("fan_ctrl", true);  // read-only
    g_data.lowThreshold             = prefs.getUChar("low_th",  g_data.lowThreshold);
    g_data.lowTargetPercent         = prefs.getUChar("low_percent",  g_data.lowTargetPercent);
    g_data.mediumThreshold          = prefs.getUChar("med_th",  g_data.mediumThreshold);
    g_data.mediumTargetPercent      = prefs.getUChar("med_percent",  g_data.mediumTargetPercent);
    g_data.highThreshold            = prefs.getUChar("high_th", g_data.highThreshold);
    g_data.highTargetPercent        = prefs.getUChar("high_percent",  g_data.highTargetPercent);
    prefs.end();
    Serial.printf("[NVS] Loaded thresholds: low=%d percent=%d, med=%d percent=%d, high=%d percent=%d\n",
                  g_data.lowThreshold, g_data.lowTargetPercent,
                  g_data.mediumThreshold, g_data.mediumTargetPercent,
                  g_data.highThreshold, g_data.highTargetPercent);
}

/**
 * Save current thresholds to NVS (called under mutex protection).
 */
void saveThresholdsToNVS() {
    prefs.begin("fan_ctrl", false);  // read-write
    prefs.putUChar("low_th",  g_data.lowThreshold);
    prefs.putUChar("low_percent",  g_data.lowTargetPercent);
    prefs.putUChar("med_th",  g_data.mediumThreshold);
    prefs.putUChar("med_percent",  g_data.mediumTargetPercent);
    prefs.putUChar("high_th", g_data.highThreshold);
    prefs.putUChar("high_percent",  g_data.highTargetPercent);
    prefs.end();
    Serial.printf("[NVS] Saved thresholds: low=%d percent=%d, med=%d percent=%d, high=%d percent=%d\n",
                  g_data.lowThreshold, g_data.lowTargetPercent,
                  g_data.mediumThreshold, g_data.mediumTargetPercent,
                  g_data.highThreshold, g_data.highTargetPercent);
}

// ==================== WIFI EVENT HANDLER ====================
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("[WiFi] Connected to AP.");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            g_wifiConnected = true;
            g_reconnectDelayMs = 1000;
            Serial.printf("[WiFi] Got IP: %s\n", WiFi.localIP().toString().c_str());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            g_wifiConnected = false;
            Serial.printf("[WiFi] Disconnected! Reason: %d. Will reconnect.\n",
                          info.wifi_sta_disconnected.reason);
            break;
        default:
            break;
    }
}

void handleWiFiReconnect() {
    if (g_wifiConnected) return;
    unsigned long now = millis();
    if (now - g_lastReconnectAttempt >= g_reconnectDelayMs) {
        Serial.printf("[WiFi] Reconnecting... (next retry in %lu ms)\n", g_reconnectDelayMs);
        WiFi.reconnect();
        g_lastReconnectAttempt = now;
        g_reconnectDelayMs = min(g_reconnectDelayMs * 2, (unsigned long)MAX_RECONNECT_DELAY_MS);
    }
}

// ============================================================
//               TASK A: AHT20 Read (Every 6 seconds)
// ============================================================
void taskA_AHTRead(void* param) {
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(6000);
    for (;;) {
        sensors_event_t humidityEvent, tempEvent;
        bool success = aht.getEvent(&humidityEvent, &tempEvent);
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (success && !isnan(tempEvent.temperature) && !isnan(humidityEvent.relative_humidity)) {
                g_data.temperature = tempEvent.temperature;
                g_data.humidity = humidityEvent.relative_humidity;
                g_data.ahtValid = true;
                Serial.printf("[TaskA] AHT20 OK: T=%.2f°C H=%.2f%%\n",
                              g_data.temperature, g_data.humidity);
            } else {
                g_data.ahtValid = false;
                Serial.println("[TaskA] AHT20 READ FAILED! Will retry next cycle.");
            }
            xSemaphoreGive(g_mutex);
        } else {
            Serial.println("[TaskA] Mutex timeout!");
        }
        vTaskDelayUntil(&lastWake, interval);
    }
}

// ============================================================
//         TASK B: Fan Speed Control (Every 10 seconds)
// ============================================================
void taskB_FanControl(void* param) {
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(10000);
    for (;;) {
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (g_data.ahtValid) {
                if (!g_data.fanManualMode) {
                    // [MODIFIED] Use dynamic thresholds from g_data instead of hardcoded values
                    uint8_t newDuty = 0;
                    float temp = g_data.temperature;
                    uint8_t lowTh  = g_data.lowThreshold;
                    uint8_t medTh  = g_data.mediumThreshold;
                    uint8_t highTh = g_data.highThreshold;

                    if (temp < lowTh * 1.0) {
                        newDuty = g_data.lowTargetPercent;
                    } else if ( lowTh * 1.0 <= temp && temp < medTh * 1.0) {
                        newDuty = g_data.mediumTargetPercent;
                    } else if (medTh * 1.0 <= temp && temp < highTh * 1.0) {
                        newDuty = g_data.highTargetPercent;
                    } else {
                        newDuty = 100;
                    }

                    g_data.fanTargetPercent = constrain(newDuty, 0, 100);
                    setFanPWM(g_data.fanTargetPercent);
                    Serial.printf("[TaskB] Temp=%.1f°C → Fan=%d%% (thresholds: %d/%d/%d)\n",
                                  temp, g_data.fanTargetPercent, lowTh, medTh, highTh);
                } else {
                    Serial.println("[TaskB] Manual mode active, skipping auto control.");
                }
            } else {
                Serial.println("[TaskB] No valid sensor data. Keeping current fan speed.");
            }
            xSemaphoreGive(g_mutex);
        }
        vTaskDelayUntil(&lastWake, interval);
    }
}

// ============================================================
//         TASK C: Fan RPM Feedback (Every 2 seconds)
// ============================================================
void taskC_FanFeedback(void* param) {
    TickType_t lastWake = xTaskGetTickCount();
    const TickType_t interval = pdMS_TO_TICKS(2000);
    for (;;) {
        float rpm = calculateRPM();
        if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_data.fanRPM = rpm;
            Serial.printf("[TaskC] Fan RPM: %.1f (Target: %d%%)\n",
                          g_data.fanRPM, g_data.fanTargetPercent);
            xSemaphoreGive(g_mutex);
        }
        vTaskDelayUntil(&lastWake, interval);
    }
}

// ============================================================
//              TASK D: HTTP Server Handler
// ============================================================
void taskD_HTTPServer(void* param) {
    for (;;) {
        handleWiFiReconnect();
        server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================== HTTP ROUTE HANDLERS ====================
/**
 * GET /api/status
 * [MODIFIED] Now includes threshold fields
 */
void handleGetStatus() {
    JsonDocument doc;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (g_data.ahtValid) {
            doc["temperature"] = g_data.temperature;
            doc["humidity"] = g_data.humidity;
        } else {
            doc["temperature"] = nullptr;
            doc["humidity"] = nullptr;
        }
        doc["fan_rpm"] = g_data.fanRPM;
        doc["fan_target_percent"] = g_data.fanTargetPercent;
        doc["sensor_valid"] = g_data.ahtValid;
        doc["fan_manual_mode"] = g_data.fanManualMode;
        doc["low_threshold"] = g_data.lowThreshold;
        doc["low_target_percent"] = g_data.lowTargetPercent;
        doc["medium_threshold"] = g_data.mediumThreshold;
        doc["medium_target_percent"] = g_data.mediumTargetPercent;
        doc["high_threshold"] = g_data.highThreshold;
        doc["high_target_percent"] = g_data.highTargetPercent;
        xSemaphoreGive(g_mutex);
    } else {
        doc["error"] = "Resource busy, try again";
    }
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

/**
 * POST /api/fan
 */
void handleSetFan() {
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"error\":\"Method not allowed\"}");
        return;
    }
    JsonDocument inDoc;
    DeserializationError err = deserializeJson(inDoc, server.arg("plain"));
    if (err) {
        JsonDocument errDoc;
        errDoc["error"] = "Invalid JSON: " + String(err.c_str());
        String resp;
        serializeJson(errDoc, resp);
        server.send(400, "application/json", resp);
        return;
    }
    JsonDocument outDoc;
    int errorCode = 500;
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!inDoc.containsKey("speed")) {
            errorCode = 400;
            outDoc["success"] = false;
            outDoc["error"] = "Missing 'speed' field";
        } else {
            int speed = inDoc["speed"].as<int>();
            if (speed < 0) {
                errorCode = 200;
                g_data.fanManualMode = false;
                outDoc["success"] = true;
                outDoc["message"] = "fan exited manual mode";
            } else if (speed > 100) {
                errorCode = 400;
                outDoc["success"] = false;
                outDoc["error"] = "'speed' must be 0-100 or -1";
            } else {
                errorCode = 200;
                g_data.fanTargetPercent = (uint8_t)speed;
                g_data.fanManualMode = true;
                setFanPWM(g_data.fanTargetPercent);
                outDoc["success"] = true;
                outDoc["fan_target_percent"] = g_data.fanTargetPercent;
                Serial.printf("[HTTP] Fan speed set to %d%% via REST API\n", speed);
            }
        }
        xSemaphoreGive(g_mutex);
    } else {
        errorCode = 503;
        outDoc["success"] = false;
        outDoc["error"] = "Resource busy";
    }
    String response;
    serializeJson(outDoc, response);
    server.send(errorCode, "application/json", response);
}

// ==================== THRESHOLD ENDPOINT ====================
/**
 * POST /api/threshold
 * Body JSON:
    {
        "low_threshold":N,
        "low_target_percent": M, 
        "medium_threshold":N,
        "medium_target_percent": M,
        "high_threshold":N,
        "high_target_percent": M
    }
 * All three fields are REQUIRED. Values must satisfy: low_threshold < medium_threshold < high_threshold
 * Persists to NVS on success.
 */
void handleSetThreshold() {
    if (server.method() != HTTP_POST) {
        server.send(405, "application/json", "{\"error\":\"Method not allowed\"}");
        return;
    }

    JsonDocument inDoc;
    DeserializationError err = deserializeJson(inDoc, server.arg("plain"));
    if (err) {
        JsonDocument errDoc;
        errDoc["error"] = "Invalid JSON: " + String(err.c_str());
        String resp;
        serializeJson(errDoc, resp);
        server.send(400, "application/json", resp);
        return;
    }

    JsonDocument outDoc;
    int errorCode = 400;

    // Validate all required fields exist
    if (!inDoc.containsKey("low_threshold") || !inDoc.containsKey("medium_threshold") || !inDoc.containsKey("high_threshold") ||
        !inDoc.containsKey("low_target_percent") || !inDoc.containsKey("medium_target_percent") || !inDoc.containsKey("high_target_percent")) {
        outDoc["success"] = false;
        outDoc["error"] = "Missing fields. Required: {\"low_threshold\":N, \"low_target_percent\": M, "
            "\"medium_threshold\":N, \"medium_target_percent\": M, \"high_threshold\":N, \"high_target_percent\": M}";
        String resp;
        serializeJson(outDoc, resp);
        server.send(400, "application/json", resp);
        return;
    }

    int low_threshold  = inDoc["low_threshold"].as<int>();
    int low_target_percent  = inDoc["low_target_percent"].as<int>();
    int medium_threshold  = inDoc["medium_threshold"].as<int>();
    int medium_target_percent  = inDoc["medium_target_percent"].as<int>();
    int high_thresholdh = inDoc["high_threshold"].as<int>();
    int high_target_percent  = inDoc["high_target_percent"].as<int>();

    // Ordering validation
    if (!(low_threshold < medium_threshold && medium_threshold < high_thresholdh)) {
        outDoc["success"] = false;
        outDoc["error"] = "Must satisfy: low_threshold < medium_threshold < hihigh_thresholdgh";
        String resp;
        serializeJson(outDoc, resp);
        server.send(400, "application/json", resp);
        return;
    }

    // Apply under mutex
    if (xSemaphoreTake(g_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_data.lowThreshold    = (uint8_t)low_threshold;
        g_data.lowTargetPercent = (uint8_t)low_target_percent;
        g_data.mediumThreshold = (uint8_t)medium_threshold;
        g_data.mediumTargetPercent = (uint8_t)medium_target_percent;
        g_data.highThreshold   = (uint8_t)high_thresholdh;
        g_data.highTargetPercent = (uint8_t)high_target_percent;

        // Persist to NVS while still holding mutex (NVS write is fast ~ms)
        saveThresholdsToNVS();

        outDoc["success"] = true;
        outDoc["low_threshold"] = g_data.lowThreshold;
        outDoc["low_target_percent"] = g_data.lowTargetPercent;
        outDoc["medium_threshold"] = g_data.mediumThreshold;
        outDoc["medium_target_percent"] = g_data.mediumTargetPercent;
        outDoc["high_threshold"] = g_data.highThreshold;
        outDoc["high_target_percent"] = g_data.highTargetPercent;
        outDoc["message"] = "Thresholds updated and saved to NVS";
        errorCode = 200;

        Serial.printf("[HTTP] Thresholds updated: low=%d percent=%d, med=%d percent=%d, high=%d percent=%d\n",
                  g_data.lowThreshold, g_data.lowTargetPercent,
                  g_data.mediumThreshold, g_data.mediumTargetPercent,
                  g_data.highThreshold, g_data.highTargetPercent);
        xSemaphoreGive(g_mutex);
    } else {
        errorCode = 503;
        outDoc["success"] = false;
        outDoc["error"] = "Resource busy";
    }

    String response;
    serializeJson(outDoc, response);
    server.send(errorCode, "application/json", response);
}

// ==================== ROUTE REGISTRATION ====================
void setupHTTPRoutes() {
    server.on("/api/status", HTTP_GET, handleGetStatus);
    server.on("/api/fan", HTTP_POST, handleSetFan);
    server.on("/api/threshold", HTTP_POST, handleSetThreshold);

    server.on("/", HTTP_GET, []{
        JsonDocument doc;
        doc["device"] = "XIAO_ESP32C5_Controller";
        doc["endpoints"]["GET /api/status"] = "Read temp, humidity, fan RPM, thresholds";
        doc["endpoints"]["POST /api/fan"] = "Set fan speed {\"speed\": 0-100} or exit manual {\"speed\": -1}";
        doc["endpoints"]["POST /api/threshold"] = "Set thresholds {\"low_threshold\":N, \"low_target_percent\": M, "
            "\"medium_threshold\":N, \"medium_target_percent\": M, \"high_threshold\":N, \"high_target_percent\": M}"
            ", persisted to NVS";
        String resp;
        serializeJson(doc, resp);
        server.send(200, "application/json", resp);
    });

    server.onNotFound([]{
        server.send(404, "application/json", "{\"error\":\"Not found\"}");
    });
}

// ==================== HELPER FUNCTIONS ====================
void setFanPWM(uint8_t percent) {
    uint32_t duty = map(constrain(percent, 0, 100), 0, 100, 0, (1 << FAN_PWM_RES) - 1);
    ledcWriteChannel(FAN_PWM_CHANNEL, duty);
}

float calculateRPM() {
    unsigned long count;
    unsigned long now;
    noInterrupts();
    count = tachPulseCount;
    now   = micros();
    tachPulseCount = 0;
    interrupts();

    unsigned long elapsed;
    if (now >= tachLastMicros) {
        elapsed = now - tachLastMicros;
    } else {
        elapsed = 0xFFFFFFFFUL - tachLastMicros + now + 1;
    }
    tachLastMicros = now;

    if (elapsed == 0 || count == 0) return 0.0f;

    const float PULSES_PER_REV = 2.0f;
    float revolutions = count / PULSES_PER_REV;
    float seconds     = elapsed / 1000000.0f;
    float rpm = (revolutions / seconds) * 60.0f;
    return rpm;
}
