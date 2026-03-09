#include "TapDevice.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/if_tun.h>

bool TapDevice::open(const std::string& name)
{
    int fd = ::open("/dev/net/tun", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("[TapDevice] open /dev/net/tun");
        return false;
    }

    struct ifreq ifr{};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("[TapDevice] TUNSETIFF");
        ::close(fd);
        return false;
    }

    fd_ = fd;

    // Bring the interface UP automatically via a temporary control socket.
    int ctl = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (ctl >= 0) {
        struct ifreq up_ifr{};
        memcpy(up_ifr.ifr_name, ifr.ifr_name, IFNAMSIZ);
        if (ioctl(ctl, SIOCGIFFLAGS, &up_ifr) == 0) {
            up_ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
            if (ioctl(ctl, SIOCSIFFLAGS, &up_ifr) < 0)
                perror("[TapDevice] SIOCSIFFLAGS up");
        }
        ::close(ctl);
    }

    printf("[TapDevice] opened %s (fd=%d) — interface is UP\n",
           ifr.ifr_name, fd_);
    return true;
}

void TapDevice::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int TapDevice::release()
{
    int fd = fd_;
    fd_ = -1;
    return fd;
}
