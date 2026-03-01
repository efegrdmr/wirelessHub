#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Manages the vhci-hcd kernel module:
//   - loads the module on construction
//   - attaches / detaches virtual USB ports
//   - reads USBIP_CMD_* requests from /dev/vhci
//   - writes USBIP_RET_* responses back to /dev/vhci

class VhciDriver {
public:
    VhciDriver();
    ~VhciDriver();

    // Load vhci-hcd if not already loaded; returns false on failure
    bool init();

    // Attach a virtual USB port; returns the assigned port number, -1 on failure
    // socket_fd: the TCP socket fd that carries URB traffic for this device
    // speed: USB speed (1=low, 2=full, 3=high)
    // devid: (busnum << 16) | devnum
    int attach(int socket_fd, uint32_t devid, uint32_t speed);

    // Detach the virtual port with the given port number
    bool detach(int port);

    // Mark port as free in daemon's tracking table without writing to sysfs.
    // Use this when the kernel has already released the port (USB reset) and
    // you want to re-attach without a sysfs detach write.
    void freePort(int port);

    // Returns the epoll-ready fd for /dev/vhci (use with epoll to detect incoming requests)
    int getFd() const;

    // Read one raw USB/IP message from /dev/vhci into buf; returns bytes read, -1 on error
    ssize_t readRequest(uint8_t* buf, size_t buf_len);

    // Write a raw USB/IP response to /dev/vhci; returns bytes written, -1 on error
    ssize_t writeResponse(const uint8_t* buf, size_t buf_len);

    // Convenience overload for std::vector
    ssize_t writeResponse(const std::vector<uint8_t>& buf);

    // Detach all currently attached ports (called on shutdown)
    void detachAll();

private:
    int  vhci_fd_   = -1;   // fd for /dev/vhci
    int  max_ports_ = 8;    // number of available vhci ports
    bool ports_[8]  = {};   // true = port in use

    // Write to a sysfs file; returns false on failure
    static bool sysfsWrite(const std::string& path, const std::string& value);

    // sysfs path for attach/detach
    static constexpr const char* SYSFS_ATTACH = "/sys/devices/platform/vhci_hcd.0/attach";
    static constexpr const char* SYSFS_DETACH = "/sys/devices/platform/vhci_hcd.0/detach";
    static constexpr const char* VHCI_DEV     = "/dev/vhci";
};
