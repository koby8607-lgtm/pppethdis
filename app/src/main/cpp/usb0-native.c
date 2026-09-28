#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fib_rules.h>
#include <linux/if_addr.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>
#include <linux/usbdevice_fs.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef RT_TABLE_UNSPEC
#define RT_TABLE_UNSPEC 0
#endif
#ifndef RT_TABLE_MAIN
#define RT_TABLE_MAIN 254
#endif
#ifndef RT_TABLE_LOCAL
#define RT_TABLE_LOCAL 255
#endif
#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif
#ifndef ARPHRD_ETHER
#define ARPHRD_ETHER 1
#endif

#define DEFAULT_IFNAME "usb0"
#ifndef IFNAMSIZ
#define IFNAMSIZ IF_NAMESIZE
#endif
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC 0x63825363U
#define DHCP_MAX_OPTIONS 312
#define NETLINK_BUF 32768
#define POLICY_RULE_PRIORITY 12050U
#define AUTO_MTU 1500

typedef struct {
    int found;
    uint32_t table;
    int oif;
    uint32_t gateway;
    uint32_t priority;
    uint8_t scope;
    uint8_t type;
} route_info;

typedef struct {
    int found;
    uint32_t table;
    uint32_t priority;
    uint32_t src;
    uint8_t src_len;
    uint32_t mark;
    uint32_t mask;
    char oifname[IFNAMSIZ];
    char iifname[IFNAMSIZ];
} rule_info;

typedef struct {
    uint32_t yiaddr;
    uint32_t server_id;
    uint32_t subnet_mask;
    uint32_t router;
    uint32_t dns1;
    uint32_t dns2;
    uint32_t lease_time;
} dhcp_result;

static char g_ifname[IFNAMSIZ] = DEFAULT_IFNAME;

static const char *iface(void) { return g_ifname; }
static int ifindex(void) { return (int)if_nametoindex(iface()); }

static int addattr(struct nlmsghdr *nlh, size_t maxlen, int type, const void *data, size_t alen);
static int rtnl_exchange(struct nlmsghdr *nlh);
static int route_dump(route_info *selected);
static void print_ip(uint32_t v);

static uint16_t csum16(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += ((uint16_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (uint16_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return htons((uint16_t)~sum);
}

static int bind_to_iface(int fd) {
    return setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface(), (socklen_t)(strlen(iface()) + 1));
}

static int read_file(const char *path, char *out, size_t out_sz) {
    if (!out || out_sz < 2) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, out_sz - 1);
    close(fd);
    if (n < 0) return -1;
    out[n] = 0;
    while (n > 0 && isspace((unsigned char)out[n - 1])) out[--n] = 0;
    return 0;
}

static int usb_reset_device(unsigned bus, unsigned devnum) {
    char path[128];
    snprintf(path, sizeof(path), "/dev/bus/usb/%03u/%03u", bus, devnum);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open usbfs"); return -1; }
    int rc = ioctl(fd, USBDEVFS_RESET, 0);
    if (rc < 0) perror("USBDEVFS_RESET");
    close(fd);
    return rc;
}

static int write_file(const char *path, const char *value) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t len = strlen(value);
    ssize_t n = write(fd, value, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}


static int get_mac(uint8_t mac[6]) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface());
    int rc = ioctl(fd, SIOCGIFHWADDR, &ifr);
    if (rc == 0) memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    close(fd);
    return rc;
}

static int set_link_state(int up) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface());
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) { close(fd); return -1; }
    if (up) ifr.ifr_flags |= IFF_UP; else ifr.ifr_flags &= (short)~IFF_UP;
    int rc = ioctl(fd, SIOCSIFFLAGS, &ifr);
    close(fd);
    return rc;
}

static int get_link_info(int *up, int *carrier, int *mtu) {
    char path[256], buf[64];
    if (up) *up = 0;
    if (carrier) *carrier = -1;
    if (mtu) *mtu = 0;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
        struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface());
        if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0 && up) *up = (ifr.ifr_flags & IFF_UP) ? 1 : 0;
        close(fd);
    }
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", iface());
    if (read_file(path, buf, sizeof(buf)) == 0 && carrier) *carrier = atoi(buf);
    if (mtu) {
        snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", iface());
        if (read_file(path, buf, sizeof(buf)) == 0) *mtu = atoi(buf);
    }
    return 0;
}

static int current_ipv4(uint32_t *addr, uint32_t *mask) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface());
    int rc = -1;
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        if (addr) *addr = sin->sin_addr.s_addr;
        rc = 0;
    }
    if (rc == 0 && mask) {
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface());
        if (ioctl(fd, SIOCGIFNETMASK, &ifr) == 0) *mask = ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr.s_addr;
        else *mask = 0;
    }
    close(fd);
    return rc;
}

static unsigned mask_to_prefix(uint32_t mask) {
    uint32_t m = ntohl(mask); unsigned p = 0;
    while (m & 0x80000000U) { ++p; m <<= 1; }
    return p;
}

static int parse_mac(const char *text, uint8_t mac[6]) {
    unsigned v[6];
    if (!text || !mac || sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)v[i];
    return 0;
}

static int mac_invalid(const uint8_t mac[6]) {
    int all0 = 1, allf = 1;
    for (int i = 0; i < 6; ++i) {
        if (mac[i]) all0 = 0;
        if (mac[i] != 0xff) allf = 0;
    }
    return all0 || allf || (mac[0] & 1);
}

static void print_mac(const uint8_t mac[6]) {
    if (!mac) { printf("-"); return; }
    printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int set_mac_addr(const uint8_t mac[6]) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface());
    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(ifr.ifr_hwaddr.sa_data, mac, 6);
    int rc = ioctl(fd, SIOCSIFHWADDR, &ifr);
    close(fd);
    return rc;
}

static int add_neighbor(uint32_t dst, const uint8_t mac[6], uint16_t state) {
    struct { struct nlmsghdr nlh; struct ndmsg ndm; char buf[256]; } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ndm));
    req.nlh.nlmsg_type = RTM_NEWNEIGH;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    req.ndm.ndm_family = AF_INET;
    req.ndm.ndm_ifindex = ifindex();
    req.ndm.ndm_state = state;
    req.ndm.ndm_flags = NTF_USE;
    if (addattr(&req.nlh, sizeof(req), NDA_DST, &dst, 4) < 0) return -1;
    if (addattr(&req.nlh, sizeof(req), NDA_LLADDR, mac, 6) < 0) return -1;
    return rtnl_exchange(&req.nlh);
}

static int arp_resolve(uint32_t target, uint8_t out_mac[6], int timeout_ms) {
    int idx = ifindex();
    uint8_t src_mac[6];
    uint32_t src_ip = 0, mask = 0;
    if (!idx || !out_mac || get_mac(src_mac) < 0 || mac_invalid(src_mac)) return -1;
    (void)current_ipv4(&src_ip, &mask);
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ARP));
    if (fd < 0) return -1;
    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(ETH_P_ARP);
    sa.sll_ifindex = idx;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }

    uint8_t frame[42] = {0};
    memset(frame, 0xff, 6);
    memcpy(frame + 6, src_mac, 6);
    frame[12] = 0x08; frame[13] = 0x06;
    frame[14] = 0; frame[15] = 1;
    frame[16] = 0x08; frame[17] = 0;
    frame[18] = 6; frame[19] = 4;
    frame[20] = 0; frame[21] = 1;
    memcpy(frame + 22, src_mac, 6);
    memcpy(frame + 28, &src_ip, 4);
    memcpy(frame + 38, &target, 4);

    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET; sa.sll_ifindex = idx; sa.sll_halen = 6;
    memset(sa.sll_addr, 0xff, 6);
    if (sendto(fd, frame, sizeof(frame), 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }

    uint64_t deadline = (uint64_t)time(NULL) * 1000ULL + (uint64_t)timeout_ms;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    for (;;) {
        uint64_t now = (uint64_t)time(NULL) * 1000ULL;
        if (now >= deadline) break;
        int left = (int)(deadline - now); if (left > 1000) left = 1000;
        if (poll(&pfd, 1, left) <= 0) continue;
        uint8_t rx[2048]; ssize_t n = recv(fd, rx, sizeof(rx), 0);
        if (n < 42 || rx[12] != 0x08 || rx[13] != 0x06 || rx[20] != 0 || rx[21] != 2) continue;
        if (memcmp(rx + 28, &target, 4) != 0 || memcmp(rx + 38, &src_ip, 4) != 0) continue;
        memcpy(out_mac, rx + 22, 6);
        close(fd);
        return mac_invalid(out_mac) ? -1 : 0;
    }
    close(fd);
    return -1;
}

static int arp_set_cmd(const char *ip_text, const char *mac_text) {
    struct in_addr a;
    uint8_t mac[6];
    if (!ip_text || !mac_text || inet_aton(ip_text, &a) == 0 || parse_mac(mac_text, mac) < 0 || mac_invalid(mac)) {
        puts("ARP_RESULT=BAD_INPUT");
        return 2;
    }
    if (!ifindex()) {
        puts("ARP_RESULT=NO_INTERFACE");
        return 1;
    }
    if (add_neighbor(a.s_addr, mac, NUD_PERMANENT) < 0) {
        puts("ARP_RESULT=NEIGHBOR_ADD_FAIL");
        return 1;
    }
    printf("ARP_TARGET=%s\n", ip_text);
    printf("ARP_MAC="); print_mac(mac); puts("");
    puts("ARP_RESULT=OK");
    return 0;
}

static int arp_gateway(void) {
    route_info ri; memset(&ri, 0, sizeof(ri));
    if (route_dump(&ri) < 0 || !ri.gateway) { puts("ARP_RESULT=NO_GATEWAY"); return 1; }
    uint8_t mac[6];
    printf("ARP_TARGET="); print_ip(ri.gateway); puts("");
    if (arp_resolve(ri.gateway, mac, 1800) < 0) { puts("ARP_RESULT=FAIL"); return 1; }
    if (add_neighbor(ri.gateway, mac, NUD_REACHABLE) < 0) { puts("ARP_RESULT=NEIGHBOR_ADD_FAIL"); return 1; }
    printf("ARP_MAC="); print_mac(mac); puts("");
    puts("ARP_RESULT=OK");
    return 0;
}

static uint32_t network_of(uint32_t addr, uint32_t mask) { return addr & mask; }

static int addattr(struct nlmsghdr *nlh, size_t maxlen, int type, const void *data, size_t alen) {
    size_t len = RTA_LENGTH(alen);
    size_t pos = NLMSG_ALIGN(nlh->nlmsg_len);
    size_t newlen = pos + RTA_ALIGN(len);
    if (newlen > maxlen) return -1;
    struct rtattr *rta = (struct rtattr *)((char *)nlh + pos);
    rta->rta_type = type; rta->rta_len = (unsigned short)len;
    if (alen) memcpy(RTA_DATA(rta), data, alen);
    nlh->nlmsg_len = (unsigned)newlen;
    return 0;
}

static int rtnl_exchange(struct nlmsghdr *nlh) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sockaddr_nl local; memset(&local, 0, sizeof(local)); local.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) { close(fd); return -1; }
    static uint32_t seq = 0x61000000U; nlh->nlmsg_seq = ++seq;
    struct sockaddr_nl dst; memset(&dst, 0, sizeof(dst)); dst.nl_family = AF_NETLINK;
    if (sendto(fd, nlh, nlh->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) { close(fd); return -1; }
    char buf[NETLINK_BUF];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h); int err = e->error; close(fd);
                if (err == 0) return 0;
                errno = -err;
                return -1;
            }
            if (h->nlmsg_type == NLMSG_DONE || !(h->nlmsg_flags & NLM_F_MULTI)) { close(fd); return 0; }
        }
    }
}

static int route_dump(route_info *selected) {
    memset(selected, 0, sizeof(*selected));
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    char req[NLMSG_SPACE(sizeof(struct rtmsg))]; memset(req, 0, sizeof(req));
    struct nlmsghdr *nlh = (struct nlmsghdr *)req; struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*rtm)); nlh->nlmsg_type = RTM_GETROUTE; nlh->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST; rtm->rtm_family = AF_INET;
    static uint32_t seq = 0x62000000U; nlh->nlmsg_seq = ++seq;
    struct sockaddr_nl dst; memset(&dst, 0, sizeof(dst)); dst.nl_family = AF_NETLINK;
    if (sendto(fd, req, nlh->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) { close(fd); return -1; }
    int idx = ifindex(); char buf[NETLINK_BUF];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) { close(fd); return 0; }
            if (h->nlmsg_type == NLMSG_ERROR) { close(fd); return -1; }
            if (h->nlmsg_type != RTM_NEWROUTE) continue;
            struct rtmsg *m = (struct rtmsg *)NLMSG_DATA(h);
            if (m->rtm_family != AF_INET || m->rtm_dst_len != 0 || m->rtm_type != RTN_UNICAST) continue;
            uint32_t table = m->rtm_table, gw = 0, priority = 0; int oif = -1; int len = RTM_PAYLOAD(h);
            for (struct rtattr *a = RTM_RTA(m); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
                if (a->rta_type == RTA_OIF && RTA_PAYLOAD(a) >= sizeof(int)) memcpy(&oif, RTA_DATA(a), sizeof(oif));
                else if (a->rta_type == RTA_GATEWAY && RTA_PAYLOAD(a) >= 4) memcpy(&gw, RTA_DATA(a), 4);
                else if (a->rta_type == RTA_TABLE && RTA_PAYLOAD(a) >= 4) memcpy(&table, RTA_DATA(a), 4);
                else if (a->rta_type == RTA_PRIORITY && RTA_PAYLOAD(a) >= 4) memcpy(&priority, RTA_DATA(a), 4);
            }
            if (oif == idx && !selected->found) {
                selected->found = 1; selected->table = table ? table : RT_TABLE_MAIN; selected->oif = oif; selected->gateway = gw; selected->priority = priority; selected->scope = m->rtm_scope; selected->type = m->rtm_type;
            }
        }
    }
}

static int rule_dump(rule_info *first, int verbose);

static uint32_t any_route_table_for_usb0(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return 0;
    char reqbuf[NLMSG_SPACE(sizeof(struct rtmsg))]; memset(reqbuf, 0, sizeof(reqbuf));
    struct nlmsghdr *nlh = (struct nlmsghdr *)reqbuf;
    struct rtmsg *rtm = (struct rtmsg *)NLMSG_DATA(nlh);
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*rtm));
    nlh->nlmsg_type = RTM_GETROUTE;
    nlh->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
    rtm->rtm_family = AF_INET;
    static uint32_t seq = 0x67500000U; nlh->nlmsg_seq = ++seq;
    struct sockaddr_nl dst; memset(&dst, 0, sizeof(dst)); dst.nl_family = AF_NETLINK;
    if (sendto(fd, reqbuf, nlh->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) { close(fd); return 0; }
    int idx = ifindex(); uint32_t candidate = 0; char buf[NETLINK_BUF];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) { close(fd); return candidate; }
            if (h->nlmsg_type != RTM_NEWROUTE) continue;
            struct rtmsg *m = (struct rtmsg *)NLMSG_DATA(h);
            if (m->rtm_family != AF_INET || m->rtm_type != RTN_UNICAST) continue;
            uint32_t table = m->rtm_table; int oif = -1;
            int len = RTM_PAYLOAD(h);
            for (struct rtattr *a = RTM_RTA(m); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
                if (a->rta_type == RTA_OIF && RTA_PAYLOAD(a) >= sizeof(int)) memcpy(&oif, RTA_DATA(a), sizeof(oif));
                else if (a->rta_type == RTA_TABLE && RTA_PAYLOAD(a) >= 4) memcpy(&table, RTA_DATA(a), 4);
            }
            if (oif == idx && table && table != RT_TABLE_LOCAL) candidate = table;
        }
    }
    close(fd); return candidate;
}

static uint32_t route_table_for_usb0(void) {
    rule_info r; if (rule_dump(&r, 0) == 0 && r.found && r.table && r.table != RT_TABLE_LOCAL) return r.table;
    route_info ri; if (route_dump(&ri) == 0 && ri.found && ri.table) return ri.table;
    uint32_t any = any_route_table_for_usb0();
    return any ? any : RT_TABLE_MAIN;
}

static void print_ip(uint32_t v) {
    char s[INET_ADDRSTRLEN]; struct in_addr a = { .s_addr = v };
    if (inet_ntop(AF_INET, &a, s, sizeof(s))) printf("%s", s); else printf("-");
}

static int parse_ipv4(const char *text, uint32_t *out) {
    struct in_addr a;
    if (!text || !out || inet_pton(AF_INET, text, &a) != 1) return -1;
    *out = a.s_addr; return 0;
}

static uint32_t prefix_to_mask(unsigned prefix) {
    if (prefix > 32) return 0;
    if (prefix == 0) return 0;
    return htonl(0xffffffffU << (32U - prefix));
}

static int add_address(uint32_t addr, uint32_t mask) {
    struct { struct nlmsghdr nlh; struct ifaddrmsg ifa; char buf[256]; } req;
    memset(&req, 0, sizeof(req)); req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifa)); req.nlh.nlmsg_type = RTM_NEWADDR; req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_REPLACE;
    req.ifa.ifa_family = AF_INET; req.ifa.ifa_prefixlen = (uint8_t)mask_to_prefix(mask); req.ifa.ifa_scope = RT_SCOPE_UNIVERSE; req.ifa.ifa_index = (unsigned)ifindex();
    if (addattr(&req.nlh, sizeof(req), IFA_LOCAL, &addr, 4) < 0) return -1;
    if (addattr(&req.nlh, sizeof(req), IFA_ADDRESS, &addr, 4) < 0) return -1;
    return rtnl_exchange(&req.nlh);
}

static int delete_all_ipv4(void) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -1;
    char req[NLMSG_SPACE(sizeof(struct ifaddrmsg))]; memset(req, 0, sizeof(req));
    struct nlmsghdr *nlh = (struct nlmsghdr *)req; struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(*ifa)); nlh->nlmsg_type = RTM_GETADDR; nlh->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST; ifa->ifa_family = AF_INET; ifa->ifa_index = (unsigned)ifindex();
    static uint32_t seq = 0x63000000U; nlh->nlmsg_seq = ++seq;
    struct sockaddr_nl dst; memset(&dst, 0, sizeof(dst)); dst.nl_family = AF_NETLINK;
    if (sendto(fd, req, nlh->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) { close(fd); return -1; }
    char buf[NETLINK_BUF]; int count = 0;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0); if (n < 0) { if (errno == EINTR) continue; close(fd); return -1; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_type == NLMSG_DONE) { close(fd); return count; }
            if (h->nlmsg_type != RTM_NEWADDR) continue;
            struct ifaddrmsg *a = (struct ifaddrmsg *)NLMSG_DATA(h); uint32_t local = 0; int len = IFA_PAYLOAD(h);
            for (struct rtattr *r = IFA_RTA(a); RTA_OK(r, len); r = RTA_NEXT(r, len)) if (r->rta_type == IFA_LOCAL && RTA_PAYLOAD(r) >= 4) { memcpy(&local, RTA_DATA(r), 4); break; }
            if (!local) continue;
            struct { struct nlmsghdr nlh; struct ifaddrmsg ifa; char buf[128]; } del; memset(&del, 0, sizeof(del)); del.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(del.ifa)); del.nlh.nlmsg_type = RTM_DELADDR; del.nlh.nlmsg_flags = NLM_F_REQUEST; del.ifa = *a;
            addattr(&del.nlh, sizeof(del), IFA_LOCAL, &local, 4); rtnl_exchange(&del.nlh); ++count;
        }
    }
}

static int replace_route(uint32_t dst, uint8_t prefix, uint32_t gateway, uint32_t table, uint8_t scope, uint8_t proto, int replace, int delete_first) {
    int idx = ifindex();
    struct { struct nlmsghdr nlh; struct rtmsg rtm; char buf[256]; } req;
    memset(&req, 0, sizeof(req)); req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.rtm)); req.nlh.nlmsg_type = delete_first ? RTM_DELROUTE : RTM_NEWROUTE; req.nlh.nlmsg_flags = NLM_F_REQUEST;
    if (!delete_first && replace) req.nlh.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
    req.rtm.rtm_family = AF_INET; req.rtm.rtm_dst_len = prefix; req.rtm.rtm_table = table <= 255 ? (uint8_t)table : RT_TABLE_UNSPEC; req.rtm.rtm_protocol = proto; req.rtm.rtm_scope = scope; req.rtm.rtm_type = RTN_UNICAST;
    if (prefix) if (addattr(&req.nlh, sizeof(req), RTA_DST, &dst, 4) < 0) return -1;
    if (addattr(&req.nlh, sizeof(req), RTA_OIF, &idx, sizeof(idx)) < 0) return -1;
    if (gateway) if (addattr(&req.nlh, sizeof(req), RTA_GATEWAY, &gateway, 4) < 0) return -1;
    if (table > 255) if (addattr(&req.nlh, sizeof(req), RTA_TABLE, &table, 4) < 0) return -1;
    return rtnl_exchange(&req.nlh);
}

static int ensure_connected_route(uint32_t ip, uint32_t mask, uint32_t table) {
    uint32_t net = network_of(ip, mask); uint8_t prefix = (uint8_t)mask_to_prefix(mask);
    return replace_route(net, prefix, 0, table, RT_SCOPE_LINK, RTPROT_STATIC, 1, 0);
}

static int ensure_default_route(uint32_t gateway, uint32_t table) {
    if (!gateway) return -1;
    return replace_route(0, 0, gateway, table, RT_SCOPE_UNIVERSE, RTPROT_STATIC, 1, 0);
}

static int set_mtu(int mtu) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return -1;
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr)); snprintf(ifr.ifr_name, IFNAMSIZ, "%s", iface()); ifr.ifr_mtu = mtu;
    int rc = ioctl(fd, SIOCSIFMTU, &ifr); close(fd); return rc;
}

static int set_rp_filter(int value) {
    char path[256]; snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/rp_filter", iface());
    char v[16]; snprintf(v, sizeof(v), "%d", value); return write_file(path, v);
}

static int get_rp_filter(void) {
    char path[256], v[32]; snprintf(path, sizeof(path), "/proc/sys/net/ipv4/conf/%s/rp_filter", iface());
    return read_file(path, v, sizeof(v)) == 0 ? atoi(v) : -1;
}

static int rule_dump(rule_info *first, int verbose) {
    memset(first, 0, sizeof(*first));
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE); if (fd < 0) return -1;
    char req[NLMSG_SPACE(sizeof(struct fib_rule_hdr))]; memset(req, 0, sizeof(req)); struct nlmsghdr *nlh=(struct nlmsghdr*)req; struct fib_rule_hdr *frh=(struct fib_rule_hdr*)NLMSG_DATA(nlh);
    nlh->nlmsg_len=NLMSG_LENGTH(sizeof(*frh)); nlh->nlmsg_type=RTM_GETRULE; nlh->nlmsg_flags=NLM_F_DUMP|NLM_F_REQUEST; frh->family=AF_INET;
    static uint32_t seq=0x64000000U; nlh->nlmsg_seq=++seq; struct sockaddr_nl dst; memset(&dst,0,sizeof(dst)); dst.nl_family=AF_NETLINK;
    if (sendto(fd, req, nlh->nlmsg_len, 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) { close(fd); return -1; }
    char buf[NETLINK_BUF];
    for (;;) {
        ssize_t n=recv(fd,buf,sizeof(buf),0); if(n<0){if(errno==EINTR)continue;close(fd);return-1;}
        for(struct nlmsghdr *h=(struct nlmsghdr*)buf; NLMSG_OK(h,(unsigned)n); h=NLMSG_NEXT(h,n)){
            if(h->nlmsg_type==NLMSG_DONE){close(fd);return 0;} if(h->nlmsg_type==NLMSG_ERROR)continue; if(h->nlmsg_type!=RTM_NEWRULE)continue;
            struct fib_rule_hdr *m=(struct fib_rule_hdr*)NLMSG_DATA(h); rule_info ri; memset(&ri,0,sizeof(ri)); ri.table=m->table; ri.src_len=m->src_len;
            int len=NLMSG_PAYLOAD(h,sizeof(*m));
            for(struct rtattr *a=(struct rtattr*)((char*)m+NLMSG_ALIGN(sizeof(*m)));RTA_OK(a,len);a=RTA_NEXT(a,len)){
                if(a->rta_type==FRA_TABLE && RTA_PAYLOAD(a)>=4) memcpy(&ri.table,RTA_DATA(a),4);
                else if(a->rta_type==FRA_PRIORITY && RTA_PAYLOAD(a)>=4) memcpy(&ri.priority,RTA_DATA(a),4);
                else if(a->rta_type==FRA_SRC && RTA_PAYLOAD(a)>=4) memcpy(&ri.src,RTA_DATA(a),4);
                else if(a->rta_type==FRA_FWMARK && RTA_PAYLOAD(a)>=4) memcpy(&ri.mark,RTA_DATA(a),4);
                else if(a->rta_type==FRA_FWMASK && RTA_PAYLOAD(a)>=4) memcpy(&ri.mask,RTA_DATA(a),4);
                else if(a->rta_type==FRA_OIFNAME) snprintf(ri.oifname,sizeof(ri.oifname),"%.*s",(int)RTA_PAYLOAD(a),(char*)RTA_DATA(a));
                else if(a->rta_type==FRA_IIFNAME) snprintf(ri.iifname,sizeof(ri.iifname),"%.*s",(int)RTA_PAYLOAD(a),(char*)RTA_DATA(a));
            }
            if (verbose) {
                printf("RULE priority=%u table=%u src=",ri.priority,ri.table); if(ri.src)print_ip(ri.src);else printf("-"); printf("/%u mark=0x%x mask=0x%x iif=%s oif=%s\n",ri.src_len,ri.mark,ri.mask,ri.iifname[0]?ri.iifname:"-",ri.oifname[0]?ri.oifname:"-");
            }
            if(!first->found && ri.table && strcmp(ri.oifname,iface())==0) { *first=ri; first->found=1; }
        }
    }
}

static int has_source_rule(uint32_t src, uint32_t table) {
    rule_info first; memset(&first,0,sizeof(first));
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_ROUTE); if(fd<0)return 0;
    char req[NLMSG_SPACE(sizeof(struct fib_rule_hdr))]; memset(req,0,sizeof(req)); struct nlmsghdr *nlh=(struct nlmsghdr*)req; struct fib_rule_hdr *frh=(struct fib_rule_hdr*)NLMSG_DATA(nlh);
    nlh->nlmsg_len=NLMSG_LENGTH(sizeof(*frh)); nlh->nlmsg_type=RTM_GETRULE; nlh->nlmsg_flags=NLM_F_DUMP|NLM_F_REQUEST; frh->family=AF_INET; static uint32_t seq=0x65000000U; nlh->nlmsg_seq=++seq;
    struct sockaddr_nl dst; memset(&dst,0,sizeof(dst)); dst.nl_family=AF_NETLINK; if(sendto(fd,req,nlh->nlmsg_len,0,(struct sockaddr*)&dst,sizeof(dst))<0){close(fd);return 0;}
    char buf[NETLINK_BUF]; for(;;){ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<0){if(errno==EINTR)continue;break;}for(struct nlmsghdr*h=(struct nlmsghdr*)buf;NLMSG_OK(h,(unsigned)n);h=NLMSG_NEXT(h,n)){if(h->nlmsg_type==NLMSG_DONE){close(fd);return 0;}if(h->nlmsg_type!=RTM_NEWRULE)continue;struct fib_rule_hdr*m=(struct fib_rule_hdr*)NLMSG_DATA(h);uint32_t t=m->table,s=0;int len=NLMSG_PAYLOAD(h,sizeof(*m));for(struct rtattr*a=(struct rtattr*)((char*)m+NLMSG_ALIGN(sizeof(*m)));RTA_OK(a,len);a=RTA_NEXT(a,len)){if(a->rta_type==FRA_TABLE&&RTA_PAYLOAD(a)>=4)memcpy(&t,RTA_DATA(a),4);else if(a->rta_type==FRA_SRC&&RTA_PAYLOAD(a)>=4)memcpy(&s,RTA_DATA(a),4);}if(t==table&&s==src){close(fd);return 1;}}}close(fd);return 0;
}

static int add_source_rule(uint32_t src, uint32_t table) {
    if (!src || !table || table == RT_TABLE_MAIN || has_source_rule(src, table)) return 0;
    struct { struct nlmsghdr nlh; struct fib_rule_hdr frh; char buf[256]; } req; memset(&req,0,sizeof(req));
    req.nlh.nlmsg_len=NLMSG_LENGTH(sizeof(req.frh)); req.nlh.nlmsg_type=RTM_NEWRULE; req.nlh.nlmsg_flags=NLM_F_REQUEST|NLM_F_CREATE|NLM_F_EXCL; req.frh.family=AF_INET; req.frh.src_len=32; req.frh.table=table;
    uint32_t pri=POLICY_RULE_PRIORITY; if(addattr(&req.nlh,sizeof(req),FRA_PRIORITY,&pri,4)<0)return-1; if(addattr(&req.nlh,sizeof(req),FRA_SRC,&src,4)<0)return-1; if(table>255&&addattr(&req.nlh,sizeof(req),FRA_TABLE,&table,4)<0)return-1;
    return rtnl_exchange(&req.nlh);
}

static int delete_source_rule(uint32_t src, uint32_t table) {
    if (!src || !table || table == RT_TABLE_MAIN) return 0;
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE); if (fd < 0) return -1;
    char req[NLMSG_SPACE(sizeof(struct fib_rule_hdr))]; memset(req, 0, sizeof(req));
    struct nlmsghdr *nlh=(struct nlmsghdr*)req; struct fib_rule_hdr *frh=(struct fib_rule_hdr*)NLMSG_DATA(nlh);
    nlh->nlmsg_len=NLMSG_LENGTH(sizeof(*frh)); nlh->nlmsg_type=RTM_GETRULE; nlh->nlmsg_flags=NLM_F_DUMP|NLM_F_REQUEST; frh->family=AF_INET;
    static uint32_t seq=0x67900000U; nlh->nlmsg_seq=++seq; struct sockaddr_nl dst; memset(&dst,0,sizeof(dst));dst.nl_family=AF_NETLINK;
    if(sendto(fd,req,nlh->nlmsg_len,0,(struct sockaddr*)&dst,sizeof(dst))<0){close(fd);return-1;}
    char buf[NETLINK_BUF]; int removed=0;
    for(;;){ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<0){if(errno==EINTR)continue;close(fd);return-1;}for(struct nlmsghdr*h=(struct nlmsghdr*)buf;NLMSG_OK(h,(unsigned)n);h=NLMSG_NEXT(h,n)){if(h->nlmsg_type==NLMSG_DONE){close(fd);return removed;}if(h->nlmsg_type!=RTM_NEWRULE)continue;struct fib_rule_hdr*m=(struct fib_rule_hdr*)NLMSG_DATA(h);uint32_t t=m->table,s=0,pri=0;int len=NLMSG_PAYLOAD(h,sizeof(*m));for(struct rtattr*a=(struct rtattr*)((char*)m+NLMSG_ALIGN(sizeof(*m)));RTA_OK(a,len);a=RTA_NEXT(a,len)){if(a->rta_type==FRA_TABLE&&RTA_PAYLOAD(a)>=4)memcpy(&t,RTA_DATA(a),4);else if(a->rta_type==FRA_SRC&&RTA_PAYLOAD(a)>=4)memcpy(&s,RTA_DATA(a),4);else if(a->rta_type==FRA_PRIORITY&&RTA_PAYLOAD(a)>=4)memcpy(&pri,RTA_DATA(a),4);}if(t!=table||s!=src||pri!=POLICY_RULE_PRIORITY)continue;struct{struct nlmsghdr nlh;struct fib_rule_hdr frh;char buf[256];}del;memset(&del,0,sizeof(del));del.nlh.nlmsg_len=NLMSG_LENGTH(sizeof(del.frh));del.nlh.nlmsg_type=RTM_DELRULE;del.nlh.nlmsg_flags=NLM_F_REQUEST;del.frh=*m;uint32_t p=pri;if(addattr(&del.nlh,sizeof(del),FRA_PRIORITY,&p,4)<0)continue;if(addattr(&del.nlh,sizeof(del),FRA_SRC,&s,4)<0)continue;if(t>255)addattr(&del.nlh,sizeof(del),FRA_TABLE,&t,4);if(rtnl_exchange(&del.nlh)==0)++removed;}}
}

static int flush_usb_routes(uint32_t table) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE); if (fd < 0) return -1;
    char req[NLMSG_SPACE(sizeof(struct rtmsg))]; memset(req, 0, sizeof(req));
    struct nlmsghdr *nlh=(struct nlmsghdr*)req; struct rtmsg *rtm=(struct rtmsg*)NLMSG_DATA(nlh);
    nlh->nlmsg_len=NLMSG_LENGTH(sizeof(*rtm)); nlh->nlmsg_type=RTM_GETROUTE; nlh->nlmsg_flags=NLM_F_DUMP|NLM_F_REQUEST; rtm->rtm_family=AF_INET;
    static uint32_t seq=0x67800000U; nlh->nlmsg_seq=++seq; struct sockaddr_nl dst; memset(&dst,0,sizeof(dst)); dst.nl_family=AF_NETLINK;
    if(sendto(fd,req,nlh->nlmsg_len,0,(struct sockaddr*)&dst,sizeof(dst))<0){close(fd);return-1;}
    int idx=ifindex(), count=0; char buf[NETLINK_BUF];
    for(;;){ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<0){if(errno==EINTR)continue;close(fd);return-1;}for(struct nlmsghdr*h=(struct nlmsghdr*)buf;NLMSG_OK(h,(unsigned)n);h=NLMSG_NEXT(h,n)){if(h->nlmsg_type==NLMSG_DONE){close(fd);return count;}if(h->nlmsg_type!=RTM_NEWROUTE)continue;struct rtmsg*m=(struct rtmsg*)NLMSG_DATA(h);if(m->rtm_family!=AF_INET||m->rtm_table==RT_TABLE_LOCAL)continue;uint32_t t=m->rtm_table;int oif=-1;uint32_t dstip=0;int len=RTM_PAYLOAD(h);for(struct rtattr*a=RTM_RTA(m);RTA_OK(a,len);a=RTA_NEXT(a,len)){if(a->rta_type==RTA_OIF&&RTA_PAYLOAD(a)>=sizeof(int))memcpy(&oif,RTA_DATA(a),sizeof(oif));else if(a->rta_type==RTA_TABLE&&RTA_PAYLOAD(a)>=4)memcpy(&t,RTA_DATA(a),4);else if(a->rta_type==RTA_DST&&RTA_PAYLOAD(a)>=4)memcpy(&dstip,RTA_DATA(a),4);}if(oif!=idx||t!=table)continue;struct{struct nlmsghdr nlh;struct rtmsg rtm;char buf[256];}del;memset(&del,0,sizeof(del));del.nlh.nlmsg_len=NLMSG_LENGTH(sizeof(del.rtm));del.nlh.nlmsg_type=RTM_DELROUTE;del.nlh.nlmsg_flags=NLM_F_REQUEST;del.rtm=*m;if(m->rtm_dst_len>0)addattr(&del.nlh,sizeof(del),RTA_DST,&dstip,4);addattr(&del.nlh,sizeof(del),RTA_OIF,&idx,sizeof(idx));if(t>255)addattr(&del.nlh,sizeof(del),RTA_TABLE,&t,4);if(rtnl_exchange(&del.nlh)==0)++count;}}}

static int flush_neighbors(void) {
    int fd=socket(AF_NETLINK,SOCK_RAW|SOCK_CLOEXEC,NETLINK_ROUTE); if(fd<0)return-1;
    char req[NLMSG_SPACE(sizeof(struct ndmsg))]; memset(req,0,sizeof(req)); struct nlmsghdr*nlh=(struct nlmsghdr*)req; struct ndmsg*ndm=(struct ndmsg*)NLMSG_DATA(nlh);
    nlh->nlmsg_len=NLMSG_LENGTH(sizeof(*ndm)); nlh->nlmsg_type=RTM_GETNEIGH; nlh->nlmsg_flags=NLM_F_DUMP|NLM_F_REQUEST; ndm->ndm_family=AF_INET; ndm->ndm_ifindex=ifindex(); static uint32_t seq=0x66000000U;nlh->nlmsg_seq=++seq;
    struct sockaddr_nl dst; memset(&dst,0,sizeof(dst));dst.nl_family=AF_NETLINK;if(sendto(fd,req,nlh->nlmsg_len,0,(struct sockaddr*)&dst,sizeof(dst))<0){close(fd);return-1;}
    char buf[NETLINK_BUF];int count=0;for(;;){ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<0){if(errno==EINTR)continue;close(fd);return-1;}for(struct nlmsghdr*h=(struct nlmsghdr*)buf;NLMSG_OK(h,(unsigned)n);h=NLMSG_NEXT(h,n)){if(h->nlmsg_type==NLMSG_DONE){close(fd);return count;}if(h->nlmsg_type!=RTM_NEWNEIGH)continue;struct ndmsg*m=(struct ndmsg*)NLMSG_DATA(h);if(m->ndm_ifindex!=ifindex())continue;uint8_t dstip[4];int have=0;int len=NLMSG_PAYLOAD(h,sizeof(*m));for(struct rtattr*a=(struct rtattr*)((char*)m+NLMSG_ALIGN(sizeof(*m)));RTA_OK(a,len);a=RTA_NEXT(a,len))if(a->rta_type==NDA_DST&&RTA_PAYLOAD(a)>=4){memcpy(dstip,RTA_DATA(a),4);have=1;}if(!have)continue;struct{struct nlmsghdr nlh;struct ndmsg ndm;char buf[128];}del;memset(&del,0,sizeof(del));del.nlh.nlmsg_len=NLMSG_LENGTH(sizeof(del.ndm));del.nlh.nlmsg_type=RTM_DELNEIGH;del.nlh.nlmsg_flags=NLM_F_REQUEST;del.ndm=*m;addattr(&del.nlh,sizeof(del),NDA_DST,dstip,4);if(rtnl_exchange(&del.nlh)==0)++count;}}
}

static size_t build_dhcp_packet(uint8_t *buf,size_t cap,uint32_t xid,const uint8_t mac[6],uint8_t type,uint32_t requested,uint32_t server) {
    if(cap < 240) return 0;
    memset(buf,0,cap); buf[0]=1;buf[1]=1;buf[2]=6; memcpy(buf+4,&xid,4);uint16_t flags=htons(0x8000);memcpy(buf+10,&flags,2);memcpy(buf+28,mac,6);uint32_t cookie=htonl(DHCP_MAGIC);memcpy(buf+236,&cookie,4);size_t o=240;buf[o++]=53;buf[o++]=1;buf[o++]=type;buf[o++]=61;buf[o++]=7;buf[o++]=1;memcpy(buf+o,mac,6);o+=6;if(requested){buf[o++]=50;buf[o++]=4;memcpy(buf+o,&requested,4);o+=4;}if(server){buf[o++]=54;buf[o++]=4;memcpy(buf+o,&server,4);o+=4;}uint8_t prl[]={1,3,6,15,51,54};buf[o++]=55;buf[o++]=sizeof(prl);memcpy(buf+o,prl,sizeof(prl));o+=sizeof(prl);buf[o++]=255;return o;
}

static int dhcp_opt(const uint8_t *pkt,size_t len,int want,uint8_t*out,size_t*outlen){size_t i=240;if(len<i+1)return-1;while(i<len){uint8_t tag=pkt[i++];if(tag==0)continue;if(tag==255)break;if(i>=len)break;uint8_t l=pkt[i++];if(i+l>len)break;if(tag==want){size_t n=l<*outlen?l:*outlen;memcpy(out,pkt+i,n);*outlen=n;return 0;}i+=l;}return-1;}

static int dhcp_client(dhcp_result *res) {
    memset(res,0,sizeof(*res));uint8_t mac[6];if(get_mac(mac)<0)return-1;int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,IPPROTO_UDP);if(fd<0)return-1;int yes=1;setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes));if(bind_to_iface(fd)<0){close(fd);return-1;}
    struct sockaddr_in local;memset(&local,0,sizeof(local));local.sin_family=AF_INET;local.sin_port=htons(DHCP_CLIENT_PORT);local.sin_addr.s_addr=INADDR_ANY;if(bind(fd,(struct sockaddr*)&local,sizeof(local))<0){close(fd);return-1;}
    srand((unsigned)(time(NULL)^getpid()));uint32_t xid=((uint32_t)rand()^0x9e3779b9U);uint8_t buf[1500];size_t plen=build_dhcp_packet(buf,sizeof(buf),xid,mac,1,0,0);struct sockaddr_in dst;memset(&dst,0,sizeof(dst));dst.sin_family=AF_INET;dst.sin_port=htons(DHCP_SERVER_PORT);dst.sin_addr.s_addr=INADDR_BROADCAST;if(sendto(fd,buf,plen,0,(struct sockaddr*)&dst,sizeof(dst))<0){close(fd);return-1;}
    struct pollfd p={.fd=fd,.events=POLLIN};uint64_t deadline=(uint64_t)time(NULL)+6;uint32_t offered=0,server_id=0;
    for(;;){int left=(int)(deadline-(uint64_t)time(NULL));if(left<=0){close(fd);return-1;}if(poll(&p,1,left*1000)<=0){close(fd);return-1;}ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<240||memcmp(buf+28,mac,6)!=0)continue;uint32_t gotx;memcpy(&gotx,buf+4,4);if(gotx!=xid)continue;uint8_t mt=0;size_t ml=1;if(dhcp_opt(buf,(size_t)n,53,&mt,&ml)==0&&mt==2){offered=0;memcpy(&offered,buf+16,4);uint8_t b[4];size_t l=4;if(dhcp_opt(buf,(size_t)n,54,b,&l)==0)memcpy(&server_id,b,4);break;}}
    plen=build_dhcp_packet(buf,sizeof(buf),xid,mac,3,offered,server_id);if(sendto(fd,buf,plen,0,(struct sockaddr*)&dst,sizeof(dst))<0){close(fd);return-1;}deadline=(uint64_t)time(NULL)+6;
    for(;;){int left=(int)(deadline-(uint64_t)time(NULL));if(left<=0){close(fd);return-1;}if(poll(&p,1,left*1000)<=0){close(fd);return-1;}ssize_t n=recv(fd,buf,sizeof(buf),0);if(n<240||memcmp(buf+28,mac,6)!=0)continue;uint32_t gotx;memcpy(&gotx,buf+4,4);if(gotx!=xid)continue;uint32_t yi;memcpy(&yi,buf+16,4);if(yi!=offered)continue;uint8_t mt=0;size_t ml=1;if(dhcp_opt(buf,(size_t)n,53,&mt,&ml)!=0||mt!=5)continue;res->yiaddr=yi;res->server_id=server_id;uint8_t b[4];size_t l=4;if(dhcp_opt(buf,(size_t)n,1,b,&l)==0)memcpy(&res->subnet_mask,b,4);l=4;if(dhcp_opt(buf,(size_t)n,3,b,&l)==0)memcpy(&res->router,b,4);uint8_t dns[8];l=sizeof(dns);if(dhcp_opt(buf,(size_t)n,6,dns,&l)==0){if(l>=4)memcpy(&res->dns1,dns,4);if(l>=8)memcpy(&res->dns2,dns+4,4);}uint8_t ltbuf[4];l=4;if(dhcp_opt(buf,(size_t)n,51,ltbuf,&l)==0){uint32_t lt;memcpy(&lt,ltbuf,4);res->lease_time=ntohl(lt);}close(fd);return 0;}
}

static int ping4(uint32_t dst, int timeout_ms, int count) {
    int fd=socket(AF_INET,SOCK_RAW|SOCK_CLOEXEC,IPPROTO_ICMP);if(fd<0)return-1;if(bind_to_iface(fd)<0){close(fd);return-1;}struct timeval tv={.tv_sec=timeout_ms/1000,.tv_usec=(timeout_ms%1000)*1000};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    struct {struct icmphdr icmp;char payload[24];} pkt;struct sockaddr_in sa;memset(&sa,0,sizeof(sa));sa.sin_family=AF_INET;sa.sin_addr.s_addr=dst;int success=0;
    for(int i=0;i<count;i++){memset(&pkt,0,sizeof(pkt));pkt.icmp.type=ICMP_ECHO;pkt.icmp.code=0;pkt.icmp.un.echo.id=htons((uint16_t)getpid());pkt.icmp.un.echo.sequence=htons((uint16_t)(i+1));memset(pkt.payload,'U',sizeof(pkt.payload));pkt.icmp.checksum=csum16(&pkt,sizeof(pkt));if(sendto(fd,&pkt,sizeof(pkt),0,(struct sockaddr*)&sa,sizeof(sa))<0)continue;uint8_t buf[2048];ssize_t n=recv(fd,buf,sizeof(buf),0);if(n>0){struct iphdr*ip=(struct iphdr*)buf;size_t ihl=(size_t)ip->ihl*4;if(n > (ssize_t)(ihl + sizeof(struct icmphdr))){struct icmphdr*ic=(struct icmphdr*)(buf+ihl);if(ic->type==ICMP_ECHOREPLY&&ic->un.echo.id==pkt.icmp.un.echo.id){success++;}}}}
    close(fd);return success>0?0:-1;
}

static int dns_query(uint32_t server) {
    int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);if(fd<0)return-1;if(bind_to_iface(fd)<0){close(fd);return-1;}struct timeval tv={.tv_sec=3,.tv_usec=0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));uint8_t buf[512];memset(buf,0,sizeof(buf));uint16_t id=htons((uint16_t)(getpid()^(unsigned)time(NULL)));memcpy(buf,&id,2);buf[2]=1;buf[5]=1;size_t off=12;const char*name="example.com";const char*p=name;while(*p){const char*dot=strchr(p,'.');size_t l=dot?(size_t)(dot-p):strlen(p);buf[off++]=(uint8_t)l;memcpy(buf+off,p,l);off+=l;if(!dot)break;p=dot+1;}buf[off++]=0;buf[off++]=0;buf[off++]=1;buf[off++]=0;buf[off++]=1;struct sockaddr_in sa;memset(&sa,0,sizeof(sa));sa.sin_family=AF_INET;sa.sin_port=htons(53);sa.sin_addr.s_addr=server;if(sendto(fd,buf,off,0,(struct sockaddr*)&sa,sizeof(sa))<0){close(fd);return-1;}ssize_t n=recv(fd,buf,sizeof(buf),0);close(fd);if(n<12)return-1;uint16_t rid;memcpy(&rid,buf,2);if(rid!=id)return-1;uint16_t flags=ntohs(*(uint16_t*)(buf+2));uint16_t an=ntohs(*(uint16_t*)(buf+6));return ((flags&0x000f)==0&&an>0)?0:-1;
}

static void print_stats(void){const char*names[]={"rx_bytes","rx_packets","tx_bytes","tx_packets","rx_errors","tx_errors","multicast"};for(size_t i=0;i<sizeof(names)/sizeof(names[0]);++i){char path[256],v[64]="-";snprintf(path,sizeof(path),"/sys/class/net/%s/statistics/%s",iface(),names[i]);read_file(path,v,sizeof(v));printf("STAT_%s=%s\n",names[i],v);}}

static void usb_state(void){char v[512];const char*props[]={"sys.usb.config","sys.usb.state","persist.sys.usb.config","sys.usb.ffs.ready","sys.usb.ffs.rndis.ready","sys.usb.configfs"};for(size_t i=0;i<sizeof(props)/sizeof(props[0]);++i){char cmd[640];snprintf(cmd,sizeof(cmd),"/system/bin/getprop %s 2>/dev/null",props[i]);FILE*f=popen(cmd,"r");if(f){if(fgets(v,sizeof(v),f)){v[strcspn(v,"\r\n")]=0;printf("PROP_%s=%s\n",props[i],v);}pclose(f);}}char path[256];snprintf(path,sizeof(path),"/sys/class/net/%s/uevent",iface());if(read_file(path,v,sizeof(v))==0)printf("--- UEVENT ---\n%s\n",v);}

static void status(void){int idx=ifindex();printf("INTERFACE=%s\n",iface());printf("NATIVE_INTERFACE=%s\n",idx?"YES":"NO");if(!idx)return;int up=0,car=0,mtu=0;get_link_info(&up,&car,&mtu);printf("LINK=%s\n",up?"up":"down");printf("CARRIER=%d\n",car);printf("MTU=%d\n",mtu);char macpath[256],mac[64]="-",op[64]="-";snprintf(macpath,sizeof(macpath),"/sys/class/net/%s/address",iface());read_file(macpath,mac,sizeof(mac));printf("MAC=%s\n",mac);snprintf(macpath,sizeof(macpath),"/sys/class/net/%s/operstate",iface());if(read_file(macpath,op,sizeof(op))==0)printf("OPERSTATE=%s\n",op);uint32_t ip=0,mask=0;if(current_ipv4(&ip,&mask)==0){printf("IP=");print_ip(ip);printf("\nPREFIX=%u\n",mask_to_prefix(mask));}else printf("IP=-\nPREFIX=0\n");route_info ri;route_dump(&ri);uint32_t table=ri.found?ri.table:route_table_for_usb0();printf("TABLE=%u\n",table);if(ri.found&&ri.gateway){printf("GW=");print_ip(ri.gateway);printf("\n");printf("ROUTE=OK\n");printf("GWTEST=%s\n",ping4(ri.gateway,1500,1)==0?"OK":"FAIL");printf("INET=%s\n",ping4(inet_addr("1.1.1.1"),2200,1)==0?"OK":"FAIL");printf("DNS=%s\n",dns_query(inet_addr("1.1.1.1"))==0?"OK":"FAIL");}else{printf("GW=-\nROUTE=FAIL\nGWTEST=NO-GW\nINET=FAIL\nDNS=FAIL\n");}printf("RP_FILTER=%d\n",get_rp_filter());}

static void diagnose(void){status();printf("--- ROUTES (/proc/net/route) ---\n");FILE*f=fopen("/proc/net/route","r");if(f){char line[1024];while(fgets(line,sizeof(line),f))fputs(line,stdout);fclose(f);}printf("--- POLICY RULES ---\n");rule_info r;rule_dump(&r,1);printf("--- STATS ---\n");print_stats();printf("--- USB/RNDIS STATE ---\n");usb_state();char p[256],v[64];snprintf(p,sizeof(p),"/sys/class/net/%s/flags",iface());if(read_file(p,v,sizeof(v))==0)printf("FLAGS=%s\n",v);snprintf(p,sizeof(p),"/sys/class/net/%s/speed",iface());if(read_file(p,v,sizeof(v))==0)printf("SPEED=%s\n",v);}

static int recover(int aggressive){
    if(!ifindex()){puts("RESULT=NO_INTERFACE");return 10;}
    int changed=0; if(set_link_state(1)==0)changed=1; uint32_t ip=0,mask=0;
    if(current_ipv4(&ip,&mask)<0){dhcp_result d;if(dhcp_client(&d)==0){if(!d.subnet_mask)d.subnet_mask=htonl(0xffffff00U);if(add_address(d.yiaddr,d.subnet_mask)==0){ip=d.yiaddr;mask=d.subnet_mask;changed=1;}printf("DHCP=OK\nDNS1=");if(d.dns1)print_ip(d.dns1);else printf("-");printf("\nDNS2=");if(d.dns2)print_ip(d.dns2);printf("\nGW=");if(d.router)print_ip(d.router);else printf("-");printf("\n");}else puts("DHCP=FAIL");}
    route_info ri;route_dump(&ri);uint32_t table=ri.found?ri.table:route_table_for_usb0();uint32_t gw=ri.gateway;
    if(!gw&&ip&&aggressive){uint32_t n=ntohl(ip)&ntohl(mask);uint32_t first=(n+1);gw=htonl(first);printf("GW_GUESS=");print_ip(gw);printf("\n");}
    if(ip&&mask){if(ensure_connected_route(ip,mask,table)==0){puts("CONNECTED_ROUTE=OK");changed=1;}else puts("CONNECTED_ROUTE=FAIL");}
    if(gw){if(ensure_default_route(gw,table)==0){puts("DEFAULT_ROUTE=OK");changed=1;}else puts("DEFAULT_ROUTE=FAIL");if(ip&&table!=RT_TABLE_MAIN){int rr=add_source_rule(ip,table);printf("SOURCE_RULE=%s\n",rr==0?"OK":"FAIL");}}
    if(get_rp_filter()>0){if(set_rp_filter(2)==0)puts("RP_FILTER=2");else puts("RP_FILTER=FAIL");}
    char mtu_path[256],mtu_v[32];snprintf(mtu_path,sizeof(mtu_path),"/sys/class/net/%s/mtu",iface());if(read_file(mtu_path,mtu_v,sizeof(mtu_v))==0){int mtu=atoi(mtu_v);if(mtu>0&&mtu>2000){if(set_mtu(AUTO_MTU)==0)puts("MTU=1500");}}
    flush_neighbors(); if(aggressive) puts("AGGRESSIVE_MODE=YES"); status(); printf("RESULT=%s\n",changed?"CHANGED":"CHECKED"); return 0;
}

static void usage(void){puts("usb0-native commands: check status up down dhcp static-ip route route-connected policy policy-repair policy-delete flush-usb-routes neigh arp-gateway arp-set set-mac ensure-mac reset recover recover-aggressive mtu set-mtu rpfilter set-rpfilter usb-reset test diagnose snapshot iface");}

int main(int argc,char**argv){
    const char*env_if=getenv("USB0_IFACE"); if(env_if&&*env_if) snprintf(g_ifname,sizeof(g_ifname),"%.*s",IFNAMSIZ-1,env_if);
    const char*cmd=argc>1?argv[1]:"status";
    if(strcmp(cmd,"set-iface")==0){if(argc<3)return 2;snprintf(g_ifname,sizeof(g_ifname),"%.*s",IFNAMSIZ-1,argv[2]);printf("INTERFACE=%s\n",iface());return ifindex()?0:1;}
    if(strcmp(cmd,"check")==0){printf("native=YES\ninterface=%s\n",ifindex()?"YES":"NO");printf("ifname=%s\n",iface());return 0;}
    if(strcmp(cmd,"iface")==0){printf("INTERFACE=%s\n",iface());return ifindex()?0:1;}
    if(strcmp(cmd,"status")==0){status();return 0;}
    if(strcmp(cmd,"up")==0||strcmp(cmd,"down")==0){if(!ifindex()){puts("RESULT=NO_INTERFACE");return 1;}int rc=set_link_state(strcmp(cmd,"up")==0);printf("RESULT=%s\n",rc==0?"OK":"FAIL");return rc==0?0:1;}
    if(strcmp(cmd,"usb-reset")==0){if(argc<4){puts("RESULT=USAGE");return 2;}unsigned bus=(unsigned)strtoul(argv[2],NULL,10),dev=(unsigned)strtoul(argv[3],NULL,10);int rc=usb_reset_device(bus,dev);printf("USB_RESET=%s BUS=%u DEV=%u\n",rc==0?"OK":"FAIL",bus,dev);return rc==0?0:1;}
    if(strcmp(cmd,"dhcp")==0){if(!ifindex()){puts("RESULT=NO_INTERFACE");return 1;}if(set_link_state(1)<0)perror("link up");dhcp_result d;if(dhcp_client(&d)<0){puts("DHCP=FAIL");return 1;}if(!d.subnet_mask)d.subnet_mask=htonl(0xffffff00U);printf("DHCP=OK\nIP=");print_ip(d.yiaddr);printf("\nMASK=");print_ip(d.subnet_mask);printf("\nGW=");if(d.router)print_ip(d.router);else printf("-");printf("\nDNS1=");if(d.dns1)print_ip(d.dns1);else printf("-");printf("\nDNS2=");if(d.dns2)print_ip(d.dns2);printf("\nLEASE=%u\n",d.lease_time);if(add_address(d.yiaddr,d.subnet_mask)<0)perror("add_address");if(d.router)ensure_connected_route(d.yiaddr,d.subnet_mask,route_table_for_usb0()),ensure_default_route(d.router,route_table_for_usb0());return 0;}
    if(strcmp(cmd,"route") == 0 || strcmp(cmd,"route-connected") == 0){if(!ifindex())return 1;uint32_t ip=0,mask=0;if(current_ipv4(&ip,&mask)<0){puts("RESULT=NO_IP");return 2;}route_info ri;route_dump(&ri);uint32_t table=ri.found?ri.table:route_table_for_usb0();if(strcmp(cmd,"route-connected")==0){int rc=ensure_connected_route(ip,mask,table);printf("TABLE=%u\nRESULT=%d\n",table,rc==0?0:1);return rc==0?0:1;}uint32_t gw=ri.gateway;if(argc>2&&inet_pton(AF_INET,argv[2],&gw)!=1)gw=0;if(!gw){uint32_t n=ntohl(ip)&ntohl(mask);gw=htonl(n+1);}ensure_connected_route(ip,mask,table);int rc=ensure_default_route(gw,table);flush_neighbors();printf("TABLE=%u\nGATEWAY=",table);print_ip(gw);printf("\nRESULT=%d\n",rc==0?0:1);return rc==0?0:1;}
    if(strcmp(cmd,"static-ip")==0){
        if(!ifindex()||argc<5){puts("RESULT=USAGE");return 2;}
        uint32_t ip=0,gw=0; unsigned prefix=(unsigned)atoi(argv[3]);
        if(parse_ipv4(argv[2],&ip)<0||parse_ipv4(argv[4],&gw)<0||prefix>32){puts("RESULT=BAD_ADDRESS");return 2;}
        uint32_t mask=prefix_to_mask(prefix); if(!mask&&prefix!=0){puts("RESULT=BAD_PREFIX");return 2;}
        if(set_link_state(1)<0) perror("link up");
        delete_all_ipv4();
        int a=add_address(ip,mask); int table=(int)route_table_for_usb0(); int c=ensure_connected_route(ip,mask,(uint32_t)table); int r=ensure_default_route(gw,(uint32_t)table); flush_neighbors();
        printf("STATIC_IP=");print_ip(ip);printf("/%u\nGATEWAY=",prefix);print_ip(gw);printf("\nTABLE=%d\nRESULT=%d\n",table,(a==0&&c==0&&r==0)?0:1);return (a==0&&c==0&&r==0)?0:1;
    }
    if(strcmp(cmd,"policy-delete")==0){uint32_t ip=0;if(argc>2&&parse_ipv4(argv[2],&ip)==0){uint32_t table=argc>3?(uint32_t)strtoul(argv[3],NULL,10):route_table_for_usb0();int rc=delete_source_rule(ip,table);printf("POLICY_DELETE=%d\n",rc);return rc<0?1:0;}puts("RESULT=USAGE");return 2;}
    if(strcmp(cmd,"policy-repair")==0){uint32_t ip=0,mask=0;if(current_ipv4(&ip,&mask)<0){puts("RESULT=NO_IP");return 2;}uint32_t table=route_table_for_usb0();if(table==RT_TABLE_MAIN){puts("TABLE=MAIN");puts("RESULT=NOT_NEEDED");return 0;}int rc=add_source_rule(ip,table);printf("TABLE=%u\nSOURCE_RULE=%s\n",table,rc==0?"OK":"FAIL");return rc==0?0:1;}
    if(strcmp(cmd,"flush-usb-routes")==0){uint32_t table=route_table_for_usb0();int rc=flush_usb_routes(table);printf("TABLE=%u\nFLUSHED=%d\n",table,rc);return rc<0?1:0;}
    if(strcmp(cmd,"neigh")==0){int rc=flush_neighbors();printf("RESULT=%d\n",rc==0?0:1);return rc==0?0:1;}
    if(strcmp(cmd,"arp-gateway")==0)return arp_gateway();
    if(strcmp(cmd,"arp-set")==0){if(argc<4){puts("RESULT=USAGE");return 2;}return arp_set_cmd(argv[2],argv[3]);}
    if(strcmp(cmd,"set-mac")==0){if(argc<3){puts("RESULT=USAGE");return 2;}uint8_t mac[6];if(parse_mac(argv[2],mac)<0||mac_invalid(mac)){puts("RESULT=BAD_MAC");return 2;}int rc=set_mac_addr(mac);printf("MAC=");print_mac(mac);printf("\nRESULT=%s\n",rc==0?"OK":"FAIL");return rc==0?0:1;}
    if(strcmp(cmd,"ensure-mac")==0){if(!ifindex()){puts("RESULT=NO_INTERFACE");return 1;}uint8_t mac[6];if(get_mac(mac)==0&&!mac_invalid(mac)){printf("MAC=");print_mac(mac);printf("\nRESULT=EXISTING\n");return 0;}FILE*f=fopen("/dev/urandom","rb");if(!f)return 1;if(fread(mac,1,6,f)!=6){fclose(f);return 1;}fclose(f);mac[0]=(uint8_t)((mac[0]&0xfeU)|2U);int rc=set_mac_addr(mac);printf("MAC=");print_mac(mac);printf("\nRESULT=%s\n",rc==0?"GENERATED":"FAIL");return rc==0?0:1;}
    if(strcmp(cmd,"reset")==0){if(!ifindex())return 1;set_link_state(0);sleep(1);if(set_link_state(1)<0)return 1;dhcp_result d;if(dhcp_client(&d)==0){if(!d.subnet_mask)d.subnet_mask=htonl(0xffffff00U);add_address(d.yiaddr,d.subnet_mask);if(d.router)ensure_default_route(d.router,route_table_for_usb0());ensure_connected_route(d.yiaddr,d.subnet_mask,route_table_for_usb0());}flush_neighbors();status();return 0;}
    if(strcmp(cmd,"recover")==0)return recover(0);
    if(strcmp(cmd,"recover-aggressive")==0)return recover(1);
    if(strcmp(cmd,"mtu")==0){char path[256],v[64];snprintf(path,sizeof(path),"/sys/class/net/%s/mtu",iface());read_file(path,v,sizeof(v));printf("MTU=%s\n",v);return 0;}
    if(strcmp(cmd,"set-mtu")==0){int mtu=argc>2?atoi(argv[2]):AUTO_MTU;if(mtu<576||mtu>9000)return 2;int rc=set_mtu(mtu);printf("MTU_RESULT=%d\n",rc==0?0:1);return rc==0?0:1;}
    if(strcmp(cmd,"rpfilter")==0){printf("RP_FILTER=%d\n",get_rp_filter());return 0;}
    if(strcmp(cmd,"set-rpfilter")==0){int v=argc>2?atoi(argv[2]):2;if(v<0||v>2)return 2;int rc=set_rp_filter(v);printf("RP_FILTER_RESULT=%d\n",rc==0?0:1);return rc==0?0:1;}
    if(strcmp(cmd,"test")==0){route_info ri;route_dump(&ri);if(!ri.gateway) {puts("GATEWAY=NO-GW");return 2;}printf("GATEWAY=%s\n",ping4(ri.gateway,2000,3)==0?"OK":"FAIL");printf("INET=%s\n",ping4(inet_addr("1.1.1.1"),3000,3)==0?"OK":"FAIL");printf("DNS=%s\n",dns_query(inet_addr("1.1.1.1"))==0?"OK":"FAIL");return 0;}
    if(strcmp(cmd,"diagnose")==0){diagnose();return 0;}
    if(strcmp(cmd,"policy")==0){rule_info r;rule_dump(&r,1);return 0;}
    if(strcmp(cmd,"snapshot")==0){status();puts("--- POLICY ---");rule_info r;rule_dump(&r,1);return 0;}
    usage();return 2;
}
