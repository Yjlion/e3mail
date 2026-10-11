/* SPDX-License-Identifier: MPL-2.0 */
/* The C interface of src/p2p (Rust, Iroh): bytes between e3mail devices.
 * Every call returns at once; what happens arrives as events, taken with
 * e3p2p_poll after the waker has been called (from another thread). Kept by
 * hand next to src/lib.rs, which defines each of these. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct e3p2p_node e3p2p_node;

enum { E3P2P_EVENT_CONNECTED = 1, E3P2P_EVENT_CLOSED = 2, E3P2P_EVENT_FRAME = 3 };
enum { E3P2P_RELAY_DEFAULT = 0, E3P2P_RELAY_CUSTOM = 1, E3P2P_RELAY_OFF = 2 };
#define E3P2P_MAX_FRAME (64u << 20)

typedef struct e3p2p_event {
    int32_t kind;
    uint64_t conn;
    char peer[65];   /* the peer's endpoint id */
    int32_t incoming; /* CONNECTED: the peer dialed us */
    uint8_t frame;   /* FRAME: its kind */
    uint8_t *data;   /* FRAME: its bytes; CLOSED: why, as text (not terminated) */
    size_t len;
} e3p2p_event;

typedef void (*e3p2p_waker)(void *ctx);

void e3p2p_generate_secret(uint8_t out[32]);
e3p2p_node *e3p2p_start(const uint8_t secret[32], int32_t relay, const char *relay_url, e3p2p_waker waker,
                        void *waker_ctx);
void e3p2p_stop(e3p2p_node *node);
void e3p2p_node_id(const e3p2p_node *node, char out[65]);
char *e3p2p_addr(const e3p2p_node *node); /* free with e3p2p_string_free */
int32_t e3p2p_wait_online(const e3p2p_node *node, uint64_t ms);
void e3p2p_string_free(char *s);
int32_t e3p2p_allow(const e3p2p_node *node, const char *peer, int32_t allowed);
void e3p2p_accept_any(const e3p2p_node *node, int32_t on);
uint64_t e3p2p_connect(const e3p2p_node *node, const char *addr);
int32_t e3p2p_send(const e3p2p_node *node, uint64_t conn, uint8_t kind, const uint8_t *data, size_t len);
void e3p2p_close(const e3p2p_node *node, uint64_t conn);
int32_t e3p2p_poll(const e3p2p_node *node, e3p2p_event *out);
void e3p2p_event_free(e3p2p_event *event);

#ifdef __cplusplus
}
#endif
