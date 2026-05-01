#ifndef PCNET_H
#define PCNET_H

#include <stdint.h>

/* Initialize the AMD PCNet NIC. Returns 1 on success, 0 if not found. */
int pcnet_init(void);

/* Send a raw Ethernet frame (data must include Ethernet header).
   len = total frame length in bytes. Returns 1 on success. */
int pcnet_send(const uint8_t *data, uint16_t len);

/* Poll for a received frame. Copies into buf (must be >= 1536 bytes).
   Returns the frame length in bytes, or 0 if nothing ready. */
uint16_t pcnet_poll(uint8_t *buf);

/* Copy the 6-byte MAC address into out[6]. */
void pcnet_get_mac(uint8_t out[6]);

/* 1 if NIC is initialized and running, 0 otherwise. */
extern int pcnet_ready;

#endif /* PCNET_H */
