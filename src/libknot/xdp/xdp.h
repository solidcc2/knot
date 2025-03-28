/*  Copyright (C) CZ.NIC, z.s.p.o. and contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  For more information, see <https://www.knot-dns.cz/>
 */

/*!
 * \file
 *
 * \brief XDP IO interface.
 *
 * \addtogroup xdp
 * @{
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <netinet/in.h>

#include "libknot/xdp/bpf-consts.h"
#include "libknot/xdp/msg.h"

/*!
 * \brief Styles of loading BPF program.
 *
 * \note In *all* the cases loading can only succeed if at the end
 *       a compatible BPF program is loaded on the interface.
 */
typedef enum {
	KNOT_XDP_LOAD_BPF_NEVER,         /*!< Do not load; error out if not loaded already. */
	KNOT_XDP_LOAD_BPF_ALWAYS,        /*!< Always load a program (overwrite it). */
	KNOT_XDP_LOAD_BPF_ALWAYS_UNLOAD, /*!< KNOT_XDP_LOAD_BPF_ALWAYS + unload previous. */
	KNOT_XDP_LOAD_BPF_MAYBE,         /*!< Try with present program or load if none. */
	/* Implementation caveat: when re-using program in _MAYBE case, we get a message:
	 * libbpf: Kernel error message: XDP program already attached */
} knot_xdp_load_bpf_t;

/*! \brief Context structure for one XDP socket. */
typedef struct knot_xdp_socket knot_xdp_socket_t;

/*! \brief Configuration of XDP socket. */
struct knot_xdp_config {
	uint16_t ring_size;  /*!< Size of RX and TX rings (must be power of 2). */
	bool force_generic;  /*!< Use generic XDP mode (avoid driver/hardware implementation). */
	bool force_copy;     /*!< Force copying packet data between kernel and user-space (avoid zero-copy). */
	unsigned busy_poll_timeout; /*!< Preferred busy poll budget (0 means disabled). */
	unsigned busy_poll_budget;  /*!< Preferred busy poll timeout (in microseconds) . */
};

/*! \brief Configuration of XDP socket. */
typedef struct knot_xdp_config knot_xdp_config_t;

/*! \brief Various statistics of an XDP socket (optimally kernel >=5.9). */
typedef struct {
	/*! Interface name. */
	const char *if_name;
	/*! Interface name index (derived from ifname). */
	int if_index;
	/*! Network card queue id. */
	unsigned if_queue;
	/*! Counters (xdp_statistics) retrieved from the kernel via XDP_STATISTICS. */
	struct {
		/*! Dropped for other reasons. */
		uint64_t rx_dropped;
		/*! Dropped due to invalid descriptor. */
		uint64_t rx_invalid;
		/*! Dropped due to invalid descriptor. */
		uint64_t tx_invalid;
		/*! Dropped due to rx ring being full. */
		uint64_t rx_full;
		/*! Failed to retrieve item from fill ring. */
		uint64_t fq_empty;
		/*! Failed to retrieve item from tx ring. */
		uint64_t tx_empty;
	} socket;
	/*! States of rings of the XDP socket. */
	struct {
		/*! Busy TX buffers. */
		uint16_t tx_busy;
		/*! Free buffers to consume from FQ ring. */
		uint16_t fq_fill;
		/*! Pending buffers in TX ring. */
		uint16_t rx_fill;
		/*! Pending buffers in RX ring. */
		uint16_t tx_fill;
		/*! Pending buffers in CQ ring. */
		uint16_t cq_fill;
	} rings;
} knot_xdp_stats_t;

/*!
 * \brief Initialize XDP socket.
 *
 * \param socket       XDP socket.
 * \param if_name      Name of the net iface (e.g. eth0).
 * \param if_queue     Network card queue to be used (normally 1 socket per each queue).
 * \param flags        XDP filter configuration flags.
 * \param udp_port     UDP and/or TCP port to listen on if enabled via \a opts.
 * \param quic_port    QUIC/UDP port to listen on if enabled via \a opts.
 * \param load_bpf     Insert BPF program into packet processing.
 * \param xdp_config   Optional XDP socket configuration.
 *
 * \return KNOT_E* or -errno
 */
int knot_xdp_init(knot_xdp_socket_t **socket, const char *if_name, int if_queue,
                  knot_xdp_filter_flag_t flags, uint16_t udp_port, uint16_t quic_port,
                  knot_xdp_load_bpf_t load_bpf, const knot_xdp_config_t *xdp_config);

/*!
 * \brief De-init XDP socket.
 *
 * \param socket  XDP socket.
 */
void knot_xdp_deinit(knot_xdp_socket_t *socket);

/*!
 * \brief Return a file descriptor to be polled on for incoming packets.
 *
 * \param socket  XDP socket.
 *
 * \return KNOT_E*
 */
int knot_xdp_socket_fd(knot_xdp_socket_t *socket);

/*!
 * \brief Collect completed TX buffers, so they can be used by knot_xdp_send_alloc().
 *
 * \param socket  XDP socket.
 */
void knot_xdp_send_prepare(knot_xdp_socket_t *socket);

/*!
 * \brief Allocate one buffer for an outgoing packet.
 *
 * \param socket       XDP socket.
 * \param flags        Flags for new message.
 * \param out          Out: the allocated packet buffer.
 *
 * \return KNOT_E*
 */
int knot_xdp_send_alloc(knot_xdp_socket_t *socket, knot_xdp_msg_flag_t flags,
                        knot_xdp_msg_t *out);

/*!
 * \brief Allocate one buffer for a reply packet.
 *
 * \param socket       XDP socket.
 * \param query        The packet to be replied to.
 * \param out          Out: the allocated packet buffer.
 *
 * \return KNOT_E*
 */
int knot_xdp_reply_alloc(knot_xdp_socket_t *socket, const knot_xdp_msg_t *query,
                         knot_xdp_msg_t *out);

/*!
 * \brief Send multiple packets thru XDP.
 *
 * \note The packets all must have been allocated by knot_xdp_send_alloc()!
 * \note Do not free the packet payloads afterwards.
 * \note Packets with zero length will be skipped.
 *
 * \param socket  XDP socket.
 * \param msgs    Packets to be sent.
 * \param count   Number of packets.
 * \param sent    Out: number of packet successfully sent.
 *
 * \return KNOT_E*
 */
int knot_xdp_send(knot_xdp_socket_t *socket, const knot_xdp_msg_t msgs[],
                  uint32_t count, uint32_t *sent);

/*!
 * \brief Cleanup messages that have not been knot_xdp_send().
 *
 * ...possibly due to some error.
 *
 * \param socket   XDP socket.
 * \param msgs     Messages to be freed.
 * \param count    Number of messages.
 */
void knot_xdp_send_free(knot_xdp_socket_t *socket, const knot_xdp_msg_t msgs[],
                        uint32_t count);

/*!
 * \brief Syscall to kernel to wake up the network card driver after knot_xdp_send().
 *
 * \param socket  XDP socket.
 *
 * \return KNOT_E* or -errno
 */
int knot_xdp_send_finish(knot_xdp_socket_t *socket);

/*!
 * \brief Receive multiple packets thru XDP.
 *
 * \param socket     XDP socket.
 * \param msgs       Out: buffers to be filled in with incoming packets.
 * \param max_count  Limit for number of packets received at once.
 * \param count      Out: real number of received packets.
 * \param wire_size  Out: (optional) total wire size of received packets.
 *
 * \return KNOT_E*
 */
int knot_xdp_recv(knot_xdp_socket_t *socket, knot_xdp_msg_t msgs[],
                  uint32_t max_count, uint32_t *count, size_t *wire_size);

/*!
 * \brief Free buffers with received packets.
 *
 * \param socket  XDP socket.
 * \param msgs    Buffers with received packets.
 * \param count   Number of received packets to free.
 */
void knot_xdp_recv_finish(knot_xdp_socket_t *socket, const knot_xdp_msg_t msgs[],
                          uint32_t count);

/*!
 * \brief Print some info about the XDP socket.
 *
 * \param socket  XDP socket.
 * \param file    Output file.
 */
void knot_xdp_socket_info(const knot_xdp_socket_t *socket, FILE *file);

/*!
 * \brief Gets various statistics of the XDP socket.
 *
 * \param socket  XDP socket.
 * \param stats   Output structure.
 *
 * \return KNOT_E*
 */
int knot_xdp_socket_stats(knot_xdp_socket_t *socket, knot_xdp_stats_t *stats);

/*! @} */
