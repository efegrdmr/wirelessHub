#include "VhciDriver.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

VhciDriver::VhciDriver() = default;

VhciDriver::~VhciDriver() {
    detachAll();
    if (vhci_fd_ >= 0)
        close(vhci_fd_);
}

bool VhciDriver::init() {
    // load vhci-hcd kernel module if not already present
    if (system("modprobe vhci-hcd") != 0) {
        printf("[VhciDriver] warning: modprobe vhci-hcd failed (may already be loaded)\n");
    }

    vhci_fd_ = open(VHCI_DEV, O_RDWR | O_NONBLOCK);
    if (vhci_fd_ < 0) {
        perror("[VhciDriver] failed to open /dev/vhci");
        return false;
    }

    printf("[VhciDriver] opened %s (fd=%d)\n", VHCI_DEV, vhci_fd_);
    return true;
}

int VhciDriver::attach(int socket_fd, uint32_t devid, uint32_t speed) {
    // find a free port slot
    int port = -1;
    for (int i = 0; i < max_ports_; ++i) {
        if (!ports_[i]) { port = i; break; }
    }
    if (port < 0) {
        printf("[VhciDriver] attach: no free ports available\n");
        return -1;
    }

    // sysfs format: "<port> <socket_fd> <devid> <speed>"
    char buf[64];
    snprintf(buf, sizeof(buf), "%d %d %u %u", port, socket_fd, devid, speed);

    if (!sysfsWrite(SYSFS_ATTACH, buf)) {
        printf("[VhciDriver] attach: sysfs write failed for port %d\n", port);
        return -1;
    }

    ports_[port] = true;
    printf("[VhciDriver] attached virtual port %d (devid=0x%08X speed=%u)\n", port, devid, speed);
    return port;
}

bool VhciDriver::detach(int port) {
    if (port < 0 || port >= max_ports_ || !ports_[port]) {
        printf("[VhciDriver] detach: port %d not in use\n", port);
        return false;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", port);

    if (!sysfsWrite(SYSFS_DETACH, buf)) {
        printf("[VhciDriver] detach: sysfs write failed for port %d\n", port);
        return false;
    }

    ports_[port] = false;
    printf("[VhciDriver] detached virtual port %d\n", port);
    return true;
}

void VhciDriver::freePort(int port) {
    if (port >= 0 && port < max_ports_)
        ports_[port] = false;
}

int VhciDriver::getFd() const {
    return vhci_fd_;
}

ssize_t VhciDriver::readRequest(uint8_t* buf, size_t buf_len) {
    ssize_t n = read(vhci_fd_, buf, buf_len);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        perror("[VhciDriver] read error");
    return n;
}

ssize_t VhciDriver::writeResponse(const uint8_t* buf, size_t buf_len) {
    ssize_t n = write(vhci_fd_, buf, buf_len);
    if (n < 0)
        perror("[VhciDriver] write error");
    return n;
}

ssize_t VhciDriver::writeResponse(const std::vector<uint8_t>& buf) {
    return writeResponse(buf.data(), buf.size());
}

void VhciDriver::detachAll() {
    for (int i = 0; i < max_ports_; ++i) {
        if (ports_[i])
            detach(i);
    }
}

bool VhciDriver::sysfsWrite(const std::string& path, const std::string& value) {
    int fd = open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[VhciDriver] cannot open sysfs path %s: %s\n",
                path.c_str(), strerror(errno));
        return false;
    }
    ssize_t n = write(fd, value.c_str(), value.size());
    close(fd);
    return n == static_cast<ssize_t>(value.size());
}
