/*
 * P4OS - what lwIP asks P4OS, from inside its own code (ESP_IDF_LWIP_HOOK_FILENAME,
 * set for the lwip component in main/CMakeLists.txt).
 *
 * The firmware is built with IP forwarding and NAT (sdkconfig.defaults), which
 * one tool needs: sharing the board's connection with a device on the bench
 * network. Left to itself lwIP would then pass packets between every network
 * the board is on, the house's Wi-Fi, the wire, the USB cable and the board's
 * own access point. This hook is asked before any packet is forwarded, and
 * says no unless sharing is on and the packet belongs to it
 * (aos_ip4_canforward in aos_net_p4.c).
 */
#pragma once

struct pbuf;
/* 0: do not forward; < 0: lwIP's own rules decide. dest is in host byte order. */
int aos_ip4_canforward(struct pbuf *p, unsigned int dest);
#define LWIP_HOOK_IP4_CANFORWARD(p, dest) aos_ip4_canforward((p), (unsigned int)(dest))
