#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c.h"

// Notice: Edge Impulse library has been completely removed. We are using pure deterministic math.

static const char *TAG = "Deterministic_Motor";

// Actuator Pin Definitions
#define PIN_MOTOR_IN1 4
#define PIN_MOTOR_IN2 5
#define PIN_LED_GREEN 6
#define PIN_LED_RED   7

// Throttled Speed 
#define MOTOR_SPEED 150

// --- STATE MACHINE & DEBOUNCING ---
enum SystemState { STATE_IDLE, STATE_RUNNING, STATE_FAULT_LATCHED };
volatile SystemState currentState = STATE_IDLE;

volatile int healthy_count = 0;
String latest_log = "System Idle. Awaiting Start Command...";

// --- WI-FI & WEB SERVER ---
const char* ssid = "Harsha_AI_Motor";
const char* password = "password123";
WebServer server(80);

// Live Dashboard HTML & JavaScript (Upgraded with UTF-8 and matching trigger words)
const char* html_page = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; text-align: center; background-color: #0d1117; color: #c9d1d9; padding-top: 20px; transition: background-color 0.3s;}
  .btn { padding: 15px 30px; font-size: 20px; margin: 10px; cursor: pointer; border: none; border-radius: 8px; color: white; font-weight: bold; width: 80%; max-width: 300px;}
  .start { background-color: #238636; }
  .stop { background-color: #da3633; }
  .terminal { background-color: #010409; border: 1px solid #30363d; border-radius: 8px; width: 90%; max-width: 600px; height: 300px; margin: 20px auto; overflow-y: auto; text-align: left; padding: 15px; font-family: 'Courier New', Courier, monospace; font-size: 14px; box-shadow: inset 0 0 10px #000;}
  .log-entry { margin-bottom: 5px; border-bottom: 1px dashed #30363d; padding-bottom: 5px; }
  h1 { font-size: 2.5rem; margin-bottom: 5px;}
  #status-header { font-size: 1.5rem; color: #8b949e; margin-top: 0; font-weight: bold;}
</style>
<script>
  // TURBO POLLING: Checks the ESP32 every 250ms for lightning-fast response
  setInterval(function() {
    fetch('/status').then(response => response.text()).then(text => {
      if(text !== "") {
        let term = document.getElementById('terminal-box');
        let statusH = document.getElementById('status-header');
        let color = "#c9d1d9"; 
        
        // Dynamic UI Screaming Logic (Now looking for the word "FAULT")
        if(text.includes("FAULT")) {
            color = "#ff7b72";
            document.body.style.backgroundColor = "#4a0000"; 
            statusH.innerHTML = "⚠️ MALFUNCTION DETECTED - WHEEL STOPPED ⚠️";
            statusH.style.color = "#ff7b72";
        } 
        else if(text.includes("System Healthy")) {
            color = "#3fb950";
            document.body.style.backgroundColor = "#0d1117"; 
            statusH.innerHTML = "✅ SYSTEM HEALTHY ✅";
            statusH.style.color = "#3fb950";
        }
        else if(text.includes("Idle")) {
            document.body.style.backgroundColor = "#0d1117";
            statusH.innerHTML = "Live Motor Analytics";
            statusH.style.color = "#8b949e";
        }

        let newLog = "<div class='log-entry' style='color:" + color + "'>" + text + "</div>";
        term.innerHTML = newLog + term.innerHTML; 
      }
    });
  }, 250); 
</script>
</head><body>
  <h1>Industrial Motor Monitor</h1>
  <div id="status-header">Live Motor Analytics</div>
  
  <button class="btn start" onclick="fetch('/start')">START / RESET SYSTEM</button><br>
  <button class="btn stop" onclick="fetch('/stop')">MANUAL OVERRIDE STOP</button>
  
  <div class="terminal" id="terminal-box">
    <div class='log-entry'>[SYSTEM BOOT] Web server initialized. Awaiting commands...</div>
  </div>
</body></html>
)rawliteral";

void handleRoot() { server.send(200, "text/html", html_page); }

void handleStart() {
    currentState = STATE_RUNNING;
    healthy_count = 0;
    latest_log = "[CMD] Motor Started. Calibrating physics baseline...";
    server.send(200, "text/plain", "Started");
}

void handleStop() {
    currentState = STATE_IDLE;
    latest_log = "[CMD] Manual Stop Engaged. System Idle.";
    server.send(200, "text/plain", "Stopped");
}

void handleStatus() {
    server.send(200, "text/plain", latest_log);
    latest_log = ""; // Clear after sending
}

// I2C parameters
#define I2C_MASTER_SCL_IO           9
#define I2C_MASTER_SDA_IO           8
#define I2C_MASTER_NUM              I2C_NUM_0
#define I2C_MASTER_FREQ_HZ          10000 
#define MPU6050_SENSOR_ADDR         0x68
#define MPU6050_PWR_MGMT_1_REG      0x6B
#define MPU6050_ACCEL_XOUT_H_REG    0x3B

static esp_err_t i2c_master_init(void) {
    i2c_port_t i2c_master_port = I2C_MASTER_NUM;
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_IO;
    conf.scl_io_num = I2C_MASTER_SCL_IO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;
    i2c_param_config(i2c_master_port, &conf);
    return i2c_driver_install(i2c_master_port, conf.mode, 0, 0, 0);
}

static esp_err_t mpu6050_wake_up() {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, MPU6050_PWR_MGMT_1_REG, true);
    i2c_master_write_byte(cmd, 0x00, true); 
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK) return ret;

    // +-16g Scale for extreme vibrations
    cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, 0x1C, true); 
    i2c_master_write_byte(cmd, 0x18, true); 
    i2c_master_stop(cmd);
    ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t mpu6050_read_accel(int16_t *accel_x, int16_t *accel_y, int16_t *accel_z) {
    uint8_t data[6];
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, MPU6050_ACCEL_XOUT_H_REG, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MPU6050_SENSOR_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 5, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, data + 5, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 1000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        *accel_x = (data[0] << 8) | data[1];
        *accel_y = (data[2] << 8) | data[3];
        *accel_z = (data[4] << 8) | data[5];
    }
    return ret;
}

void vInferenceTask(void *pvParameters) {
    int16_t ax, ay, az;

    while (1) {
        if (currentState == STATE_IDLE || currentState == STATE_FAULT_LATCHED) {
            if(currentState == STATE_IDLE) {
                analogWrite(PIN_MOTOR_IN1, 0);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_RED, LOW);
            } else {
                analogWrite(PIN_MOTOR_IN1, 0);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_RED, HIGH); 
            }
            digitalWrite(PIN_MOTOR_IN2, LOW);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (currentState == STATE_RUNNING) {
            analogWrite(PIN_MOTOR_IN1, MOTOR_SPEED);
            digitalWrite(PIN_MOTOR_IN2, LOW);

            // Tracking the Z-Axis!
            int16_t max_z = -32768;
            int16_t min_z = 32767;

            // Take 100 rapid-fire samples of pure physics
            for (int i = 0; i < 100; i++) {
                if (mpu6050_read_accel(&ax, &ay, &az) == ESP_OK) {
                    // Tracking 'az' (Z-axis) for the vertical bounce
                    if (az > max_z) max_z = az;
                    if (az < min_z) min_z = az;
                }
                vTaskDelay(pdMS_TO_TICKS(10)); 
            }

            // Calculate the total physical variance (Peak-to-Peak Amplitude) on the Z-Axis
            int32_t p2p_shake = max_z - min_z;

            // --- DETERMINISTIC KILL SWITCH ---
            
            // Adjust this threshold based on what numbers you see on your dashboard
            int threshold = 4250; 

            if (p2p_shake > threshold) {
                // INSTANT TRIP
                latest_log = "!!! FAULT (Shake: " + String(p2p_shake) + ") - WHEEL STOPPED !!!";
                
                analogWrite(PIN_MOTOR_IN1, 0);
                digitalWrite(PIN_MOTOR_IN2, LOW);
                digitalWrite(PIN_LED_GREEN, LOW);
                digitalWrite(PIN_LED_RED, HIGH);
                
                currentState = STATE_FAULT_LATCHED; 
            } 
            else {
                healthy_count++;
                if (healthy_count >= 2) {
                    latest_log = "-> System Healthy (Shake: " + String(p2p_shake) + ")";
                    digitalWrite(PIN_LED_GREEN, HIGH);
                    digitalWrite(PIN_LED_RED, LOW);
                    if(healthy_count > 100) healthy_count = 2; 
                } else {
                    latest_log = "-> Calibrating... " + String(healthy_count) + "/2";
                }
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000); 

    pinMode(PIN_MOTOR_IN1, OUTPUT);
    pinMode(PIN_MOTOR_IN2, OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED, OUTPUT);
    analogWrite(PIN_MOTOR_IN1, 0);
    digitalWrite(PIN_MOTOR_IN2, LOW);
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, LOW);

    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("Web Server IP: "); Serial.println(IP);

    server.on("/", handleRoot);
    server.on("/start", handleStart);
    server.on("/stop", handleStop);
    server.on("/status", handleStatus);
    server.begin();

    ESP_ERROR_CHECK(i2c_master_init());
    if (mpu6050_wake_up() == ESP_OK) {
        xTaskCreatePinnedToCore(vInferenceTask, "InferenceTask", 8192, NULL, 5, NULL, 0);
    }
}

void loop() {
    server.handleClient();
    delay(10);
}