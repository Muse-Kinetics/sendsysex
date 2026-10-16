#ifndef USB_TOPOLOGY_H
#define USB_TOPOLOGY_H

#include <string>
#include <vector>

// Whether a KMI device is on a root port or behind external hubs. Windows only.
namespace usbTopology
{
struct Connection
{
    std::string product;            // USB product string, e.g. "SSCOM"
    std::string instanceId;         // e.g. USB\VID_1F38&PID_000B\5&...
    std::vector<std::string> hubs;  // external hubs, nearest first, e.g. "USB2.0 Hub (1A40:0101)"
    bool virtualController = false; // root hub belongs to a VM's emulated USB controller
};

// Present USB devices whose product string matches one of productStrings
// (case-insensitive, spaces ignored), e.g. a family's discovery markers.
std::vector<Connection> find(const std::vector<std::string> &productStrings);

// One line per connection for the console/session log.
std::string describe(const Connection &c);
}

#endif
