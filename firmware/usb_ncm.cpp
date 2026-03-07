/*
 * usb_ncm.cpp — USB CDC-NCM ethernet + lwIP + minimal DHCP server
 *
 * Board = USB CDC-NCM ethernet adapter (192.168.7.2)
 * Host  = 192.168.7.1 (assigned by our DHCP server)
 * Relay runs on host at 192.168.7.1:7799
 *
 * Requires ESP32 Arduino core 3.x (CFG_TUD_NCM=1 compiled in)
 */

#include "usb_ncm.h"
#include <Arduino.h>
#include <string.h>

// Core TinyUSB interface registration
#include "esp32-hal-tinyusb.h"  // tinyusb_interface_t, tinyusb_enable_interface

// TinyUSB
extern "C" {
#include "tusb.h"
#include "class/net/net_device.h"
}

// lwIP
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

// ── Addresses ─────────────────────────────────────────────────────
static const uint8_t BOARD_MAC[6] = {0x02,0xDE,0xAD,0xBE,0xEF,0x01};

// ── lwIP state ────────────────────────────────────────────────────
static struct netif s_netif;
static bool s_lwip_ready    = false;
static bool s_ncm_connected = false;

// ── TX ring (one frame) ───────────────────────────────────────────
static uint8_t  s_tx_buf[1600];
static uint16_t s_tx_len     = 0;
static bool     s_tx_pending = false;

// ── TinyUSB NCM callbacks (weak stubs override in core) ──────────
extern "C" {

void tud_network_init_cb(void) {}

bool tud_network_recv_cb(const uint8_t *src, uint16_t size) {
    if (s_lwip_ready && size > 14 && size <= 1514) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
        if (p) {
            pbuf_take(p, src, size);
            if (s_netif.input(p, &s_netif) != ERR_OK) pbuf_free(p);
        }
    }
    tud_network_recv_renew();
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg) {
    (void)ref; (void)arg;
    if (!s_tx_pending || s_tx_len == 0) return 0;
    uint16_t len = s_tx_len;
    memcpy(dst, s_tx_buf, len);
    s_tx_pending = false;
    s_tx_len = 0;
    return len;
}

} // extern "C"

// ── lwIP netif ────────────────────────────────────────────────────
static err_t ncm_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    if (!s_ncm_connected) return ERR_IF;
    uint16_t total = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        if (total + q->len > (uint16_t)sizeof(s_tx_buf)) return ERR_BUF;
        memcpy(s_tx_buf + total, q->payload, q->len);
        total += q->len;
    }
    if (!tud_network_can_xmit(total)) return ERR_BUF;
    s_tx_len = total;
    s_tx_pending = true;
    tud_network_xmit(NULL, 0);
    return ERR_OK;
}

static err_t ncm_netif_init(struct netif *netif) {
    netif->linkoutput = ncm_output;
    netif->output     = etharp_output;
    netif->mtu        = 1500;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(netif->hwaddr, BOARD_MAC, ETH_HWADDR_LEN);
    netif->name[0] = 'u'; netif->name[1] = 'n';
    return ERR_OK;
}

// ── Minimal DHCP server ───────────────────────────────────────────
static struct udp_pcb *s_dhcp_pcb = NULL;

static void dhcp_recv(void *arg, struct udp_pcb *pcb,
                      struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    (void)arg; (void)addr; (void)port;
    if (!p || p->tot_len < 240) { if(p) pbuf_free(p); return; }

    uint8_t *msg = (uint8_t*)p->payload;
    if (msg[0] != 1) { pbuf_free(p); return; }

    uint8_t mtype = 0;
    uint8_t *opt = msg + 236;
    if (p->tot_len >= 240 && memcmp(opt,"\x63\x82\x53\x63",4)==0) {
        opt += 4;
        uint8_t *end = msg + p->tot_len;
        while (opt < end && *opt != 255) {
            if (*opt == 53 && opt[1] >= 1) { mtype = opt[2]; break; }
            opt += 2 + opt[1];
        }
    }
    if (mtype != 1 && mtype != 3) { pbuf_free(p); return; }

    uint8_t reply[300]; memset(reply,0,sizeof(reply));
    reply[0]=2; reply[1]=msg[1]; reply[2]=msg[2]; reply[3]=msg[3];
    memcpy(reply+4,msg+4,4);         // xid
    reply[10]=msg[10]; reply[11]=msg[11]; // flags
    reply[16]=192; reply[17]=168; reply[18]=7; reply[19]=1; // yiaddr
    reply[20]=192; reply[21]=168; reply[22]=7; reply[23]=2; // siaddr
    memcpy(reply+28,msg+28,6);       // chaddr
    reply[236]=0x63; reply[237]=0x82; reply[238]=0x53; reply[239]=0x63;
    uint8_t *o = reply+240;
    *o++=53;*o++=1;*o++=(mtype==1)?2:5;   // OFFER or ACK
    *o++=54;*o++=4;*o++=192;*o++=168;*o++=7;*o++=2; // server id
    *o++=51;*o++=4;*o++=0;*o++=0;*o++=0x54;*o++=0x60; // lease 6h
    *o++=1;*o++=4;*o++=255;*o++=255;*o++=255;*o++=0;  // subnet
    *o++=3;*o++=4;*o++=192;*o++=168;*o++=7;*o++=2;    // router
    *o++=6;*o++=4;*o++=8;*o++=8;*o++=8;*o++=8;        // dns
    *o++=255;

    struct pbuf *rp = pbuf_alloc(PBUF_TRANSPORT, (uint16_t)(o-reply), PBUF_RAM);
    if (rp) {
        memcpy(rp->payload, reply, o-reply);
        ip_addr_t bc; ip4_addr_set_any(ip_2_ip4(&bc));
        IP_ADDR4(&bc,255,255,255,255);
        udp_sendto(pcb, rp, &bc, 68);
        pbuf_free(rp);
    }
    pbuf_free(p);
}

static void dhcp_server_start(void) {
    s_dhcp_pcb = udp_new();
    if (!s_dhcp_pcb) return;
    udp_bind(s_dhcp_pcb, IP_ADDR_ANY, 67);
    udp_recv(s_dhcp_pcb, dhcp_recv, NULL);
}

// ── NCM USB descriptor registration ──────────────────────────────
// Interface layout: 0=HID, 1=NCM-ctrl, 2=NCM-data
#define NCM_ITF      1
#define NCM_STR_IDX  4    // "NCM" string
#define NCM_MAC_IDX  5    // MAC address string
#define NCM_EP_NOTIF 0x82 // interrupt IN
#define NCM_EP_OUT   0x03 // bulk OUT
#define NCM_EP_IN    0x83 // bulk IN

static uint16_t ncm_load_descriptor(uint8_t *dst, uint8_t *itf) {
    uint8_t desc[] = {
        TUD_CDC_NCM_DESCRIPTOR(NCM_ITF, NCM_STR_IDX, NCM_MAC_IDX,
                               NCM_EP_NOTIF, 64,
                               NCM_EP_OUT, NCM_EP_IN, 64, 1514)
    };
    memcpy(dst, desc, sizeof(desc));
    *itf += 2;
    return (uint16_t)sizeof(desc);
}

// ── Public API ────────────────────────────────────────────────────
void usb_ncm_init(void) {
    // Register NCM descriptor before USB.begin()
    tinyusb_enable_interface(USB_INTERFACE_CDC, TUD_CDC_NCM_DESC_LEN, ncm_load_descriptor);

    // Init lwIP + netif
    lwip_init();
    ip4_addr_t ip, mask, gw;
    ip4addr_aton(USB_NCM_BOARD_IP, &ip);
    ip4addr_aton(USB_NCM_NETMASK,  &mask);
    ip4addr_aton(USB_NCM_HOST_IP,  &gw);
    netif_add(&s_netif, &ip, &mask, &gw, NULL, ncm_netif_init, ethernet_input);
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    s_lwip_ready = true;

    dhcp_server_start();
}

void usb_ncm_poll(void) {
    sys_check_timeouts();
    bool conn = tud_ready() && tud_connected();
    if (conn != s_ncm_connected) {
        s_ncm_connected = conn;
        if (conn) netif_set_link_up(&s_netif);
        else      netif_set_link_down(&s_netif);
    }
}

bool usb_ncm_connected(void) { return s_ncm_connected; }

// ── HTTP POST over lwIP TCP ───────────────────────────────────────
struct http_ctx {
    volatile bool done, error;
    String resp, req;
};

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_ctx *ctx = (http_ctx*)arg;
    if (!p) { ctx->done = true; return ERR_OK; }
    if (err != ERR_OK) { ctx->error = ctx->done = true; pbuf_free(p); return err; }
    uint16_t pos = 0;
    for (struct pbuf *q = p; q; q = q->next) {
        ctx->resp += String((const char*)q->payload).substring(0, q->len);
        pos += q->len;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (ctx->resp.indexOf("\r\n\r\n") >= 0) ctx->done = true;
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    http_ctx *ctx = (http_ctx*)arg;
    ctx->error = ctx->done = true;
}

static err_t on_conn(void *arg, struct tcp_pcb *pcb, err_t err) {
    http_ctx *ctx = (http_ctx*)arg;
    if (err != ERR_OK) { ctx->error = ctx->done = true; return err; }
    const char *s = ctx->req.c_str();
    tcp_write(pcb, s, strlen(s), TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}

String usb_ncm_post(const char *path, const String &body, int timeout_ms) {
    if (!s_ncm_connected) return "";

    http_ctx ctx;
    ctx.done = ctx.error = false;
    ctx.req = String("POST ") + path + " HTTP/1.0\r\n"
            + "Host: " + USB_NCM_HOST_IP + "\r\n"
            + "Content-Type: application/json\r\n"
            + "Content-Length: " + String(body.length()) + "\r\n"
            + "Connection: close\r\n\r\n" + body;

    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return "";
    tcp_arg(pcb, &ctx);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);

    ip_addr_t host; IP_ADDR4(&host,192,168,7,1);
    if (tcp_connect(pcb, &host, USB_NCM_RELAY_PORT, on_conn) != ERR_OK) {
        tcp_abort(pcb); return "";
    }

    uint32_t t0 = millis();
    while (!ctx.done && (millis()-t0) < (uint32_t)timeout_ms) {
        usb_ncm_poll(); delay(1);
    }
    if (!ctx.done || ctx.error) { tcp_abort(pcb); return ""; }
    tcp_close(pcb);

    int bi = ctx.resp.indexOf("\r\n\r\n");
    return (bi >= 0) ? ctx.resp.substring(bi+4) : ctx.resp;
}
