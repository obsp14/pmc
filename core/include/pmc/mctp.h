/*
 * mctp.h - MCTP (DMTF DSP0236) endpoint + serial binding (DSP0253)
 *
 *  UART bytes --> serial deframer --> packet layer (reassembly) --> message
 *                                                                  handler
 *  message --> fragment into 64-byte-MTU packets --> serial framer --> UART
 *
 * Serial frame (DSP0253):
 *   0x7E | revision 0x01 | byte_count | MCTP packet ... | FCS hi | FCS lo | 0x7E
 *   0x7E / 0x7D inside the frame are escaped as 0x7D (byte ^ 0x20).
 *
 * MCTP packet header (4 bytes):
 *   [0] hdr version (0x01)  [1] dest EID  [2] src EID
 *   [3] SOM(7) EOM(6) pkt_seq(5:4) TO(3) msg_tag(2:0)
 * First byte of a message body = IC(7) | message type (0x00 control, 0x01 PLDM)
 */
#ifndef PMC_MCTP_H
#define PMC_MCTP_H

#include "pmc/common.h"

#define MCTP_HDR_VERSION    0x01
#define MCTP_SERIAL_REV     0x01
#define MCTP_SERIAL_FLAG    0x7E
#define MCTP_SERIAL_ESC     0x7D
#define MCTP_BTU            64u     /* baseline transmission unit (payload) */
#define MCTP_PKT_MAX        (4u + MCTP_BTU)
#define MCTP_MSG_MAX        1024u   /* largest reassembled message we accept */

#define MCTP_EID_NULL       0x00
#define MCTP_EID_BROADCAST  0xFF

#define MCTP_TYPE_CONTROL   0x00
#define MCTP_TYPE_PLDM      0x01

#define MCTP_FLAG_SOM 0x80
#define MCTP_FLAG_EOM 0x40
#define MCTP_FLAG_TO  0x08

/* ---------------- serial binding ---------------- */
typedef struct {
	enum { DF_IDLE, DF_REV, DF_LEN, DF_DATA, DF_FCS1, DF_FCS2, DF_END } st;
	int esc;
	uint8_t len, pos;
	uint8_t buf[MCTP_PKT_MAX];
	uint16_t fcs_rx;
	/* statistics: useful on a real link */
	uint32_t frames_ok, err_fcs, err_len, err_rev, err_framing;
} pmc_mctp_deframer;

void pmc_mctp_deframer_init(pmc_mctp_deframer *d);
/* Feed one byte. Returns packet length (>0) when a valid packet is in
 * d->buf, 0 otherwise. */
int pmc_mctp_deframer_feed(pmc_mctp_deframer *d, uint8_t byte);

/* Frame one packet. Returns the framed length or PMC_E_NOSPC. */
int pmc_mctp_frame(const uint8_t *pkt, size_t len, uint8_t *out, size_t outmax);

/* ---------------- endpoint ---------------- */
struct pmc_mctp;
typedef void (*pmc_mctp_tx_fn)(void *arg, const uint8_t *frame, size_t len);
typedef void (*pmc_mctp_rx_fn)(struct pmc_mctp *m, uint8_t src_eid, uint8_t tag,
			       int tag_owner, const uint8_t *msg, size_t len);

typedef struct pmc_mctp {
	uint8_t eid;                 /* our endpoint ID (0 until assigned) */
	pmc_mctp_tx_fn tx;
	void *tx_arg;
	pmc_mctp_rx_fn rx;
	void *user;
	/* single reassembly context (one message in flight per peer is the
	 * common case for a management endpoint) */
	struct {
		int active;
		uint8_t src, tag, to, next_seq;
		size_t len;
		uint8_t buf[MCTP_MSG_MAX];
	} rasm;
	uint32_t rx_msgs, rx_dropped;
	uint8_t next_tag;
} pmc_mctp;

void pmc_mctp_init(pmc_mctp *m, uint8_t eid, pmc_mctp_tx_fn tx, void *tx_arg,
		   pmc_mctp_rx_fn rx, void *user);

/* Handle one received packet (from the deframer). */
void pmc_mctp_rx_packet(pmc_mctp *m, const uint8_t *pkt, size_t len);

/* Send a message (first byte = message type), fragmenting as needed. */
pmc_err_t pmc_mctp_send(pmc_mctp *m, uint8_t dest, int tag_owner, uint8_t tag,
			const uint8_t *msg, size_t len);

/* Allocate a tag for a request we originate (tag_owner = 1). */
uint8_t pmc_mctp_alloc_tag(pmc_mctp *m);

/* MCTP control protocol responder (Set/Get EID, versions, message types). */
void pmc_mctp_control_handle(pmc_mctp *m, uint8_t src, uint8_t tag,
			     const uint8_t *msg, size_t len);

#endif
