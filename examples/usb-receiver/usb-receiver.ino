#include "rgb-receiver.hpp"

// Select the native USB CDC endpoint for your board; some cores call it SerialUSB.
#define LED_SERIAL Serial
static RgbReceiver<64 * 32 * 3> receiver;

void display_frame(uint32_t width, uint32_t height, uint32_t sequence, const uint8_t *rgb)
{
    // Connect this callback to your board's matrix driver. Pixel (x,y) starts at
    // rgb[(y * width + x) * 3]. This buffer is reused: copy/swap before async DMA.
    // Matrix pin mapping, multiplexing and physical panel layout are board-specific.
    (void)width; (void)height; (void)sequence; (void)rgb;
}

void setup()
{
    LED_SERIAL.begin(2000000);
    // Initialize the matrix driver here. Do not wait forever for a serial monitor.
}

void loop()
{
    static uint32_t last_byte = 0;
    if (uint32_t(millis() - last_byte) > 2500) receiver.reset();
    while (LED_SERIAL.available()) {
        receiver.feed(static_cast<uint8_t>(LED_SERIAL.read()), display_frame);
        last_byte = millis();
    }
    // Keep logging off LED_SERIAL: it is the binary video channel.
}
