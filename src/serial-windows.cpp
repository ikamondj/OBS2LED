#include "output.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace obs2led {
namespace {
std::string utf8(const wchar_t *value)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

bool usb_parent(DEVINST device)
{
    for (int depth = 0; depth < 16; ++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Device_IDW(device, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS &&
            wcsncmp(id, L"USB\\", 4) == 0) return true;
        DEVINST parent;
        if (CM_Get_Parent(&parent, device, 0) != CR_SUCCESS) break;
        device = parent;
    }
    return false;
}

[[noreturn]] void fail(const char *operation)
{
    throw std::runtime_error(std::string(operation) + " failed (Windows error " + std::to_string(GetLastError()) + ").");
}

class Serial final : public Connection {
public:
    explicit Serial(const OutputConfig &config)
    {
        // Only accept COM names, never arbitrary filesystem/device paths from saved settings.
        if (!config.device.starts_with("COM") || config.device.size() < 4 ||
            !std::all_of(config.device.begin() + 3, config.device.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw std::runtime_error("Invalid COM port name.");
        const auto path = std::string("\\\\.\\") + config.device;
        handle_ = CreateFileA(path.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) fail("Open USB serial device");
        try {
            DCB state{};
            state.DCBlength = sizeof(state);
            if (!GetCommState(handle_, &state)) fail("Read serial settings");
            state.BaudRate = static_cast<DWORD>(config.baud);
            state.ByteSize = 8; state.Parity = NOPARITY; state.StopBits = ONESTOPBIT;
            state.fBinary = TRUE; state.fParity = FALSE;
            state.fOutxCtsFlow = FALSE; state.fOutxDsrFlow = FALSE;
            state.fDtrControl = DTR_CONTROL_ENABLE; state.fRtsControl = RTS_CONTROL_DISABLE;
            state.fDsrSensitivity = FALSE; state.fOutX = FALSE; state.fInX = FALSE;
            state.fErrorChar = FALSE; state.fNull = FALSE; state.fAbortOnError = FALSE;
            if (!SetCommState(handle_, &state)) fail("Set serial baud rate / 8N1");
            COMMTIMEOUTS timeouts{};
            timeouts.WriteTotalTimeoutConstant = 2000;
            if (!SetCommTimeouts(handle_, &timeouts)) fail("Set serial timeout");
            PurgeComm(handle_, PURGE_TXCLEAR | PURGE_RXCLEAR);
            event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!event_) fail("Create serial event");
        } catch (...) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; throw; }
    }
    ~Serial() override
    {
        if (event_) CloseHandle(event_);
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    void write(std::span<const uint8_t> bytes, const Cancelled &cancelled) override
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        size_t offset = 0;
        while (offset < bytes.size()) {
            if (cancelled()) throw std::runtime_error("USB transfer cancelled.");
            OVERLAPPED overlapped{};
            overlapped.hEvent = event_;
            ResetEvent(event_);
            DWORD written = 0;
            const DWORD count = static_cast<DWORD>(std::min<size_t>(4096, bytes.size() - offset));
            if (!WriteFile(handle_, bytes.data() + offset, count, &written, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) fail("USB write");
                bool abort = false;
                for (;;) {
                    const DWORD wait = WaitForSingleObject(event_, 10);
                    if (wait == WAIT_OBJECT_0) break;
                    if (wait == WAIT_FAILED || cancelled() || std::chrono::steady_clock::now() >= deadline) {
                        abort = true;
                        CancelIoEx(handle_, &overlapped);
                        break;
                    }
                }
                // Drain completion before OVERLAPPED or its frame buffer leaves scope.
                const BOOL completed = GetOverlappedResult(handle_, &overlapped, &written, TRUE);
                if (abort) {
                    PurgeComm(handle_, PURGE_TXABORT | PURGE_TXCLEAR);
                    throw std::runtime_error("USB transfer cancelled or timed out.");
                }
                if (!completed) fail("USB write completion");
            }
            if (!written) throw std::runtime_error("USB device stopped accepting data.");
            offset += written;
        }
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
};
} // namespace

std::vector<SerialDevice> serial_devices()
{
    std::vector<SerialDevice> result;
    const HDEVINFO devices = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) fail("Enumerate serial devices");
    try {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &device); ++index) {
            if (!usb_parent(device.DevInst)) continue;
            const HKEY key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
            if (key == INVALID_HANDLE_VALUE) continue;
            wchar_t port[256]{};
            DWORD size = sizeof(port);
            DWORD type = 0;
            const auto error = RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<BYTE *>(port), &size);
            RegCloseKey(key);
            if (error != ERROR_SUCCESS || type != REG_SZ || wcsncmp(port, L"COM", 3) != 0) continue;
            wchar_t label[512]{};
            SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_FRIENDLYNAME, nullptr,
                                              reinterpret_cast<BYTE *>(label), sizeof(label), nullptr);
            auto path = utf8(port);
            result.push_back({path, label[0] ? utf8(label) : path});
        }
    } catch (...) { SetupDiDestroyDeviceInfoList(devices); throw; }
    SetupDiDestroyDeviceInfoList(devices);
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    return result;
}

std::unique_ptr<Connection> open_serial(const OutputConfig &config) { return std::make_unique<Serial>(config); }
} // namespace obs2led
