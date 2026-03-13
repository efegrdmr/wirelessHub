#include "VhciDriver.h"
#include "../Log.h"
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
        LOG_WARN("[VhciDriver] modprobe vhci-hcd failed (module may already be loaded)");
    }

    vhci_fd_ = open(VHCI_DEV, O_RDWR | O_NONBLOCK);
    if (vhci_fd_ < 0) {
        LOG_ERR("[VhciDriver] failed to open %s: %s", VHCI_DEV, strerror(errno));
        return false;
    }

    LOG_INFO("[VhciDriver] opened %s  fd=%d  max_ports=%d", VHCI_DEV, vhci_fd_, max_ports_);
    return true;
}

int VhciDriver::attach(int socket_fd, uint32_t devid, uint32_t speed) {
    // find a free port slot
    int port = -1;
    for (int i = 0; i < max_ports_; ++i) {
        if (!ports_[i]) { port = i; break; }
    }
    if (port < 0) {
        LOG_ERR("[VhciDriver] attach: no free ports (all %d in use)", max_ports_);
        return -1;
    }

    // sysfs format: "<port> <socket_fd> <devid> <speed>"
    char buf[64];
    snprintf(buf, sizeof(buf), "%d %d %u %u", port, socket_fd, devid, speed);

    if (!sysfsWrite(SYSFS_ATTACH, buf)) {
        LOG_ERR("[VhciDriver] attach: sysfs write failed for port %d", port);
        return -1;
    }

    ports_[port] = true;
    LOG_INFO("[VhciDriver] attached  port=%d  devid=0x%08X  speed=%u  socket_fd=%d",
             port, devid, speed, socket_fd);
    return port;
}

bool VhciDriver::detach(int port) {
    if (port < 0 || port >= max_ports_ || !ports_[port]) {
        LOG_WARN("[VhciDriver] detach: port %d not in use", port);
        return false;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", port);

    if (!sysfsWrite(SYSFS_DETACH, buf)) {
        LOG_ERR("[VhciDriver] detach: sysfs write failed for port %d", port);
        return false;
    }

    ports_[port] = false;
    LOG_INFO("[VhciDriver] detached  port=%d", port);
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
        LOG_ERR("[VhciDriver] readRequest: %s", strerror(errno));
    return n;
}

ssize_t VhciDriver::writeResponse(const uint8_t* buf, size_t buf_len) {
    ssize_t n = write(vhci_fd_, buf, buf_len);
    if (n < 0)
        LOG_ERR("[VhciDriver] writeResponse: %s", strerror(errno));
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
        LOG_ERR("[VhciDriver] cannot open sysfs %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    ssize_t n = write(fd, value.c_str(), value.size());
    close(fd);
    return n == static_cast<ssize_t>(value.size());
}
