#pragma once
#include <stdint.h>

/* Return codes */
#define TCP_OK      0
#define TCP_TIMEOUT (-1)
#define TCP_ERROR   (-2)
#define TCP_RESET   (-3)
#define TCP_CANCEL  (-4)

/*
 * tcp_connect — perform 3-way handshake to dst_ip:dst_port.
 * Returns TCP_OK on success, negative on failure.
 */
int tcp_connect(uint32_t dst_ip, uint16_t dst_port);

/*
 * tcp_write — send data on an established connection.
 * Returns TCP_OK or negative.
 */
int tcp_write(const uint8_t *data, uint32_t len);

/*
 * tcp_read — receive all data until FIN or timeout, into buf.
 * Returns number of bytes received (0 = closed), or negative on error.
 */
int tcp_read(uint8_t *buf, uint32_t buflen);

/*
 * tcp_close — send FIN and wait briefly for ACK.
 */
void tcp_close(void);
