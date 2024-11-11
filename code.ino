#include <driver/i2s.h>
#include <WiFi.h>
#include <ArduinoWebsockets.h>
#include <Adafruit_NeoPixel.h>


// Shared clock pins
#define I2S_BCK         (GPIO_NUM_5)
#define I2S_WS          (GPIO_NUM_6)

// Separate data pins
#define I2S_SPK_DATA    (GPIO_NUM_7)
#define I2S_MIC_DATA    (GPIO_NUM_17)


#define BUTTON_PIN 35
#define RGB_BUILTIN 48
#define NUM_PIXELS 1

#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE     44100  // Changed from 16000
#define bufferLen       1024  // Increased from 64
#define bufferCnt       10     // Changed from 8

// Global variables
int16_t sBuffer[bufferLen];
bool isRecording = false;
bool isPlaying = false;
bool isWebSocketConnected = false;

Adafruit_NeoPixel pixels(NUM_PIXELS, RGB_BUILTIN, NEO_GRB + NEO_KHZ800);

const char* ssid = "xxxx";
const char* password = "xxxx";
const char* websocket_server_host = "xxxx";
const uint16_t websocket_server_port = 8888;

using namespace websockets;
WebsocketsClient client;

void updateLED() {
    if (isRecording) {
        pixels.setPixelColor(0, pixels.Color(255, 0, 0));  // Red when recording
    } else if (isPlaying) {
        pixels.setPixelColor(0, pixels.Color(0, 255, 0));  // Green when playing
    } else {
        pixels.setPixelColor(0, pixels.Color(0, 0, 255));  // Blue when idle
    }
    pixels.show();
}


void i2s_install() {
    const i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate = 44100,  // Changed from 16000 to 44100
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 10,   // Changed from 8 to 10 to match ai3.ino
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    
    esp_err_t result = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (result != ESP_OK) {
        Serial.printf("Error installing I2S driver: %d\n", result);
        while(1);
    }
}

void i2s_setpin() {
    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK,         // GPIO 5
        .ws_io_num = I2S_WS,           // GPIO 6
        .data_out_num = I2S_SPK_DATA,  // GPIO 7 (Speaker output)
        .data_in_num = I2S_MIC_DATA    // GPIO 17 (Microphone input)
    };
    
    esp_err_t result = i2s_set_pin(I2S_PORT, &pin_config);
    if (result != ESP_OK) {
        Serial.printf("Error setting I2S pins: %d\n", result);
        while(1);
    }
}

void onEventsCallback(WebsocketsEvent event, String data) {
    if (event == WebsocketsEvent::ConnectionOpened) {
        Serial.println("Connection Opened");
        isWebSocketConnected = true;
    } else if (event == WebsocketsEvent::ConnectionClosed) {
        Serial.println("Connection Closed");
        isWebSocketConnected = false;
    }
}

void onAudioData(WebsocketsMessage message) {
    if (message.isBinary()) {
        Serial.println("Received audio data chunk");
        isPlaying = true;
        updateLED();
        
        const uint8_t* audio_data = (const uint8_t*)message.c_str();
        size_t data_length = message.length();
        
        Serial.printf("Audio chunk size: %d bytes\n", data_length);
        
        // Write data in smaller chunks
        size_t bytes_written = 0;
        size_t offset = 0;  // Changed from int to size_t
        while (offset < data_length) {
            size_t chunk_size = std::min(size_t(64), data_length - offset);  // Fixed min() call
            uint8_t* chunk = (uint8_t*)audio_data + offset;
            
            esp_err_t result = i2s_write(I2S_PORT, chunk, chunk_size, &bytes_written, portMAX_DELAY);
            if (result != ESP_OK) {
                Serial.printf("Error during I2S write: %d\n", result);
                break;
            }
            offset += bytes_written;
        }
        
        Serial.printf("Successfully wrote %d bytes to I2S\n", offset);
        isPlaying = false;
        updateLED();
    }
}

void setup() {
    Serial.begin(115200);
    
    // Initialize LED
    pixels.begin();
    pixels.setBrightness(50);
    
    // Initialize button
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    
    // Connect WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected");
    
    // Setup I2S
    i2s_install();
    i2s_setpin();
    i2s_start(I2S_PORT);
    
    // Setup WebSocket
    client.onEvent(onEventsCallback);
    client.onMessage(onAudioData);
    
    // Connect WebSocket
    while (!client.connect(websocket_server_host, websocket_server_port, "/")) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("Websocket Connected!");
    
    // Create microphone task
    xTaskCreatePinnedToCore(micTask, "micTask", 10000, NULL, 1, NULL, 1);
    
    updateLED();
}

void loop() {
    if (client.available()) {
        client.poll();
    } else if (!isWebSocketConnected) {
        Serial.println("Reconnecting to WebSocket...");
        client.connect(websocket_server_host, websocket_server_port, "/");
        delay(1000);
    }
    
    // Button handling
    if (digitalRead(BUTTON_PIN) == LOW) {  // Button pressed
        if (!isRecording) {
            isRecording = true;
            updateLED();
            delay(200);  // Debounce
        }
    } else {  // Button released
        if (isRecording) {
            isRecording = false;
            updateLED();
            delay(200);  // Debounce
        }
    }
}

void micTask(void* parameter) {
    size_t bytesIn = 0;
    int16_t samples[1024];
    
    while (1) {
        if (isRecording && isWebSocketConnected) {
            esp_err_t result = i2s_read(I2S_PORT, &samples, sizeof(samples), &bytesIn, portMAX_DELAY);
            if (result == ESP_OK) {
                // Simplified processing like in ai3.ino
                client.sendBinary((const char*)samples, bytesIn);
            } else {
                Serial.printf("Error reading from I2S: %d\n", result);
            }
        }
        vTaskDelay(1);
    }
}