#include "input/backend.h"

namespace input {

void SyntheticBackend::enumerate(std::vector<DeviceDesc> *out)
{
    out->clear();
    out->reserve(devices.size());
    for (const Device &d : devices)
        out->push_back(d.desc);
}

bool SyntheticBackend::poll(int device, RawState *raw)
{
    if (device < 0 || (size_t)device >= devices.size() || !devices[device].available)
        return false;
    // The test sets the state for this frame, like a DirectInput poll.
    *raw = devices[device].raw;
    return true;
}

uint32_t SyntheticBackend::read_key()
{
    if (key_events.empty())
        return 0;
    uint32_t key = key_events.front();
    key_events.erase(key_events.begin());
    return key;
}

}  // namespace input
