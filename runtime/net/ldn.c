// nn::ldn (local wireless play) over IP. The game that creates a network (the hub's host, an "access point") listens
// on a TCP port (ini [Multiplayer] Port); the others ("stations") find it with Scan - a LAN broadcast plus the
// addresses in ini [Multiplayer] Join - and Connect to it. Each station keeps that one connection: the host hands
// out node numbers and addresses (169.254.x.n, the host n = 1), sends the network's NetworkInfo whenever it changes
// (players joining or leaving, the hub's advertise data), and relays the game's packets between the players
// (net.h, vsock.c). Only the host needs a reachable port.
//
// Link frames: u32 size (of what follows), u8 type, payload. Structures are the SDK's (sizes and offsets below).
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <string.h>

#include "../config.h"
#include "../hle.h"
#include "../kernel.h"
#include "net.h"

#ifdef _WIN32
typedef SOCKET hsock;
#define BADSOCK INVALID_SOCKET
#define hclose closesocket
static void hnet_init(void) {
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static void hset_nonblock(hsock s, bool on) { u_long v = on; ioctlsocket(s, FIONBIO, &v); }
static void hset_timeout(hsock s, int ms) {
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&v, sizeof v);
}
#else
typedef int hsock;
#define BADSOCK (-1)
#define hclose close
static void hnet_init(void) {}
static void hset_nonblock(hsock s, bool on) {
    int f = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, on ? f | O_NONBLOCK : f & ~O_NONBLOCK);
}
static void hset_timeout(hsock s, int ms) {
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
#endif

// ---- SDK structures (bytes)
#define NETINFO_SIZE 0x480
enum {
    NI_LOCAL_COMM_ID = 0x00, NI_SCENE_ID = 0x0A, NI_SESSION_ID = 0x10, NI_BSSID = 0x20, NI_SSID = 0x26,
    NI_CHANNEL = 0x48, NI_LINK_LEVEL = 0x4A, NI_NETWORK_TYPE = 0x4B, NI_SEC_PARAM = 0x50, NI_SEC_MODE = 0x60,
    NI_ACCEPT_POLICY = 0x62, NI_NODE_MAX = 0x66, NI_NODE_COUNT = 0x67, NI_NODES = 0x68, NI_ADV_SIZE = 0x26A,
    NI_ADV = 0x26C, NI_AUTH_ID = 0x478,
};
#define NODE_SIZE 0x40
enum { NODE_IP = 0x00, NODE_MAC = 0x04, NODE_ID = 0x0A, NODE_CONNECTED = 0x0B, NODE_NAME = 0x0C, NODE_LCV = 0x2E };
#define MAX_NODES 8
#define ADV_MAX 0x180
// NetworkConfig (0x20): IntentId 0x10, u16 channel, u8 node count max, u8 -, u16 local communication version
// SecurityConfig (0x44): u16 mode, u16 passphrase size, passphrase[0x40]; UserConfig (0x30): user name[0x21]
// SecurityParameter (0x20): data[0x10], session id[0x10]
// ScanFilter (0x60): NetworkId 0x20, u32 network type, bssid[6], ssid 0x22, -[0x10], u32 flags
enum { SF_LOCAL_COMM_ID = 1, SF_SESSION_ID = 2, SF_NETWORK_TYPE = 4, SF_BSSID = 8, SF_SSID = 0x10, SF_SCENE_ID = 0x20 };

enum { ST_NONE, ST_INITIALIZED, ST_ACCESS_POINT, ST_AP_CREATED, ST_STATION, ST_STATION_CONNECTED, ST_ERROR };
enum { DR_NONE, DR_BY_USER, DR_BY_SYSTEM, DR_DESTROYED_BY_USER, DR_DESTROYED_BY_SYSTEM, DR_REJECTED, DR_SIGNAL_LOST };
#define LDN_RESULT(d) MAKE_RESULT(203, d)
#define R_INVALID_STATE LDN_RESULT(32)
#define R_CONNECT_FAILURE LDN_RESULT(64)
#define R_CONNECT_NOT_FOUND LDN_RESULT(65)
#define R_CONNECT_TIMEOUT LDN_RESULT(66)
#define R_CONNECT_REJECTED LDN_RESULT(67)
#define R_VERSION_TOO_LOW LDN_RESULT(113)
#define R_VERSION_TOO_HIGH LDN_RESULT(114)
#define R_TOO_MANY_PLAYERS LDN_RESULT(144)

// ---- link protocol
#define PROTO_MAGIC "MHGL"
#define PROTO_VERSION 2
enum { F_HELLO = 1, F_INFO, F_WELCOME, F_REFUSE, F_PACKET, F_DESTROY, F_AUX };  // F_AUX: the port's own messages (emsync.c, hle/hpscale.c)
typedef struct {
    char magic[4];
    uint16_t version;
    uint8_t scan_only, pad;
    uint16_t lcv;
    uint8_t mac[6];
    char name[0x21];
    uint8_t pad2[3];
} Hello;
#define MAX_FRAME (0x10000 + 64)

// The game's packets go by UDP when it gets through (Pia, the game's network library, expects datagrams: over TCP
// one lost packet would hold up everything behind it): datagrams "MHGD" + token + kind [+ VPacket + data] between
// a station and the host's port. A station says hello every 2 s (keeping NAT mappings open), the host answers;
// without UDP (blocked, not forwarded) or after 15 s without any, the packets go through the TCP link instead.
#define UDP_MAGIC "MHGD"
enum { U_DATA = 0, U_HELLO, U_HELLO_ACK };
typedef struct { char magic[4]; uint32_t token; uint8_t kind, pad[3]; } UHdr;
#define UDP_MAX_PAYLOAD 8192
#define UDP_SILENCE_NS 15000000000ull

typedef struct {
    hsock s;
    pthread_mutex_t wmu;  // one writer at a time; s is closed only with it held (alive cleared first)
    pthread_t th;
    bool alive, has_thread;
    uint32_t ip;
    uint32_t token;               // UDP: identifies the station's datagrams
    struct sockaddr_in uaddr;     // host side: where the station's datagrams come from
    volatile bool udp_up;
    volatile uint64_t udp_rx_ns;  // last datagram from the other end
} Link;

static struct {
    pthread_mutex_t mu;
    int state, reason;
    uint8_t info[NETINFO_SIZE];
    uint32_t own_ip, subnet;
    uint8_t mac[6];
    uint8_t adv[ADV_MAX];
    uint32_t adv_size;
    int accept_policy;
    uint16_t lcv;
    KEvent *ev;
    uint32_t ev_r;
    // host
    hsock listen_s, disc_s;
    pthread_t host_th;
    bool hosting;
    Link nodes[MAX_NODES];  // [0] unused (the host itself)
    // station
    Link up;
    hsock us;  // UDP socket connected to the host
    pthread_t uth;
    volatile bool uth_run;
} L = {.mu = PTHREAD_MUTEX_INITIALIZER, .listen_s = BADSOCK, .disc_s = BADSOCK, .us = BADSOCK};

// Scan results: where each found network's host is.
static struct { uint8_t bssid[6], session[16]; struct sockaddr_in addr; } found[16];
static int nfound;

static void signal_change(void) {
    if (!L.ev) return;
    pthread_mutex_lock(&g_klock);
    kernel_signal(L.ev);
    pthread_mutex_unlock(&g_klock);
}

static void rand_bytes(void *p, size_t n) {
    for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)arc4random();
}
static const char *ipstr(uint32_t ip) {
    static __thread char b[2][20];
    static __thread int k;
    char *s = b[k++ & 1];
    snprintf(s, 20, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return s;
}
static uint8_t *node_at(uint8_t *info, int i) { return info + NI_NODES + i * NODE_SIZE; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

// ---- frames
static bool send_all(hsock s, const void *b, size_t n) {
    for (const char *p = b; n;) {
        int k = send(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool recv_all(hsock s, void *b, size_t n) {
    for (char *p = b; n;) {
        int k = recv(s, p, (int)n, 0);
        if (k <= 0) return false;
        p += k, n -= (size_t)k;
    }
    return true;
}
static bool frame_send(hsock s, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    uint8_t h[5];
    uint32_t size = 1 + alen + blen;
    memcpy(h, &size, 4);
    h[4] = type;
    return send_all(s, h, 5) && (!alen || send_all(s, a, alen)) && (!blen || send_all(s, b, blen));
}
static bool link_send(Link *l, uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    pthread_mutex_lock(&l->wmu);
    bool ok = l->alive && frame_send(l->s, type, a, alen, b, blen);
    pthread_mutex_unlock(&l->wmu);
    return ok;
}
// -1: closed / error; else the payload size (buf holds MAX_FRAME bytes)
static int frame_recv(hsock s, uint8_t *type, uint8_t *buf) {
    uint32_t size;
    if (!recv_all(s, &size, 4) || size < 1 || size > MAX_FRAME + 1 || !recv_all(s, type, 1)) return -1;
    return recv_all(s, buf, size - 1) ? (int)size - 1 : -1;
}
static void link_close(Link *l) {  // (caller: not holding L.mu if l has a thread to join)
    pthread_mutex_lock(&l->wmu);
    if (l->alive) {
        l->alive = false;
        shutdown(l->s, 2);
        hclose(l->s);
    }
    pthread_mutex_unlock(&l->wmu);
}

static hsock connect_to(const struct sockaddr_in *a, int timeout_ms) {
    hsock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == BADSOCK) return BADSOCK;
    hset_nonblock(s, true);
    connect(s, (const struct sockaddr *)a, sizeof *a);
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int err = 1;
    socklen_t el = sizeof err;
    if (select((int)s + 1, NULL, &w, &e, &tv) <= 0 || !FD_ISSET(s, &w) ||
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) || err) {
        hclose(s);
        return BADSOCK;
    }
    hset_nonblock(s, false);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return s;
}

// One datagram: header [+ packet]. to = NULL: s is connected (a station's socket).
static bool udp_send(hsock s, const struct sockaddr_in *to, uint32_t token, uint8_t kind, const VPacket *p,
                     const void *data, uint32_t len) {
    uint8_t buf[sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
    if (len > UDP_MAX_PAYLOAD) return false;
    UHdr h = {{0}, token, kind, {0}};
    memcpy(h.magic, UDP_MAGIC, 4);
    memcpy(buf, &h, sizeof h);
    uint32_t n = sizeof h;
    if (p) {
        memcpy(buf + n, p, sizeof *p);
        n += sizeof *p;
        if (len) memcpy(buf + n, data, len);
        n += len;
    }
    int k = to ? sendto(s, (const char *)buf, (int)n, 0, (const struct sockaddr *)to, sizeof *to)
               : send(s, (const char *)buf, (int)n, 0);
    return k == (int)n;
}

static Hello make_hello(bool scan_only, const uint8_t *user_name) {
    Hello h = {0};
    memcpy(h.magic, PROTO_MAGIC, 4);
    h.version = PROTO_VERSION;
    h.scan_only = scan_only;
    h.lcv = L.lcv;
    memcpy(h.mac, L.mac, 6);
    if (user_name) memcpy(h.name, user_name, 0x20);
    return h;
}

// ---- the virtual network's packets (vsock.c -> here)
uint32_t ldn_own_ip(void) { return L.own_ip; }
uint32_t ldn_broadcast_ip(void) { return L.subnet ? L.subnet | 0xFF : 0xFFFFFFFFu; }
bool ldn_net_up(void) { return L.state == ST_AP_CREATED || L.state == ST_STATION_CONNECTED; }
bool ldn_hosting(void) { return L.state == ST_AP_CREATED; }

static bool udp_usable(const Link *l) { return l->udp_up && plat_now_ns() - l->udp_rx_ns < UDP_SILENCE_NS; }

// Host: deliver to the host's own sockets and/or forward to the stations (from: the sending node, 0 = the host).
static void host_route(int from, const VPacket *p, const void *data, uint32_t len) {
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);
    for (int i = 1; i < MAX_NODES; i++) {
        Link *l = &L.nodes[i];
        if (i == from || !l->alive || !(all || l->ip == p->dst_ip)) continue;
        hsock ds = L.disc_s;
        if (!(ds != BADSOCK && udp_usable(l) && udp_send(ds, &l->uaddr, l->token, U_DATA, p, data, len)))
            link_send(l, F_PACKET, p, sizeof *p, data, len);
    }
}

void ldn_send(const VPacket *p, const void *data, uint32_t len) {
    if (L.state == ST_AP_CREATED) { host_route(0, p, data, len); return; }
    if (L.state != ST_STATION_CONNECTED) return;
    bool all = p->dst_ip == ldn_broadcast_ip() || p->dst_ip == 0xFFFFFFFFu;
    if (all || p->dst_ip == L.own_ip) vsock_deliver(p, data, len);  // (a broadcast reaches the sender too)
    if (p->dst_ip == L.own_ip) return;
    hsock us = L.us;
    if (!(us != BADSOCK && udp_usable(&L.up) && udp_send(us, NULL, L.up.token, U_DATA, p, data, len)))
        link_send(&L.up, F_PACKET, p, sizeof *p, data, len);
}

// The port's own messages to every other game in the network (reliable, in order: the TCP link).
void ldn_send_aux(const void *data, uint32_t len) {
    if (L.state == ST_AP_CREATED) {
        for (int i = 1; i < MAX_NODES; i++)
            if (L.nodes[i].alive) link_send(&L.nodes[i], F_AUX, data, len, NULL, 0);
    } else if (L.state == ST_STATION_CONNECTED) {
        link_send(&L.up, F_AUX, data, len, NULL, 0);
    }
}

// ---- MHGU_LDN_LAG=<ms>: received packets wait that long before they go on (testing high ping: set it on both
// ends, the round trip grows by twice the value). Kept in arrival order by one delivery thread.
typedef struct Delayed { struct Delayed *next; uint64_t due; int from; VPacket p; uint32_t len; uint8_t data[]; } Delayed;
static Delayed *lag_head, *lag_tail;
static pthread_mutex_t lag_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lag_cv = PTHREAD_COND_INITIALIZER;

static void *lag_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lag_mu);
    for (;;) {
        while (!lag_head) pthread_cond_wait(&lag_cv, &lag_mu);
        Delayed *d = lag_head;
        uint64_t now = plat_now_ns();
        if (now < d->due) {
            pthread_mutex_unlock(&lag_mu);
            plat_sleep_until_ns(d->due);
            pthread_mutex_lock(&lag_mu);
            continue;
        }
        lag_head = d->next;
        if (!lag_head) lag_tail = NULL;
        pthread_mutex_unlock(&lag_mu);
        if (d->from > 0) host_route(d->from, &d->p, d->data, d->len);
        else vsock_deliver(&d->p, d->data, d->len);
        free(d);
        pthread_mutex_lock(&lag_mu);
    }
    return NULL;
}

// A packet from the link: from = the sending station (host side) or 0 (a station: from the host).
// MHGU_LDN_LOSS=<percent>: that share of the received game packets is dropped (testing a lossy connection).
static void link_received(int from, const VPacket *p, const void *data, uint32_t len) {
    static int lag_ms = -1, loss_pct;
    if (lag_ms < 0) {
        const char *e = getenv("MHGU_LDN_LAG");
        lag_ms = e ? atoi(e) : 0;
        if (lag_ms > 0) {
            pthread_t t;
            pthread_create(&t, NULL, lag_thread, NULL);
            LOG("[ldn] MHGU_LDN_LAG: received packets are held back %d ms\n", lag_ms);
        }
        if ((e = getenv("MHGU_LDN_LOSS")) && atoi(e) > 0) {
            loss_pct = atoi(e);
            LOG("[ldn] MHGU_LDN_LOSS: dropping %d%% of the received packets\n", loss_pct);
        }
    }
    if (loss_pct && (int)(arc4random() % 100) < loss_pct) return;
    if (lag_ms <= 0) {
        if (from > 0) host_route(from, p, data, len);
        else vsock_deliver(p, data, len);
        return;
    }
    Delayed *d = malloc(sizeof *d + len);
    if (!d) return;
    d->next = NULL, d->due = plat_now_ns() + (uint64_t)lag_ms * 1000000ull, d->from = from, d->p = *p, d->len = len;
    memcpy(d->data, data, len);
    pthread_mutex_lock(&lag_mu);
    if (lag_tail) lag_tail->next = d; else lag_head = d;
    lag_tail = d;
    pthread_cond_signal(&lag_cv);
    pthread_mutex_unlock(&lag_mu);
}

// ---- host
static void push_info_locked(int except) {  // to every station: the network changed
    for (int i = 1; i < MAX_NODES; i++)
        if (i != except && L.nodes[i].alive) link_send(&L.nodes[i], F_INFO, L.info, NETINFO_SIZE, NULL, 0);
}

typedef struct { hsock s; struct sockaddr_in from; } Accepted;

static void *station_thread(void *arg) {
    Accepted acc = *(Accepted *)arg;
    free(arg);
    static __thread uint8_t buf[MAX_FRAME];
    uint8_t type;
    hset_timeout(acc.s, 5000);
    int n = frame_recv(acc.s, &type, buf);
    Hello h;
    if (n < (int)sizeof h || type != F_HELLO) { hclose(acc.s); return NULL; }
    memcpy(&h, buf, sizeof h);
    if (memcmp(h.magic, PROTO_MAGIC, 4) || h.version != PROTO_VERSION) {
        uint32_t r = R_CONNECT_REJECTED;
        frame_send(acc.s, F_REFUSE, &r, 4, NULL, 0);
        hclose(acc.s);
        return NULL;
    }
    pthread_mutex_lock(&L.mu);
    if (h.scan_only || L.state != ST_AP_CREATED) {
        uint8_t info[NETINFO_SIZE];
        memcpy(info, L.info, sizeof info);
        bool up = L.state == ST_AP_CREATED;
        pthread_mutex_unlock(&L.mu);
        if (up) frame_send(acc.s, F_INFO, info, sizeof info, NULL, 0);
        hclose(acc.s);
        return NULL;
    }
    uint32_t refuse = 0;
    int slot = 0;
    if (h.lcv < L.lcv) refuse = R_VERSION_TOO_LOW;
    else if (h.lcv > L.lcv) refuse = R_VERSION_TOO_HIGH;
    else if (L.accept_policy == 1) refuse = R_CONNECT_REJECTED;  // RejectAll
    else if (L.info[NI_NODE_COUNT] >= L.info[NI_NODE_MAX]) refuse = R_TOO_MANY_PLAYERS;
    else {
        for (slot = 1; slot < MAX_NODES && (L.nodes[slot].alive || L.nodes[slot].has_thread); slot++) {}
        if (slot >= MAX_NODES || slot >= L.info[NI_NODE_MAX]) refuse = R_TOO_MANY_PLAYERS;
    }
    if (refuse) {
        pthread_mutex_unlock(&L.mu);
        frame_send(acc.s, F_REFUSE, &refuse, 4, NULL, 0);
        hclose(acc.s);
        LOG("[ldn] refused a player from %s (result %#x)\n", inet_ntoa(acc.from.sin_addr), refuse);
        return NULL;
    }
    Link *l = &L.nodes[slot];
    l->s = acc.s, l->ip = L.subnet | (uint32_t)(slot + 1), l->alive = true, l->has_thread = true;
    l->th = pthread_self();
    l->token = arc4random() | 1, l->udp_up = false, l->udp_rx_ns = 0;
    uint8_t *nd = node_at(L.info, slot);
    memset(nd, 0, NODE_SIZE);
    wr32(nd + NODE_IP, l->ip);
    memcpy(nd + NODE_MAC, h.mac, 6);
    nd[NODE_ID] = (uint8_t)slot;
    nd[NODE_CONNECTED] = 1;
    memcpy(nd + NODE_NAME, h.name, 0x20);
    wr16(nd + NODE_LCV, h.lcv);
    L.info[NI_NODE_COUNT]++;
    uint8_t welcome[8 + NETINFO_SIZE] = {(uint8_t)slot};  // node, -, -, -, u32 UDP token, NetworkInfo
    wr32(welcome + 4, l->token);
    memcpy(welcome + 8, L.info, NETINFO_SIZE);
    link_send(l, F_WELCOME, welcome, sizeof welcome, NULL, 0);
    push_info_locked(slot);
    pthread_mutex_unlock(&L.mu);
    hset_timeout(acc.s, 0);
    LOG("[ldn] %.32s joined from %s as %s (node %d)\n", h.name, inet_ntoa(acc.from.sin_addr), ipstr(l->ip), slot);
    signal_change();

    while ((n = frame_recv(acc.s, &type, buf)) >= 0) {
        if (type == F_PACKET && n >= (int)sizeof(VPacket)) {
            VPacket p;
            memcpy(&p, buf, sizeof p);
            p.src_ip = l->ip;  // (a station speaks for itself only)
            link_received(slot, &p, buf + sizeof p, (uint32_t)n - sizeof p);
        } else if (type == F_AUX && n > 0) {  // to the host's own game and on to the other stations
            emsync_aux_received(buf, (uint32_t)n);
            hpscale_aux_received(buf, (uint32_t)n);
            for (int i = 1; i < MAX_NODES; i++)
                if (i != slot && L.nodes[i].alive) link_send(&L.nodes[i], F_AUX, buf, (uint32_t)n, NULL, 0);
        }
    }

    pthread_mutex_lock(&L.mu);
    bool was = l->alive;
    link_close(l);
    if (was && L.state == ST_AP_CREATED) {
        memset(node_at(L.info, slot), 0, NODE_SIZE);
        if (L.info[NI_NODE_COUNT] > 1) L.info[NI_NODE_COUNT]--;
        push_info_locked(0);
    }
    l->has_thread = false;
    pthread_mutex_unlock(&L.mu);
    LOG("[ldn] node %d (%s) left\n", slot, ipstr(l->ip));
    vsock_peer_gone(l->ip);
    signal_change();
    return NULL;
}

// Accepts stations and answers LAN discovery probes until the listening sockets close.
static void *host_thread(void *arg) {
    (void)arg;
    for (;;) {
        hsock ls = L.listen_s, ds = L.disc_s;
        if (ls == BADSOCK) break;
        fd_set r;
        FD_ZERO(&r);
        FD_SET(ls, &r);
        if (ds != BADSOCK) FD_SET(ds, &r);
        struct timeval tv = {0, 200000};
        int k = select((int)(ls > ds || ds == BADSOCK ? ls : ds) + 1, &r, NULL, NULL, &tv);
        if (!L.hosting) break;
        if (k <= 0) continue;
        if (FD_ISSET(ls, &r)) {
            Accepted *a = malloc(sizeof *a);
            socklen_t al = sizeof a->from;
            a->s = accept(ls, (struct sockaddr *)&a->from, &al);
            if (a->s == BADSOCK) { free(a); continue; }
            int one = 1;
            setsockopt(a->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
            pthread_t t;
            if (pthread_create(&t, NULL, station_thread, a)) { hclose(a->s); free(a); }
            else pthread_detach(t);
        }
        if (ds != BADSOCK && FD_ISSET(ds, &r)) {
            static uint8_t probe[sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(ds, (char *)probe, sizeof probe, 0, (struct sockaddr *)&from, &fl);
            if (n >= (int)sizeof(UHdr) && !memcmp(probe, UDP_MAGIC, 4)) {  // a station's datagram
                UHdr h;
                memcpy(&h, probe, sizeof h);
                int slot = 1;
                while (slot < MAX_NODES && !(L.nodes[slot].alive && L.nodes[slot].token == h.token)) slot++;
                if (slot == MAX_NODES) continue;
                Link *l = &L.nodes[slot];
                l->uaddr = from;  // (NAT may change it: the latest wins)
                l->udp_rx_ns = plat_now_ns();
                if (h.kind == U_HELLO) {
                    if (!l->udp_up) LOG("[ldn] node %d: game packets go by UDP\n", slot);
                    l->udp_up = true;
                    udp_send(ds, &from, h.token, U_HELLO_ACK, NULL, NULL, 0);
                } else if (h.kind == U_DATA && n >= (int)(sizeof h + sizeof(VPacket))) {
                    VPacket p;
                    memcpy(&p, probe + sizeof h, sizeof p);
                    p.src_ip = l->ip;
                    link_received(slot, &p, probe + sizeof h + sizeof p, (uint32_t)n - sizeof h - sizeof p);
                }
                continue;
            }
            if (n >= 6 && !memcmp(probe, PROTO_MAGIC, 4) && rd16(probe + 4) == PROTO_VERSION) {
                uint8_t reply[8 + NETINFO_SIZE];
                memcpy(reply, PROTO_MAGIC, 4);
                wr16(reply + 4, PROTO_VERSION);
                wr16(reply + 6, (uint16_t)g_config.ldn_port);
                pthread_mutex_lock(&L.mu);
                memcpy(reply + 8, L.info, NETINFO_SIZE);
                pthread_mutex_unlock(&L.mu);
                sendto(ds, (const char *)reply, sizeof reply, 0, (struct sockaddr *)&from, fl);
            }
        }
    }
    return NULL;
}

static bool host_start(void) {
    hnet_init();
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)g_config.ldn_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    hsock ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == BADSOCK || bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8)) {
        LOG("[ldn] can't listen on TCP port %d (in use by another program?): nobody can join this hub\n", g_config.ldn_port);
        if (ls != BADSOCK) hclose(ls);
        return false;
    }
    hsock ds = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ds != BADSOCK && bind(ds, (struct sockaddr *)&a, sizeof a)) {
        LOG("[ldn] UDP port %d is in use: no LAN search (only Join addresses), game packets go by TCP\n", g_config.ldn_port);
        hclose(ds);
        ds = BADSOCK;
    }
    L.listen_s = ls, L.disc_s = ds, L.hosting = true;
    pthread_create(&L.host_th, NULL, host_thread, NULL);
    LOG("[ldn] hosting: players join this computer's address, port %d (TCP, and UDP for LAN search and game packets)\n",
        g_config.ldn_port);
    return true;
}

static void host_stop(uint32_t reason) {  // (caller: not holding L.mu)
    if (L.hosting) {
        L.hosting = false;
        pthread_join(L.host_th, NULL);
        hclose(L.listen_s);
        if (L.disc_s != BADSOCK) hclose(L.disc_s);
        L.listen_s = L.disc_s = BADSOCK;
    }
    for (int i = 1; i < MAX_NODES; i++) {
        Link *l = &L.nodes[i];
        if (l->alive) link_send(l, F_DESTROY, &reason, 4, NULL, 0);
        link_close(l);
    }
    for (int i = 1; i < MAX_NODES; i++)  // (their threads see the closed sockets and end)
        for (int t = 0; t < 200 && L.nodes[i].has_thread; t++) plat_sleep_until_ns(plat_now_ns() + 5000000);
}

// ---- station
static void *uplink_thread(void *arg) {
    (void)arg;
    static __thread uint8_t buf[MAX_FRAME];
    uint8_t type;
    int n, reason = DR_SIGNAL_LOST;
    while ((n = frame_recv(L.up.s, &type, buf)) >= 0) {
        if (type == F_PACKET && n >= (int)sizeof(VPacket)) {
            VPacket p;
            memcpy(&p, buf, sizeof p);
            link_received(0, &p, buf + sizeof p, (uint32_t)n - sizeof p);
        } else if (type == F_INFO && n >= NETINFO_SIZE) {
            pthread_mutex_lock(&L.mu);
            memcpy(L.info, buf, NETINFO_SIZE);
            pthread_mutex_unlock(&L.mu);
            signal_change();
        } else if (type == F_DESTROY && n >= 4) {
            reason = (int)rd32(buf);
        } else if (type == F_AUX && n > 0) {
            emsync_aux_received(buf, (uint32_t)n);
            hpscale_aux_received(buf, (uint32_t)n);
        }
    }
    L.uth_run = false;
    pthread_mutex_lock(&L.mu);
    bool lost = L.up.alive;  // (else: we disconnected ourselves)
    link_close(&L.up);
    if (lost && L.state == ST_STATION_CONNECTED) {
        L.state = ST_ERROR, L.reason = reason;
        LOG("[ldn] connection to the host ended (reason %d)\n", reason);
    }
    pthread_mutex_unlock(&L.mu);
    if (lost) { vsock_network_down(); signal_change(); }
    return NULL;
}

// The station's datagrams: hello (every 2 s once answered, else every 0.5 s) and the host's packets.
static void *station_udp_thread(void *arg) {
    (void)arg;
    static __thread uint8_t buf[sizeof(UHdr) + sizeof(VPacket) + UDP_MAX_PAYLOAD];
    hsock us = L.us;
    uint64_t next_hello = 0;
    hset_timeout(us, 250);
    while (L.uth_run) {
        uint64_t now = plat_now_ns();
        if (now >= next_hello) {
            udp_send(us, NULL, L.up.token, U_HELLO, NULL, NULL, 0);
            next_hello = now + (L.up.udp_up ? 2000000000ull : 500000000ull);
        }
        int n = recv(us, (char *)buf, sizeof buf, 0);
        if (n < 0) { plat_sleep_until_ns(plat_now_ns() + 20000000); continue; }  // (timeout, or ICMP unreachable)
        UHdr h;
        if (n < (int)sizeof h || memcmp(buf, UDP_MAGIC, 4)) continue;
        memcpy(&h, buf, sizeof h);
        if (h.token != L.up.token) continue;
        L.up.udp_rx_ns = plat_now_ns();
        if (h.kind == U_HELLO_ACK && !L.up.udp_up) {
            L.up.udp_up = true;
            LOG("[ldn] UDP to the host works: game packets go by UDP\n");
        } else if (h.kind == U_DATA && n >= (int)(sizeof h + sizeof(VPacket))) {
            VPacket p;
            memcpy(&p, buf + sizeof h, sizeof p);
            link_received(0, &p, buf + sizeof h + sizeof p, (uint32_t)n - sizeof h - sizeof p);
        }
        if (L.up.udp_up && plat_now_ns() - L.up.udp_rx_ns > UDP_SILENCE_NS) {
            L.up.udp_up = false;
            LOG("[ldn] nothing by UDP from the host for 15 s: game packets go by TCP\n");
        }
    }
    return NULL;
}

static void station_leave(void) {  // (caller: not holding L.mu)
    if (L.up.has_thread) {
        link_close(&L.up);
        pthread_join(L.up.th, NULL);
        L.up.has_thread = false;
    }
    if (L.us != BADSOCK) {
        L.uth_run = false;
        pthread_join(L.uth, NULL);
        hclose(L.us);
        L.us = BADSOCK;
    }
    L.up.udp_up = false;
    vsock_network_down();
}

// Hosts to ask: ini Join (comma separated host[:port]) and this computer.
static int scan_addresses(struct sockaddr_in *out, int max) {
    int n = 0;
    char list[sizeof g_config.ldn_join];
    snprintf(list, sizeof list, "%s", g_config.ldn_join);
    for (char *t = strtok(list, ", ;"); t && n < max; t = strtok(NULL, ", ;")) {
        char host[200];
        int port = g_config.ldn_port;
        snprintf(host, sizeof host, "%s", t);
        char *colon = strrchr(host, ':');
        if (colon) { *colon = 0; port = atoi(colon + 1); }
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) || !res) { LOG("[ldn] Join address \"%s\" not found\n", t); continue; }
        out[n] = *(struct sockaddr_in *)res->ai_addr;
        out[n++].sin_port = htons((uint16_t)port);
        freeaddrinfo(res);
    }
    return n;
}

// Collects the networks of hosts that answer: by address (TCP) and by LAN broadcast (UDP).
static int discover(uint8_t (*infos)[NETINFO_SIZE], struct sockaddr_in *addrs, int max) {
    hnet_init();
    int n = 0;
    static __thread uint8_t buf[MAX_FRAME];
    struct sockaddr_in targets[8];
    int nt = scan_addresses(targets, 8);
    Hello h = make_hello(true, NULL);
    for (int i = 0; i < nt && n < max; i++) {
        hsock s = connect_to(&targets[i], 1500);
        if (s == BADSOCK) continue;
        hset_timeout(s, 1500);
        uint8_t type;
        int k;
        if (frame_send(s, F_HELLO, &h, sizeof h, NULL, 0) && (k = frame_recv(s, &type, buf)) >= NETINFO_SIZE && type == F_INFO) {
            memcpy(infos[n], buf, NETINFO_SIZE);
            addrs[n++] = targets[i];
        }
        hclose(s);
    }
    hsock u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == BADSOCK) return n;
    int one = 1;
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof one);
    uint8_t probe[8];
    memcpy(probe, PROTO_MAGIC, 4);
    wr16(probe + 4, PROTO_VERSION);
    wr16(probe + 6, 0);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)g_config.ldn_port);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(u, (const char *)probe, sizeof probe, 0, (struct sockaddr *)&to, sizeof to);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(u, (const char *)probe, sizeof probe, 0, (struct sockaddr *)&to, sizeof to);
    uint64_t end = plat_now_ns() + 400000000ull;
    for (uint64_t now; (now = plat_now_ns()) < end && n < max;) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(u, &r);
        uint64_t left = (end - now) / 1000;
        struct timeval tv = {(long)(left / 1000000), (long)(left % 1000000)};
        if (select((int)u + 1, &r, NULL, NULL, &tv) <= 0) break;
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        int k = recvfrom(u, (char *)buf, MAX_FRAME, 0, (struct sockaddr *)&from, &fl);
        if (k < 8 + NETINFO_SIZE || memcmp(buf, PROTO_MAGIC, 4) || rd16(buf + 4) != PROTO_VERSION) continue;
        bool dup = false;
        for (int i = 0; i < n; i++) dup |= !memcmp(infos[i] + NI_BSSID, buf + 8 + NI_BSSID, 6);
        if (dup) continue;
        memcpy(infos[n], buf + 8, NETINFO_SIZE);
        from.sin_port = htons(rd16(buf + 6));
        addrs[n++] = from;
    }
    hclose(u);
    return n;
}

// ---- nn::ldn
#define ONCE(...) do { static int once_; if (!once_++) LOG(__VA_ARGS__); } while (0)

HLE_FUNC("_ZN2nn3ldn10InitializeEv", ldn_initialize) { NTRACE();
    pthread_mutex_lock(&L.mu);
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    if (L.state == ST_NONE) {
        L.state = ST_INITIALIZED, L.reason = DR_NONE;
        rand_bytes(L.mac, 6);
        L.mac[0] = (uint8_t)((L.mac[0] & 0xFC) | 0x02);  // locally administered, unicast
    }
    pthread_mutex_unlock(&L.mu);
    ONCE("[ldn] local play over IP: host port %d, join %s (state event %x)\n", g_config.ldn_port,
         g_config.ldn_join[0] ? g_config.ldn_join : "(LAN search only)", L.ev_r);
    RET(0);
}

static void to_initialized(void) {
    int st = L.state;
    if (st == ST_AP_CREATED || st == ST_ACCESS_POINT) host_stop(DR_DESTROYED_BY_USER);
    if (st == ST_STATION_CONNECTED || st == ST_STATION || st == ST_ERROR) station_leave();
    if (st == ST_AP_CREATED) vsock_network_down();
    L.own_ip = L.subnet = 0;
}

HLE_FUNC("_ZN2nn3ldn8FinalizeEv", ldn_finalize) { NTRACE();
    to_initialized();
    L.state = ST_NONE;
    (void)c;
}
HLE_FUNC("_ZN2nn3ldn8GetStateEv", ldn_get_state) { NTRACE(); RET(L.state); }
HLE_FUNC("_ZN2nn3ldn19GetDisconnectReasonEv", ldn_get_reason) { NTRACE(); RET(L.reason); }

// AttachStateChangeEvent(SystemEventType *): the event fires on every change of the state or the network
HLE_FUNC("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach_event) { NTRACE();
    if (!L.ev) L.ev = kernel_create_event_pair(&(uint32_t){0}, &L.ev_r);
    c->r[1] = L.ev_r;
    c->r[2] = 0;  // not managed: the handle stays ours
    c->r[3] = 1;  // auto clear
    guest_call(c, guest_sym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE"));
}

HLE_FUNC("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_get_network_info) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    pthread_mutex_lock(&L.mu);
    memcpy(ARGP(0), L.info, NETINFO_SIZE);
    pthread_mutex_unlock(&L.mu);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_get_ipv4) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    if (ARG(0)) ST32(ARG(0), L.own_ip);
    if (ARG(1)) ST32(ARG(1), 0xFFFFFF00u);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ldn_get_sec_param) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    memcpy(ARGP(0), L.info + NI_SEC_PARAM, 0x10);
    memcpy((uint8_t *)ARGP(0) + 0x10, L.info + NI_SESSION_ID, 0x10);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn16GetNetworkConfigEPNS0_13NetworkConfigE", ldn_get_net_config) { NTRACE();
    if (!ldn_net_up()) { RET(R_INVALID_STATE); return; }
    uint8_t *o = ARGP(0);
    memset(o, 0, 0x20);
    memcpy(o, L.info, 0x10);  // IntentId
    wr16(o + 0x10, rd16(L.info + NI_CHANNEL));
    o[0x12] = L.info[NI_NODE_MAX];
    wr16(o + 0x14, L.lcv);
    RET(0);
}

// ---- access point (host)
HLE_FUNC("_ZN2nn3ldn15OpenAccessPointEv", ldn_open_ap) { NTRACE();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_ACCESS_POINT;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn16CloseAccessPointEv", ldn_close_ap) { NTRACE();
    if (L.state != ST_ACCESS_POINT && L.state != ST_AP_CREATED) { RET(R_INVALID_STATE); return; }
    to_initialized();
    L.state = ST_INITIALIZED;
    signal_change();
    RET(0);
}

static uint32_t create_network(const uint8_t *cfg, const uint8_t *sec, const uint8_t *sec_param, const uint8_t *user) {
    if (L.state != ST_ACCESS_POINT) return R_INVALID_STATE;
    pthread_mutex_lock(&L.mu);
    memset(L.info, 0, NETINFO_SIZE);
    memcpy(L.info, cfg, 0x10);  // IntentId
    if (sec_param) {
        memcpy(L.info + NI_SEC_PARAM, sec_param, 0x10);
        memcpy(L.info + NI_SESSION_ID, sec_param + 0x10, 0x10);
    } else {
        rand_bytes(L.info + NI_SEC_PARAM, 0x10);
        rand_bytes(L.info + NI_SESSION_ID, 0x10);
    }
    memcpy(L.info + NI_BSSID, L.mac, 6);
    char ssid[33];
    snprintf(ssid, sizeof ssid, "AllGenURecomp%08x%08x", arc4random(), arc4random());
    L.info[NI_SSID] = (uint8_t)strlen(ssid);
    memcpy(L.info + NI_SSID + 1, ssid, strlen(ssid));
    uint16_t ch = rd16(cfg + 0x10);
    wr16(L.info + NI_CHANNEL, ch ? ch : 6);
    L.info[NI_LINK_LEVEL] = 3;
    L.info[NI_NETWORK_TYPE] = 2;  // ldn
    wr16(L.info + NI_SEC_MODE, rd16(sec));
    L.info[NI_ACCEPT_POLICY] = (uint8_t)L.accept_policy;
    uint8_t max = cfg[0x12];
    L.info[NI_NODE_MAX] = max >= 1 && max <= MAX_NODES ? max : MAX_NODES;
    L.info[NI_NODE_COUNT] = 1;
    L.lcv = rd16(cfg + 0x14);
    L.subnet = 0xA9FE0000u | (uint32_t)(1 + arc4random() % 254) << 8;  // 169.254.x.0/24
    L.own_ip = L.subnet | 1;
    uint8_t *nd = node_at(L.info, 0);
    wr32(nd + NODE_IP, L.own_ip);
    memcpy(nd + NODE_MAC, L.mac, 6);
    nd[NODE_ID] = 0;
    nd[NODE_CONNECTED] = 1;
    memcpy(nd + NODE_NAME, user, 0x20);
    wr16(nd + NODE_LCV, L.lcv);
    wr16(L.info + NI_ADV_SIZE, (uint16_t)L.adv_size);
    memcpy(L.info + NI_ADV, L.adv, L.adv_size);
    uint64_t auth = ((uint64_t)arc4random() << 32) | arc4random();
    memcpy(L.info + NI_AUTH_ID, &auth, 8);
    memset(L.nodes, 0, sizeof L.nodes);
    for (int i = 0; i < MAX_NODES; i++) pthread_mutex_init(&L.nodes[i].wmu, NULL);
    pthread_mutex_unlock(&L.mu);
    host_start();
    L.state = ST_AP_CREATED;
    LOG("[ldn] network created: %s, up to %d players, version %u\n", ipstr(L.own_ip), L.info[NI_NODE_MAX], L.lcv);
    signal_change();
    return 0;
}

// CreateNetwork(const NetworkConfig &, const SecurityConfig &, const UserConfig &)
HLE_FUNC("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_create) { NTRACE();
    RET(create_network(ARGP(0), ARGP(1), NULL, ARGP(2)));
}
// CreateNetworkPrivate(config, security, SecurityParameter, user, address entry count, const AddressEntry *)
HLE_FUNC("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
         ldn_create_private) {
    RET(create_network(ARGP(0), ARGP(1), ARGP(2), ARGP(3)));
}

HLE_FUNC("_ZN2nn3ldn14DestroyNetworkEv", ldn_destroy) { NTRACE();
    if (L.state != ST_AP_CREATED) { RET(R_INVALID_STATE); return; }
    host_stop(DR_DESTROYED_BY_USER);
    vsock_network_down();
    L.state = ST_ACCESS_POINT;
    L.own_ip = L.subnet = 0;
    LOG("[ldn] network destroyed\n");
    signal_change();
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn16SetAdvertiseDataEPKvj", ldn_set_advertise) { NTRACE();
    uint32_t n = ARG(1) < ADV_MAX ? ARG(1) : ADV_MAX;
    pthread_mutex_lock(&L.mu);
    memcpy(L.adv, ARGP(0), n);
    L.adv_size = n;
    if (L.state == ST_AP_CREATED) {
        wr16(L.info + NI_ADV_SIZE, (uint16_t)n);
        memset(L.info + NI_ADV, 0, ADV_MAX);
        memcpy(L.info + NI_ADV, L.adv, n);
        push_info_locked(0);
    }
    pthread_mutex_unlock(&L.mu);
    RET(0);
}

HLE_FUNC("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ldn_set_accept) { NTRACE();
    pthread_mutex_lock(&L.mu);
    L.accept_policy = (int)ARG(0);
    if (L.state == ST_AP_CREATED) { L.info[NI_ACCEPT_POLICY] = (uint8_t)ARG(0); push_info_locked(0); }
    pthread_mutex_unlock(&L.mu);
    RET(0);
}

// Reject(Ipv4Address): the host sends a player away
HLE_FUNC("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_reject) { NTRACE();
    uint32_t r = DR_REJECTED;
    for (int i = 1; i < MAX_NODES; i++)
        if (L.nodes[i].alive && L.nodes[i].ip == ARG(0)) {
            link_send(&L.nodes[i], F_DESTROY, &r, 4, NULL, 0);
            link_close(&L.nodes[i]);
        }
    RET(0);
}

// ---- station (joining)
HLE_FUNC("_ZN2nn3ldn11OpenStationEv", ldn_open_station) { NTRACE();
    if (L.state != ST_INITIALIZED) { RET(R_INVALID_STATE); return; }
    L.state = ST_STATION;
    signal_change();
    RET(0);
}
HLE_FUNC("_ZN2nn3ldn12CloseStationEv", ldn_close_station) { NTRACE();
    if (L.state != ST_STATION && L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    to_initialized();
    L.state = ST_INITIALIZED;
    signal_change();
    RET(0);
}

static bool filter_ok(const uint8_t *info, const uint8_t *f) {
    uint32_t flags = rd32(f + 0x5C);
    if ((flags & SF_LOCAL_COMM_ID) && memcmp(info + NI_LOCAL_COMM_ID, f, 8)) return false;
    if ((flags & SF_SCENE_ID) && rd16(info + NI_SCENE_ID) != rd16(f + 0x0A)) return false;
    if ((flags & SF_SESSION_ID) && memcmp(info + NI_SESSION_ID, f + 0x10, 0x10)) return false;
    if ((flags & SF_BSSID) && memcmp(info + NI_BSSID, f + 0x24, 6)) return false;
    return true;
}

// Scan(NetworkInfo *out, int *count, int max, const ScanFilter &, int channel)
HLE_FUNC("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_scan) { NTRACE();
    uint32_t out = ARG(0), pcount = ARG(1);
    int max = (int)ARG(2);
    const uint8_t *filter = ARGP(3);
    if (L.state != ST_STATION && L.state != ST_ACCESS_POINT) { RET(R_INVALID_STATE); return; }
    static uint8_t infos[16][NETINFO_SIZE];
    struct sockaddr_in addrs[16];
    int n = discover(infos, addrs, 16), k = 0;
    nfound = 0;
    for (int i = 0; i < n; i++) {
        if (!filter_ok(infos[i], filter)) continue;
        memcpy(found[nfound].bssid, infos[i] + NI_BSSID, 6);
        memcpy(found[nfound].session, infos[i] + NI_SESSION_ID, 16);
        found[nfound++].addr = addrs[i];
        if (k < max) memcpy((uint8_t *)G2H(out) + (size_t)k++ * NETINFO_SIZE, infos[i], NETINFO_SIZE);
    }
    if (pcount) ST32(pcount, k);
    static int last = -1;
    if (k != last) { LOG("[ldn] scan: %d network(s) found\n", k); last = k; }
    RET(0);
}

static uint32_t connect_network(const uint8_t *info, const uint8_t *user, uint16_t lcv) {
    if (L.state != ST_STATION) return R_INVALID_STATE;
    int i = 0;
    while (i < nfound && memcmp(found[i].bssid, info + NI_BSSID, 6)) i++;
    if (i == nfound) return R_CONNECT_NOT_FOUND;
    hsock s = connect_to(&found[i].addr, 3000);
    if (s == BADSOCK) { LOG("[ldn] can't reach the host at %s\n", inet_ntoa(found[i].addr.sin_addr)); return R_CONNECT_TIMEOUT; }
    L.lcv = lcv;
    Hello h = make_hello(false, user);
    static uint8_t buf[MAX_FRAME];
    uint8_t type;
    int n;
    hset_timeout(s, 5000);
    if (!frame_send(s, F_HELLO, &h, sizeof h, NULL, 0) || (n = frame_recv(s, &type, buf)) < 4) {
        hclose(s);
        return R_CONNECT_TIMEOUT;
    }
    if (type == F_REFUSE) {
        hclose(s);
        LOG("[ldn] the host refused (result %#x)\n", rd32(buf));
        return rd32(buf);
    }
    if (type != F_WELCOME || n < 8 + NETINFO_SIZE) { hclose(s); return R_CONNECT_FAILURE; }
    hset_timeout(s, 0);
    int node = buf[0];
    pthread_mutex_lock(&L.mu);
    memcpy(L.info, buf + 8, NETINFO_SIZE);
    L.own_ip = rd32(node_at(L.info, node) + NODE_IP);
    L.subnet = L.own_ip & 0xFFFFFF00u;
    pthread_mutex_init(&L.up.wmu, NULL);
    L.up.s = s, L.up.alive = true, L.up.ip = L.subnet | 1;
    L.up.token = rd32(buf + 4), L.up.udp_up = false, L.up.udp_rx_ns = 0;
    L.state = ST_STATION_CONNECTED, L.reason = DR_NONE;
    pthread_mutex_unlock(&L.mu);
    L.up.has_thread = pthread_create(&L.up.th, NULL, uplink_thread, NULL) == 0;
    // UDP to the same address and port as the TCP connection (the host's hello answers tell whether it gets through)
    L.us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (L.us != BADSOCK && !connect(L.us, (const struct sockaddr *)&found[i].addr, sizeof found[i].addr)) {
        L.uth_run = true;
        if (pthread_create(&L.uth, NULL, station_udp_thread, NULL)) { hclose(L.us); L.us = BADSOCK; }
    } else if (L.us != BADSOCK) {
        hclose(L.us);
        L.us = BADSOCK;
    }
    LOG("[ldn] joined %s's network as %s (node %d)\n", inet_ntoa(found[i].addr.sin_addr), ipstr(L.own_ip), node);
    signal_change();
    return 0;
}

// Connect(const NetworkInfo &, const SecurityConfig &, const UserConfig &, int version, ConnectOption)
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_connect) { NTRACE();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
HLE_FUNC("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEi", ldn_connect2) { NTRACE();
    RET(connect_network(ARGP(0), ARGP(2), (uint16_t)ARG(3)));
}
// ConnectPrivate(config, security, SecurityParameter, user, int version, ConnectOption): a network known by its
// session id (from the scan list)
static uint32_t connect_private(Cpu *c) {
    const uint8_t *session = (const uint8_t *)ARGP(2) + 0x10;
    for (int i = 0; i < nfound; i++)
        if (!memcmp(found[i].session, session, 0x10)) {
            uint8_t info[NETINFO_SIZE] = {0};
            memcpy(info + NI_BSSID, found[i].bssid, 6);
            return connect_network(info, ARGP(3), (uint16_t)STK(0));
        }
    LOG("[ldn] ConnectPrivate: no scanned network has that session\n");
    return R_CONNECT_NOT_FOUND;
}
HLE_FUNC("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
         ldn_connect_private) {
    RET(connect_private(c));
}

HLE_FUNC("_ZN2nn3ldn10DisconnectEv", ldn_disconnect) { NTRACE();
    if (L.state != ST_STATION_CONNECTED && L.state != ST_ERROR) { RET(R_INVALID_STATE); return; }
    station_leave();
    L.state = ST_STATION, L.reason = DR_BY_USER;
    L.own_ip = L.subnet = 0;
    signal_change();
    RET(0);
}
