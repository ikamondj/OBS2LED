#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || ARDUINO_USB_CDC_ON_BOOT != 1
#error "Build with ARDUINO_USB_CDC_ON_BOOT=1 so Serial is the ESP32-S3 native USB CDC port."
#endif

#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 1
#error "Build with ARDUINO_USB_MODE=1 so Serial uses the ESP32-S3 hardware USB CDC/JTAG port."
#endif

namespace {

constexpr uint16_t kPanelWidth = 64;
constexpr uint16_t kPanelHeight = 32;
constexpr size_t kRgbBytes =
    static_cast<size_t>(kPanelWidth) * kPanelHeight * 3;

constexpr size_t kHeaderBytes = 24;
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kBrightness = 128;
constexpr uint32_t kPartialFrameTimeoutMs = 250;

constexpr uint8_t kMagic[4] = {'O', '2', 'L', 'F'};

// Seengreat RGB Matrix HUB75 S3 V1.0 pin mapping.
constexpr int kR1 = 5;
constexpr int kG1 = 4;
constexpr int kB1 = 6;
constexpr int kR2 = 15;
constexpr int kG2 = 7;
constexpr int kB2 = 17;
constexpr int kA = 8;
constexpr int kB = 18;
constexpr int kC = 10;
constexpr int kD = 9;
constexpr int kE = 16;
constexpr int kLat = 11;
constexpr int kOe = 13;
constexpr int kClk = 12;

struct FrameMessage {
    uint32_t sequence = 0;
    uint8_t rgb[kRgbBytes]{};
};

MatrixPanel_I2S_DMA *g_matrix = nullptr;
QueueHandle_t g_frame_queue = nullptr;
FrameMessage g_rx_frame{};
FrameMessage g_render_frame{};

uint16_t read_le16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^
                  (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void render_frame(const FrameMessage &frame) {
    const uint8_t *p = frame.rgb;

    for (int y = 0; y < kPanelHeight; ++y) {
        for (int x = 0; x < kPanelWidth; ++x) {
            const uint8_t r = *p++;
            const uint8_t g = *p++;
            const uint8_t b = *p++;
            g_matrix->drawPixelRGB888(x, y, r, g, b);
        }
    }

    // The library was configured for two DMA framebuffers. All drawing above
    // went into the non-visible buffer; this makes the completed frame visible
    // and changes subsequent drawing to the other buffer.
    g_matrix->flipDMABuffer();
}

void render_task(void *) {
    for (;;) {
        if (xQueueReceive(g_frame_queue, &g_render_frame, portMAX_DELAY) == pdTRUE) {
            render_frame(g_render_frame);
        }
    }
}

class O2LFReceiver {
public:
    void consume(const uint8_t *data, size_t size) {
        if (state_ != State::magic &&
            static_cast<uint32_t>(millis() - last_byte_ms_) >
                kPartialFrameTimeoutMs) {
            reset();
        }

        if (size != 0) {
            last_byte_ms_ = millis();
        }

        for (size_t i = 0; i < size; ++i) {
            consume_byte(data[i]);
        }
    }

private:
    enum class State {
        magic,
        header,
        payload,
    };

    void reset() {
        state_ = State::magic;
        magic_used_ = 0;
        header_used_ = 0;
        payload_used_ = 0;
        expected_crc_ = 0;
        sequence_ = 0;
    }

    void consume_magic(uint8_t byte) {
        if (byte == kMagic[magic_used_]) {
            ++magic_used_;
        } else {
            magic_used_ = (byte == kMagic[0]) ? 1 : 0;
        }

        if (magic_used_ == sizeof(kMagic)) {
            std::memcpy(header_, kMagic, sizeof(kMagic));
            header_used_ = sizeof(kMagic);
            magic_used_ = 0;
            state_ = State::header;
        }
    }

    bool validate_header() {
        if (header_[4] != kProtocolVersion) return false;
        if (header_[5] != 0) return false;
        if (read_le16(header_ + 6) != kHeaderBytes) return false;
        if (read_le16(header_ + 8) != kPanelWidth) return false;
        if (read_le16(header_ + 10) != kPanelHeight) return false;
        if (read_le32(header_ + 16) != kRgbBytes) return false;

        sequence_ = read_le32(header_ + 12);
        expected_crc_ = read_le32(header_ + 20);
        return true;
    }

    void consume_byte(uint8_t byte) {
        switch (state_) {
        case State::magic:
            consume_magic(byte);
            break;

        case State::header:
            header_[header_used_++] = byte;
            if (header_used_ == kHeaderBytes) {
                if (!validate_header()) {
                    reset();
                    return;
                }
                payload_used_ = 0;
                state_ = State::payload;
            }
            break;

        case State::payload:
            g_rx_frame.rgb[payload_used_++] = byte;
            if (payload_used_ == kRgbBytes) {
                if (crc32(g_rx_frame.rgb, kRgbBytes) == expected_crc_) {
                    g_rx_frame.sequence = sequence_;
                    // Queue length is one, so if rendering ever falls behind,
                    // discard the queued stale frame and retain the newest one.
                    xQueueOverwrite(g_frame_queue, &g_rx_frame);
                }
                reset();
            }
            break;
        }
    }

    State state_ = State::magic;
    size_t magic_used_ = 0;
    size_t header_used_ = 0;
    size_t payload_used_ = 0;
    uint32_t expected_crc_ = 0;
    uint32_t sequence_ = 0;
    uint32_t last_byte_ms_ = 0;
    uint8_t header_[kHeaderBytes]{};
};

O2LFReceiver g_receiver;

bool start_matrix() {
    HUB75_I2S_CFG::i2s_pins pins = {
        kR1, kG1, kB1,
        kR2, kG2, kB2,
        kA, kB, kC, kD, kE,
        kLat, kOe, kClk
    };

    HUB75_I2S_CFG config(kPanelWidth, kPanelHeight, 1, pins);
    config.double_buff = true;

    g_matrix = new MatrixPanel_I2S_DMA(config);
    if (!g_matrix || !g_matrix->begin()) {
        return false;
    }

    g_matrix->setBrightness8(kBrightness);
    g_matrix->clearScreen();
    g_matrix->flipDMABuffer();
    g_matrix->clearScreen();
    return true;
}

} // namespace

void setup() {
    // The host program opens this as a Windows COM port. Because this is native
    // USB CDC rather than a physical UART, the nominal baud rate selected by
    // Windows does not limit the actual USB transfer rate.
    Serial.setRxBufferSize(32768);
    Serial.begin();

    if (!start_matrix()) {
        // Keep USB alive so the board can still be reflashed, but do not attempt
        // to process video without a valid HUB75 DMA configuration.
        for (;;) {
            delay(1000);
        }
    }

    g_frame_queue = xQueueCreate(1, sizeof(FrameMessage));
    if (!g_frame_queue) {
        for (;;) {
            delay(1000);
        }
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        render_task,
        "hub75-render",
        8192,
        nullptr,
        2,
        nullptr,
        0
    );

    if (created != pdPASS) {
        for (;;) {
            delay(1000);
        }
    }
}

void loop() {
    uint8_t usb_buffer[1024];

    const int available = Serial.available();
    if (available > 0) {
        const size_t wanted =
            std::min<size_t>(sizeof(usb_buffer), static_cast<size_t>(available));
        const size_t count = Serial.read(usb_buffer, wanted);
        if (count != 0) {
            g_receiver.consume(usb_buffer, count);
        }
    } else {
        delay(1);
    }
}
