/*  Copyright (C) 2020 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libknot/attribute.h"
#include "libknot/endian.h"
#include "libknot/errcode.h"
#include "libknot/xdp/bpf-user.h"
#include "libknot/xdp/xdp.h"
#include "contrib/macros.h"

/* Don't fragment flag. */
#define	IP_DF 0x4000

#define FRAME_SIZE 2048
#define UMEM_FRAME_COUNT_RX 4096
#define UMEM_FRAME_COUNT_TX UMEM_FRAME_COUNT_RX // No reason to differ so far.
#define UMEM_RING_LEN_RX (UMEM_FRAME_COUNT_RX * 2)
#define UMEM_RING_LEN_TX (UMEM_FRAME_COUNT_TX * 2)
#define UMEM_FRAME_COUNT (UMEM_FRAME_COUNT_RX + UMEM_FRAME_COUNT_TX)

/* With recent compilers we statically check #defines for settings that
 * get refused by AF_XDP drivers (in current versions, at least). */
#if (__STDC_VERSION__ >= 201112L)
#define IS_POWER_OF_2(n) (((n) & (n - 1)) == 0)
_Static_assert((FRAME_SIZE == 4096 || FRAME_SIZE == 2048)
	&& IS_POWER_OF_2(UMEM_FRAME_COUNT)
	/* The following two inequalities aren't required by drivers, but they allow
	 * our implementation assume that the rings can never get filled. */
	&& IS_POWER_OF_2(UMEM_RING_LEN_RX) && UMEM_RING_LEN_RX > UMEM_FRAME_COUNT_RX
	&& IS_POWER_OF_2(UMEM_RING_LEN_TX) && UMEM_RING_LEN_TX > UMEM_FRAME_COUNT_TX
	&& UMEM_FRAME_COUNT_TX <= (1 << 16) /* see tx_free_indices */
	, "Incorrect #define combination for AF_XDP.");
#endif

/*! \brief The memory layout of IPv4 umem frame. */
struct udpv4 {
	union {
		uint8_t bytes[1];
		struct {
			struct ethhdr eth; // No VLAN support; CRC at the "end" of .data!
			struct iphdr ipv4;
			struct udphdr udp;
			uint8_t data[];
		} __attribute__((packed));
	};
};

/*! \brief The memory layout of IPv6 umem frame. */
struct udpv6 {
	union {
		uint8_t bytes[1];
		struct {
			struct ethhdr eth; // No VLAN support; CRC at the "end" of .data!
			struct ipv6hdr ipv6;
			struct udphdr udp;
			uint8_t data[];
		} __attribute__((packed));
	};
};

struct tcpv4 {
	union {
		uint8_t bytes[1];
		struct {
			struct ethhdr eth; // No VLAN support; CRC at the "end" of .data!
			struct iphdr ipv4;
			struct tcphdr tcp;
			uint8_t data[];
		} __attribute__((packed));
	};
};

struct tcpv6 {
	union {
		uint8_t bytes[1];
		struct {
			struct ethhdr eth; // No VLAN support; CRC at the "end" of .data!
			struct ipv6hdr ipv6;
			struct tcphdr tcp;
			uint8_t data[];
		} __attribute__((packed));
	};
};

/*! \brief The memory layout of each umem frame. */
struct umem_frame {
	union {
		uint8_t bytes[FRAME_SIZE];
		union {
			struct udpv4 udpv4;
			struct udpv6 udpv6;
			struct tcpv4 tcpv4;
			struct tcpv6 tcpv6;
		};
	};
};

_public_
size_t knot_xdp_payload_offset(knot_xdp_flags_t flags) {
	return sizeof(struct ethhdr) +
	       ((flags & KNOT_XDP_IPV6) ? sizeof(struct ipv6hdr) : sizeof(struct iphdr)) +
	       ((flags & KNOT_XDP_TCP) ? sizeof(struct tcphdr) : sizeof(struct udphdr)); // we don't do TCP options !!
}

static int configure_xsk_umem(struct kxsk_umem **out_umem)
{
	/* Allocate memory and call driver to create the UMEM. */
	struct kxsk_umem *umem = calloc(1,
		offsetof(struct kxsk_umem, tx_free_indices)
		+ sizeof(umem->tx_free_indices[0]) * UMEM_FRAME_COUNT_TX);
	if (umem == NULL) {
		return KNOT_ENOMEM;
	}

	int ret = posix_memalign((void **)&umem->frames, getpagesize(),
	                         FRAME_SIZE * UMEM_FRAME_COUNT);
	if (ret != 0) {
		free(umem);
		return KNOT_ENOMEM;
	}

	const struct xsk_umem_config config = {
		.fill_size = UMEM_RING_LEN_RX,
		.comp_size = UMEM_RING_LEN_TX,
		.frame_size = FRAME_SIZE,
		.frame_headroom = 0,
	};

	ret = xsk_umem__create(&umem->umem, umem->frames, FRAME_SIZE * UMEM_FRAME_COUNT,
	                       &umem->fq, &umem->cq, &config);
	if (ret != KNOT_EOK) {
		free(umem->frames);
		free(umem);
		return ret;
	}
	*out_umem = umem;

	/* Designate the starting chunk of buffers for TX, and put them onto the stack. */
	umem->tx_free_count = UMEM_FRAME_COUNT_TX;
	for (uint32_t i = 0; i < UMEM_FRAME_COUNT_TX; ++i) {
		umem->tx_free_indices[i] = i;
	}

	/* Designate the rest of buffers for RX, and pass them to the driver. */
	uint32_t idx = 0;
	ret = xsk_ring_prod__reserve(&umem->fq, UMEM_FRAME_COUNT_RX, &idx);
	if (ret != UMEM_FRAME_COUNT - UMEM_FRAME_COUNT_TX) {
		assert(0);
		return KNOT_ERROR;
	}
	assert(idx == 0);
	for (uint32_t i = UMEM_FRAME_COUNT_TX; i < UMEM_FRAME_COUNT; ++i) {
		*xsk_ring_prod__fill_addr(&umem->fq, idx++) = i * FRAME_SIZE;
	}
	xsk_ring_prod__submit(&umem->fq, UMEM_FRAME_COUNT_RX);

	return KNOT_EOK;
}

static void deconfigure_xsk_umem(struct kxsk_umem *umem)
{
	(void)xsk_umem__delete(umem->umem);
	free(umem->frames);
	free(umem);
}

static int configure_xsk_socket(struct kxsk_umem *umem,
                                const struct kxsk_iface *iface,
                                knot_xdp_socket_t **out_sock)
{
	knot_xdp_socket_t *xsk_info = calloc(1, sizeof(*xsk_info));
	if (xsk_info == NULL) {
		return KNOT_ENOMEM;
	}
	xsk_info->iface = iface;
	xsk_info->umem = umem;

	const struct xsk_socket_config sock_conf = {
		.tx_size = UMEM_RING_LEN_TX,
		.rx_size = UMEM_RING_LEN_RX,
		.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
	};

	int ret = xsk_socket__create(&xsk_info->xsk, iface->if_name,
	                             iface->if_queue, umem->umem,
	                             &xsk_info->rx, &xsk_info->tx, &sock_conf);
	if (ret != 0) {
		free(xsk_info);
		return ret;
	}

	*out_sock = xsk_info;
	return KNOT_EOK;
}

_public_
int knot_xdp_init(knot_xdp_socket_t **socket, const char *if_name, int if_queue,
                  uint32_t listen_port, knot_xdp_load_bpf_t load_bpf)
{
	if (socket == NULL || if_name == NULL) {
		return KNOT_EINVAL;
	}

	struct kxsk_iface *iface;
	int ret = kxsk_iface_new(if_name, if_queue, load_bpf, &iface);
	if (ret != KNOT_EOK) {
		return ret;
	}

	/* Initialize shared packet_buffer for umem usage. */
	struct kxsk_umem *umem = NULL;
	ret = configure_xsk_umem(&umem);
	if (ret != KNOT_EOK) {
		kxsk_iface_free(iface);
		return ret;
	}

	ret = configure_xsk_socket(umem, iface, socket);
	if (ret != KNOT_EOK) {
		deconfigure_xsk_umem(umem);
		kxsk_iface_free(iface);
		return ret;
	}

	ret = kxsk_socket_start(iface, listen_port, (*socket)->xsk);
	if (ret != KNOT_EOK) {
		xsk_socket__delete((*socket)->xsk);
		deconfigure_xsk_umem(umem);
		kxsk_iface_free(iface);
		free(*socket);
		*socket = NULL;
		return ret;
	}

	return ret;
}

_public_
void knot_xdp_deinit(knot_xdp_socket_t *socket)
{
	if (socket == NULL) {
		return;
	}

	kxsk_socket_stop(socket->iface);
	xsk_socket__delete(socket->xsk);
	deconfigure_xsk_umem(socket->umem);

	kxsk_iface_free((struct kxsk_iface *)/*const-cast*/socket->iface);
	free(socket);
}

_public_
int knot_xdp_socket_fd(knot_xdp_socket_t *socket)
{
	if (socket == NULL) {
		return 0;
	}

	return xsk_socket__fd(socket->xsk);
}

static void tx_free_relative(struct kxsk_umem *umem, uint64_t addr_relative)
{
	/* The address may not point to *start* of buffer, but `/` solves that. */
	uint64_t index = addr_relative / FRAME_SIZE;
	assert(index < UMEM_FRAME_COUNT);
	umem->tx_free_indices[umem->tx_free_count++] = index;
}

_public_
void knot_xdp_send_prepare(knot_xdp_socket_t *socket)
{
	if (socket == NULL) {
		return;
	}

	struct kxsk_umem *const umem = socket->umem;
	struct xsk_ring_cons *const cq = &umem->cq;

	uint32_t idx = 0;
	const uint32_t completed = xsk_ring_cons__peek(cq, UINT32_MAX, &idx);
	if (completed == 0) {
		return;
	}
	assert(umem->tx_free_count + completed <= UMEM_FRAME_COUNT_TX);

	for (uint32_t i = 0; i < completed; ++i) {
		uint64_t addr_relative = *xsk_ring_cons__comp_addr(cq, idx++);
		tx_free_relative(umem, addr_relative);
	}

	xsk_ring_cons__release(cq, completed);
}

static struct umem_frame *alloc_tx_frame(struct kxsk_umem *umem)
{
	if (unlikely(umem->tx_free_count == 0)) {
		return NULL;
	}

	uint32_t index = umem->tx_free_indices[--umem->tx_free_count];
	return umem->frames + index;
}

static uint32_t rnd_uint32(void)
{
	uint32_t res = rand() & 0xffff;
	res <<= 16;
	res |= rand() & 0xffff;
	return res;
}

_public_
int knot_xdp_send_alloc(knot_xdp_socket_t *socket, knot_xdp_flags_t flags, knot_xdp_msg_t *out,
                        const knot_xdp_msg_t *in_reply_to)
{
	if (socket == NULL || out == NULL) {
		return KNOT_EINVAL;
	}

	size_t ofs = knot_xdp_payload_offset(flags);

	struct umem_frame *uframe = alloc_tx_frame(socket->umem);
	if (uframe == NULL) {
		return KNOT_ENOMEM;
	}

	memset(out, 0, sizeof(*out));

	// TODO substitute this tripple `if` with knot_xdp_payload_offset()
	if (flags & KNOT_XDP_IPV6) {
		if (flags & KNOT_XDP_TCP) {
			out->payload.iov_base = uframe->tcpv6.data;
		} else {
			out->payload.iov_base = uframe->udpv6.data;
		}
	} else {
		if (flags & KNOT_XDP_TCP) {
			out->payload.iov_base = uframe->tcpv4.data;
		} else {
			out->payload.iov_base = uframe->udpv4.data;
		}
	}
	out->payload.iov_len = MIN(UINT16_MAX, FRAME_SIZE - ofs);

	const struct ethhdr *eth = (struct ethhdr *)uframe;
	out->eth_from = (void *)&eth->h_source;
	out->eth_to = (void *)&eth->h_dest;

	out->flags = flags;

	if (in_reply_to != NULL) {
		memcpy(out->eth_from, in_reply_to->eth_to, ETH_ALEN);
		memcpy(out->eth_to, in_reply_to->eth_from, ETH_ALEN);

		memcpy(&out->ip_from, &in_reply_to->ip_to, sizeof(out->ip_from));
		memcpy(&out->ip_to, &in_reply_to->ip_from, sizeof(out->ip_to));

		if (flags & KNOT_XDP_TCP) {
			assert(in_reply_to->flags & KNOT_XDP_TCP);
			out->ackno = in_reply_to->seqno;
			out->ackno += in_reply_to->payload.iov_len;
			if (in_reply_to->flags & KNOT_XDP_SYN) {
				out->ackno++;
			}
			out->seqno = in_reply_to->ackno;
			if (out->seqno == 0) {
				out->seqno = rnd_uint32();
			}
		}
	} else {
		if (flags & KNOT_XDP_TCP) {
			out->ackno = 0;
			out->seqno = rnd_uint32();
		}
	}

	return KNOT_EOK;
}

static uint16_t from32to16(uint32_t sum)
{
	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return sum;
}

static uint16_t ipv4_checksum(const uint8_t *ipv4_hdr)
{
	const uint16_t *h = (const uint16_t *)ipv4_hdr;
	uint32_t sum32 = 0;
	for (int i = 0; i < 10; ++i) {
		if (i != 5) {
			sum32 += h[i];
		}
	}
	return ~from32to16(sum32);
}

/* Checksum endianness implementation notes for ipv4_checksum() and udp_checksum_step().
 *
 * The basis for checksum is addition on big-endian 16-bit words, with bit 16 carrying
 * over to bit 0.  That can be viewed as first byte carrying to the second and the
 * second one carrying back to the first one, i.e. a symmetrical situation.
 * Therefore the result is the same even when arithmetics is done on litte-endian (!)
 */

static void udp_checksum_step(size_t *result, const void *_data, size_t _data_len)
{
	assert(!(_data_len & 1));
	const uint16_t *data = _data;
	size_t len = _data_len / 2;
	while (len-- > 0) {
		*result += *data++;
	}
}

static void udp_checksum_finish(size_t *result)
{
	while (*result > 0xffff) {
		*result = (*result & 0xffff) + (*result >> 16);
	}
	if (*result != 0xffff) {
		*result = ~*result;
	}
}

static uint8_t *msg_uframe_ptr(knot_xdp_socket_t *socket, const knot_xdp_msg_t *msg)
{
	uint8_t *uNULL = NULL;
	uint8_t *uframe_p = uNULL + ((msg->payload.iov_base - NULL) & ~(FRAME_SIZE - 1));

#ifndef NDEBUG
	// Re-parse the uframe_p and check if the provided iov_base matches
	uint8_t *frame = uframe_p, proto;
	if (msg->payload.iov_base - (void *)frame > 256) {
		frame += 256;
	}
	const struct ethhdr *eth = (struct ethhdr *)frame;
	const struct iphdr *ip4 = NULL;
	const struct ipv6hdr *ip6 = NULL;
	const struct tcphdr *tcp = NULL;
	void *next_hdr = frame + sizeof(struct ethhdr);

	switch (eth->h_proto) {
	case __constant_htons(ETH_P_IP):
		ip4 = next_hdr;
		proto = ip4->protocol;
		next_hdr += ip4->ihl * 4;
		break;
	case __constant_htons(ETH_P_IPV6):
		ip6 = next_hdr;
		proto = ip6->nexthdr;
		next_hdr += sizeof(struct ipv6hdr);
		break;
	case 0:
		// FIXME WTF is this?
		goto skip_check;
	default:
		assert(0);
	}

	switch (proto) {
	case IPPROTO_UDP:
		next_hdr += sizeof(struct udphdr);
		break;
	case IPPROTO_TCP:
		tcp = next_hdr;
		next_hdr += tcp->doff * 4;
		break;
	default:
		assert(0);
	}
	assert(next_hdr == msg->payload.iov_base);
skip_check:
	;
#endif

#ifndef NDEBUG
	// now check if we are not outside of umem completely
	const uint8_t *umem_mem_start = socket->umem->frames->bytes;
	const uint8_t *umem_mem_end = umem_mem_start + FRAME_SIZE * UMEM_FRAME_COUNT;
	assert(umem_mem_start <= uframe_p && uframe_p < umem_mem_end);
#endif

	return uframe_p;
}

static void xsk_sendmsg_ipv4(knot_xdp_socket_t *socket, const knot_xdp_msg_t *msg,
                             uint32_t index)
{
	uint8_t *uframe_p = msg_uframe_ptr(socket, msg);
	struct umem_frame *uframe = (struct umem_frame *)uframe_p;
	struct udpv4 *h = &uframe->udpv4;
	struct tcpv4 *ht = &uframe->tcpv4;

	const struct sockaddr_in *src_v4 = (const struct sockaddr_in *)&msg->ip_from;
	const struct sockaddr_in *dst_v4 = (const struct sockaddr_in *)&msg->ip_to;
	const uint16_t pay_len = ((msg->flags & KNOT_XDP_TCP) ? sizeof (ht->tcp) : sizeof(h->udp)) + msg->payload.iov_len;

	h->eth.h_proto = __constant_htons(ETH_P_IP);

	h->ipv4.version  = IPVERSION;
	h->ipv4.ihl      = 5;
	h->ipv4.tos      = 0;
	h->ipv4.tot_len  = htobe16(5 * 4 + pay_len);
	h->ipv4.id       = 0;
	h->ipv4.frag_off = 0;
	h->ipv4.ttl      = IPDEFTTL;
	memcpy(&h->ipv4.saddr, &src_v4->sin_addr, sizeof(src_v4->sin_addr));
	memcpy(&h->ipv4.daddr, &dst_v4->sin_addr, sizeof(dst_v4->sin_addr));
	h->ipv4.check    = ipv4_checksum(h->bytes + sizeof(struct ethhdr));

	if (!(msg->flags & KNOT_XDP_TCP)) {
		h->ipv4.protocol = IPPROTO_UDP;

		h->udp.len    = htobe16(pay_len);
		h->udp.source = src_v4->sin_port;
		h->udp.dest   = dst_v4->sin_port;
		h->udp.check  = 0; // Optional for IPv4 - not computed.
	} else {
		ht->ipv4.protocol = IPPROTO_TCP;

		ht->tcp.source = src_v4->sin_port;
		ht->tcp.dest   = dst_v4->sin_port;
		ht->tcp.doff   = 5; // size of TCP hdr with no options in 32bit dwords

		ht->tcp.seq = htobe32(msg->seqno);
		ht->tcp.ack_seq = htobe32(msg->ackno);

		ht->tcp.syn = ((msg->flags & KNOT_XDP_SYN) ? 1 : 0);
		ht->tcp.ack = ((msg->flags & KNOT_XDP_ACK) ? 1 : 0);
		ht->tcp.fin = ((msg->flags & KNOT_XDP_FIN) ? 1 : 0);
		ht->tcp.psh = ((msg->payload.iov_len > 0) ? 1 : 0);

		ht->tcp.window = htobe16(MIN(UINT16_MAX, FRAME_SIZE - knot_xdp_payload_offset(msg->flags)));
	}

	*xsk_ring_prod__tx_desc(&socket->tx, index) = (struct xdp_desc){
		.addr = h->bytes - socket->umem->frames->bytes,
		.len = knot_xdp_payload_offset(msg->flags) + msg->payload.iov_len
	};
}

static void xsk_sendmsg_ipv6(knot_xdp_socket_t *socket, const knot_xdp_msg_t *msg,
                             uint32_t index)
{
	uint8_t *uframe_p = msg_uframe_ptr(socket, msg);
	struct umem_frame *uframe = (struct umem_frame *)uframe_p;
	struct udpv6 *h = &uframe->udpv6;

	const struct sockaddr_in6 *src_v6 = (const struct sockaddr_in6 *)&msg->ip_from;
	const struct sockaddr_in6 *dst_v6 = (const struct sockaddr_in6 *)&msg->ip_to;
	const uint16_t udp_len = sizeof(h->udp) + msg->payload.iov_len;

	h->eth.h_proto = __constant_htons(ETH_P_IPV6);

	assert(!(msg->flags & KNOT_XDP_TCP) && "FIXME implement");

	h->ipv6.version     = 6;
	h->ipv6.priority    = 0;
	memset(h->ipv6.flow_lbl, 0, sizeof(h->ipv6.flow_lbl));
	h->ipv6.payload_len = htobe16(udp_len);
	h->ipv6.nexthdr     = IPPROTO_UDP;
	h->ipv6.hop_limit   = IPDEFTTL;
	memcpy(&h->ipv6.saddr, &src_v6->sin6_addr, sizeof(src_v6->sin6_addr));
	memcpy(&h->ipv6.daddr, &dst_v6->sin6_addr, sizeof(dst_v6->sin6_addr));

	h->udp.len    = htobe16(udp_len);
	h->udp.source = src_v6->sin6_port;
	h->udp.dest   = dst_v6->sin6_port;
	h->udp.check  = 0; // Mandatory for IPv6 - computed afterwards.

	size_t chk = 0;
	udp_checksum_step(&chk, &h->ipv6.saddr, sizeof(h->ipv6.saddr));
	udp_checksum_step(&chk, &h->ipv6.daddr, sizeof(h->ipv6.daddr));
	udp_checksum_step(&chk, &h->udp.len, sizeof(h->udp.len));
	__be16 version = htobe16(h->ipv6.nexthdr);
	udp_checksum_step(&chk, &version, sizeof(version));
	udp_checksum_step(&chk, &h->udp, sizeof(h->udp));
	size_t padded_len = msg->payload.iov_len;
	if (padded_len & 1) {
		((uint8_t *)msg->payload.iov_base)[padded_len++] = 0;
	}
	udp_checksum_step(&chk, msg->payload.iov_base, padded_len);
	udp_checksum_finish(&chk);
	h->udp.check = chk;

	*xsk_ring_prod__tx_desc(&socket->tx, index) = (struct xdp_desc){
		.addr = h->bytes - socket->umem->frames->bytes,
		.len = knot_xdp_payload_offset(true) + msg->payload.iov_len
	};
}

static bool send_msg(const knot_xdp_msg_t *msg)
{
	if (msg->flags & KNOT_XDP_TCP) {
		if (msg->flags & (KNOT_XDP_SYN | KNOT_XDP_ACK | KNOT_XDP_FIN)) {
			return true;
		}
	}
	return msg->payload.iov_len > 0;
}

static bool send_msg46(const knot_xdp_msg_t *msg, bool ipv6) {
	return send_msg(msg) && ipv6 == ((msg->flags & KNOT_XDP_IPV6) ? true : false);
}

_public_
int knot_xdp_send(knot_xdp_socket_t *socket, const knot_xdp_msg_t msgs[],
                  uint32_t count, uint32_t *sent)
{
	if (socket == NULL || msgs == NULL || sent == NULL) {
		return KNOT_EINVAL;
	}

	/* Now we want to do something close to
	 *   xsk_ring_prod__reserve(&socket->tx, count, *idx)
	 * but we don't know in advance if we utilize *whole* `count`,
	 * and the API doesn't allow "cancelling reservations".
	 * Therefore we handle `socket->tx.cached_prod` by hand;
	 * that's simplified by the fact that there is always free space.
	 */
	assert(UMEM_RING_LEN_TX > UMEM_FRAME_COUNT_TX);
	uint32_t idx = socket->tx.cached_prod;

	for (uint32_t i = 0; i < count; ++i) {
		const knot_xdp_msg_t *msg = &msgs[i];

		if (send_msg46(msg, false)) {
			xsk_sendmsg_ipv4(socket, msg, idx++);
		} else if (send_msg46(msg, true)) {
			xsk_sendmsg_ipv6(socket, msg, idx++);
		} else {
			/* Some problem; we just ignore this message. */
			uint64_t addr_relative = (uint8_t *)msg->payload.iov_base
			                         - socket->umem->frames->bytes;
			tx_free_relative(socket->umem, addr_relative);
		}
	}

	*sent = idx - socket->tx.cached_prod;
	assert(*sent <= count);
	socket->tx.cached_prod = idx;
	xsk_ring_prod__submit(&socket->tx, *sent);
	socket->kernel_needs_wakeup = true;

	return KNOT_EOK;
}

_public_
int knot_xdp_send_finish(knot_xdp_socket_t *socket)
{
	if (socket == NULL) {
		return KNOT_EINVAL;
	}

	/* Trigger sending queued packets. */
	if (!socket->kernel_needs_wakeup) {
		return KNOT_EOK;
	}

	int ret = sendto(xsk_socket__fd(socket->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
	const bool is_ok = (ret >= 0);
	// List of "safe" errors taken from
	// https://github.com/torvalds/linux/blame/master/samples/bpf/xdpsock_user.c
	const bool is_again = !is_ok && (errno == ENOBUFS || errno == EAGAIN
	                                || errno == EBUSY || errno == ENETDOWN);
	// Some of the !is_ok cases are a little unclear - what to do about the syscall,
	// including how caller of _sendmsg_finish() should react.
	if (is_ok || !is_again) {
		socket->kernel_needs_wakeup = false;
	}
	if (is_again) {
		return KNOT_EAGAIN;
	} else if (is_ok) {
		return KNOT_EOK;
	} else {
		return -errno;
	}
	/* This syscall might be avoided with a newer kernel feature (>= 5.4):
	   https://www.kernel.org/doc/html/latest/networking/af_xdp.html#xdp-use-need-wakeup-bind-flag
	   Unfortunately it's not easy to continue supporting older kernels
	   when using this feature on newer ones.
	 */
}

static void rx_desc(knot_xdp_socket_t *socket, const struct xdp_desc *desc,
                    knot_xdp_msg_t *msg)
{
	uint8_t *uframe_p = socket->umem->frames->bytes + desc->addr, proto;
	const struct ethhdr *eth = (struct ethhdr *)uframe_p;
	const struct iphdr *ip4 = NULL;
	const struct ipv6hdr *ip6 = NULL;
	void *next_hdr = uframe_p + sizeof(struct ethhdr);
	uint16_t pay_len;

	msg->flags = 0;

	switch (eth->h_proto) {
	case __constant_htons(ETH_P_IP):
		ip4 = next_hdr;
		// Next conditions are ensured by the BPF filter.
		assert(ip4->version == 4);
		assert(ip4->frag_off == 0 ||
		       ip4->frag_off == __constant_htons(IP_DF));
		proto = ip4->protocol;
		// IPv4 header checksum is not verified!
		pay_len = be16toh(ip4->tot_len) - ip4->ihl * 4;
		next_hdr += ip4->ihl * 4;
		break;
	case __constant_htons(ETH_P_IPV6):
		ip6 = next_hdr;
		// Next conditions are ensured by the BPF filter.
		assert(ip6->version == 6);
		proto = ip6->nexthdr;
		pay_len = be16toh(ip6->payload_len);
		next_hdr += sizeof(struct ipv6hdr);
		msg->flags |= KNOT_XDP_IPV6;
		break;
	default:
		assert(0);
		msg->payload.iov_len = 0;
		return;
	}

	const struct udphdr *udp = NULL;
	const struct tcphdr *tcp = NULL;
	uint16_t sport, dport;

	switch (proto) {
	case IPPROTO_UDP:
		udp = next_hdr;
		// UDP checksum is not verified!
		assert(pay_len == be16toh(udp->len));
		pay_len -= sizeof(struct udphdr);
		next_hdr += sizeof(struct udphdr);
		sport = udp->source;
		dport = udp->dest;
		break;
	case IPPROTO_TCP:
		tcp = next_hdr;
		pay_len -= tcp->doff * 4;
		next_hdr += tcp->doff * 4;
		sport = tcp->source;
		dport = tcp->dest;
		msg->flags |= KNOT_XDP_TCP;
		if (tcp->syn) {
			msg->flags |= KNOT_XDP_SYN;
		}
		if (tcp->ack) {
			msg->flags |= KNOT_XDP_ACK;
		}
		if (tcp->fin) {
			msg->flags |= KNOT_XDP_FIN;
		}
		msg->seqno = be32toh(tcp->seq);
		msg->ackno = be32toh(tcp->ack_seq);
		break;
	default:
		assert(0);
		msg->payload.iov_len = 0;
		return;
	}

	assert(eth && (!!ip4 != !!ip6) && (!!udp != !!tcp));

	msg->payload.iov_base = next_hdr;
	msg->payload.iov_len = pay_len;

	msg->eth_from = (void *)&eth->h_source;
	msg->eth_to = (void *)&eth->h_dest;

	if (ip4 != NULL) {
		struct sockaddr_in *src_v4 = (struct sockaddr_in *)&msg->ip_from;
		struct sockaddr_in *dst_v4 = (struct sockaddr_in *)&msg->ip_to;
		memcpy(&src_v4->sin_addr, &ip4->saddr, sizeof(src_v4->sin_addr));
		memcpy(&dst_v4->sin_addr, &ip4->daddr, sizeof(dst_v4->sin_addr));
		src_v4->sin_port = sport;
		dst_v4->sin_port = dport;
		src_v4->sin_family = AF_INET;
		dst_v4->sin_family = AF_INET;
	} else {
		assert(ip6);
		struct sockaddr_in6 *src_v6 = (struct sockaddr_in6 *)&msg->ip_from;
		struct sockaddr_in6 *dst_v6 = (struct sockaddr_in6 *)&msg->ip_to;
		memcpy(&src_v6->sin6_addr, &ip6->saddr, sizeof(src_v6->sin6_addr));
		memcpy(&dst_v6->sin6_addr, &ip6->daddr, sizeof(dst_v6->sin6_addr));
		src_v6->sin6_port = sport;
		dst_v6->sin6_port = dport;
		src_v6->sin6_family = AF_INET6;
		dst_v6->sin6_family = AF_INET6;
		// Flow label is ignored.
	}
}

_public_
int knot_xdp_recv(knot_xdp_socket_t *socket, knot_xdp_msg_t msgs[],
                  uint32_t max_count, uint32_t *count)
{
	if (socket == NULL || msgs == NULL || count == NULL) {
		return KNOT_EINVAL;
	}

	uint32_t idx = 0;
	const uint32_t available = xsk_ring_cons__peek(&socket->rx, max_count, &idx);
	if (available == 0) {
		*count = 0;
		return KNOT_EOK;
	}
	assert(available <= max_count);

	for (uint32_t i = 0; i < available; ++i) {
		rx_desc(socket, xsk_ring_cons__rx_desc(&socket->rx, idx++), &msgs[i]);
	}

	xsk_ring_cons__release(&socket->rx, available);
	*count = available;

	return KNOT_EOK;
}

_public_
void knot_xdp_recv_finish(knot_xdp_socket_t *socket, const knot_xdp_msg_t msgs[],
                          uint32_t count)
{
	if (socket == NULL || msgs == NULL) {
		return;
	}

	struct kxsk_umem *const umem = socket->umem;
	struct xsk_ring_prod *const fq = &umem->fq;

	uint32_t idx = 0;
	const uint32_t reserved = xsk_ring_prod__reserve(fq, count, &idx);
	assert(reserved == count);

	for (uint32_t i = 0; i < reserved; ++i) {
		uint8_t *uframe_p = msg_uframe_ptr(socket, &msgs[i]);
		uint64_t offset = uframe_p - umem->frames->bytes;
		*xsk_ring_prod__fill_addr(fq, idx++) = offset;
	}

	xsk_ring_prod__submit(fq, reserved);
}

_public_
void knot_xdp_info(const knot_xdp_socket_t *socket, FILE *file)
{
	if (socket == NULL || file == NULL) {
		return;
	}

	// The number of busy frames
	#define RING_BUSY(ring) \
		((*(ring)->producer - *(ring)->consumer) & (ring)->mask)

	#define RING_PRINFO(name, ring) \
		fprintf(file, "Ring %s: size %4d, busy %4d (prod %4d, cons %4d)\n", \
		        name, (unsigned)(ring)->size, \
		        (unsigned)RING_BUSY((ring)), \
		        (unsigned)*(ring)->producer, (unsigned)*(ring)->consumer)

	const int rx_busyf = RING_BUSY(&socket->umem->fq) + RING_BUSY(&socket->rx);
	fprintf(file, "\nLOST RX frames: %4d", (int)(UMEM_FRAME_COUNT_RX - rx_busyf));

	const int tx_busyf = RING_BUSY(&socket->umem->cq) + RING_BUSY(&socket->tx);
	const int tx_freef = socket->umem->tx_free_count;
	fprintf(file, "\nLOST TX frames: %4d\n", (int)(UMEM_FRAME_COUNT_TX - tx_busyf - tx_freef));

	RING_PRINFO("FQ", &socket->umem->fq);
	RING_PRINFO("RX", &socket->rx);
	RING_PRINFO("TX", &socket->tx);
	RING_PRINFO("CQ", &socket->umem->cq);
	fprintf(file, "TX free frames: %4d\n", tx_freef);
}
