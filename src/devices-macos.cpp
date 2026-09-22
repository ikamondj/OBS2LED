#include "output.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace obs2led {
namespace {
std::string string_property(io_registry_entry_t entry, CFStringRef key)
{
    auto value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
    if (!value) return {};
    std::array<char, 2048> buffer{};
    const bool valid = CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString(static_cast<CFStringRef>(value), buffer.data(), buffer.size(), kCFStringEncodingUTF8);
    CFRelease(value);
    return valid ? buffer.data() : "";
}
} // namespace

std::vector<SerialDevice> serial_devices()
{
    std::vector<SerialDevice> devices;
    auto matching = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!matching) throw std::runtime_error("Could not enumerate serial devices.");
    CFDictionarySetValue(matching, CFSTR(kIOSerialBSDTypeKey), CFSTR(kIOSerialBSDAllTypes));
    io_iterator_t iterator = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator) != KERN_SUCCESS)
        throw std::runtime_error("Could not enumerate serial devices.");
    while (auto service = IOIteratorNext(iterator)) {
        const auto path = string_property(service, CFSTR(kIOCalloutDeviceKey));
        io_registry_entry_t current = service;
        IOObjectRetain(current);
        bool usb = false;
        std::string label;
        for (int depth = 0; depth < 16; ++depth) {
            if (IOObjectConformsTo(current, "IOUSBHostDevice") || IOObjectConformsTo(current, "IOUSBDevice")) {
                usb = true;
                label = string_property(current, CFSTR("USB Product Name"));
                break;
            }
            io_registry_entry_t parent = 0;
            if (IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent) != KERN_SUCCESS) break;
            IOObjectRelease(current);
            current = parent;
        }
        IOObjectRelease(current);
        IOObjectRelease(service);
        if (usb && !path.empty()) devices.push_back({path, (label.empty() ? "USB serial" : label) + " (" + path + ")"});
    }
    IOObjectRelease(iterator);
    std::sort(devices.begin(), devices.end(), [](const auto &a, const auto &b) { return a.path < b.path; });
    return devices;
}
} // namespace obs2led
