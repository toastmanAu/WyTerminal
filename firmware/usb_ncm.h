/*
 * usb_ncm.h — USB NCM ethernet interface for WyTerminal v3
 *
 * Board presents as USB CDC-NCM ethernet adapter.
 * Board IP: 192.168.7.2
 * Host IP:  192.168.7.1 (assigned via minimal DHCP)
 *
 * Uses TinyUSB NCM class (CFG_TUD_NCM=1, compiled into ESP32 Arduino core)
 * + ESP-IDF lwIP for TCP/IP stack
 */

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
#include <Arduino.h>
extern "C" {
#endif

#define USB_NCM_BOARD_IP   "192.168.7.2"
#define USB_NCM_HOST_IP    "192.168.7.1"
#define USB_NCM_NETMASK    "255.255.255.0"
#define USB_NCM_RELAY_PORT 7799

// Call once in setup() after USB.begin()
void usb_ncm_init(void);

// Call in loop() — pumps TinyUSB NCM + lwIP timers
void usb_ncm_poll(void);

// Returns true if host is connected and IP link is up
bool usb_ncm_connected(void);

// HTTP POST to relay via USB NCM (returns body string, "" on fail)
String usb_ncm_post(const char *path, const String &body, int timeout_ms);

#ifdef __cplusplus
}
#endif
