#pragma once
#include <string>

// Opens a Linux TAP interface (IFF_TAP | IFF_NO_PI) and returns its fd.
//
// The fd is symmetric:
//   write(fd, frame, len) → injects an Ethernet frame into the kernel
//   read(fd, buf, len)   → receives a frame the kernel wants to send out
//
// Used as a drop-in replacement for the VHCI socketpair fd in DeviceSession:
// PassthroughHandler reads from it (kernel→device) and writes to it (device→kernel),
// which is exactly the same direction convention as the VHCI path.

class TapDevice {
public:
    TapDevice()  = default;
    ~TapDevice() { close(); }

    TapDevice(const TapDevice&)            = delete;
    TapDevice& operator=(const TapDevice&) = delete;

    // Opens /dev/net/tun as a TAP device with the given interface name.
    // Returns true on success; fd() is then valid.
    bool open(const std::string& name);

    // Closes the fd if open.
    void close();

    // Releases ownership of the fd (like unique_ptr::release).
    // The caller is responsible for closing it afterwards.
    // After this call fd() returns -1.
    int release();

    int  fd()     const { return fd_; }
    bool isOpen() const { return fd_ >= 0; }

private:
    int fd_ = -1;
};
