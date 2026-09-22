#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

namespace {

constexpr uint16_t kPanelWidth = 64;
constexpr uint16_t kPanelHeight = 32;
constexpr uint8_t kBrightness = 64; // 25% brightness.
constexpr uint8_t kTextSize = 2;
constexpr uint32_t kScrollDelayMs = 30;
constexpr char kMessage[] = "LED CONTROL WORKING";
// The built-in GFX font advances by six pixels and has an eight-pixel cell.
constexpr int16_t kTextWidth = (sizeof(kMessage) - 1) * 6 * kTextSize;
constexpr int16_t kTextY = (kPanelHeight - 8 * kTextSize) / 2;

MatrixPanel_I2S_DMA *matrix = nullptr;
int16_t text_x = kPanelWidth;

} // namespace

void setup() {
    Serial.begin(115200);
    // Start even if no PC or serial monitor is connected.

    // Seengreat RGB Matrix HUB75 S3 pin mapping, same as the OBS receiver.
    HUB75_I2S_CFG::i2s_pins pins = {
        5, 4, 6,       // R1, G1, B1
        15, 7, 17,     // R2, G2, B2
        8, 18, 10, 9, 16, // A, B, C, D, E
        11, 13, 12     // LAT, OE, CLK
    };
    HUB75_I2S_CFG config(kPanelWidth, kPanelHeight, 1, pins);
    config.double_buff = true;
    matrix = new MatrixPanel_I2S_DMA(config);
    if (!matrix || !matrix->begin()) {
        for (;;) {
            Serial.println("Smoke test: matrix initialization failed.");
            delay(1000);
        }
    }

    matrix->setBrightness8(kBrightness);
    matrix->clearScreen();
    matrix->flipDMABuffer();
    matrix->clearScreen();
    matrix->setTextWrap(false);
    matrix->setTextSize(kTextSize);
    matrix->setTextColor(matrix->color565(0, 255, 0));
    Serial.println("Smoke test: scrolling LED CONTROL WORKING on a 64x32 panel.");
}

void loop() {
    matrix->clearScreen();
    matrix->setCursor(text_x, kTextY);
    matrix->print(kMessage);
    matrix->flipDMABuffer();

    if (--text_x < -kTextWidth) {
        text_x = kPanelWidth;
    }
    delay(kScrollDelayMs);
}
