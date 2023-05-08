// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011-2017  Intel Corporation. All rights reserved.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <getopt.h>
#include <signal.h>

#include <ctype.h>
#include <syslog.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <ell/ell.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"
#include "lib/l2cap.h"
#include "src/shared/btp.h"

#include "src/shared/util.h"
#include "monitor/display.h"

#define AD_PATH "/org/bluez/advertising"
#define AG_PATH "/org/bluez/agent"
#define AD_IFACE "org.bluez.LEAdvertisement1"
#define AG_IFACE "org.bluez.Agent1"

/* List of assigned numbers for advetising data and scan response */
#define AD_TYPE_FLAGS				0x01
#define AD_TYPE_INCOMPLETE_UUID16_SERVICE_LIST	0x02
#define AD_TYPE_SHORT_NAME			0x08
#define AD_TYPE_TX_POWER			0x0a
#define AD_TYPE_SOLICIT_UUID16_SERVICE_LIST	0x14
#define AD_TYPE_SERVICE_DATA_UUID16		0x16
#define AD_TYPE_APPEARANCE			0x19
#define AD_TYPE_MANUFACTURER_DATA		0xff

#define NIBBLE_TO_ASCII(c)  ((c) < 0x0a ? (c) + 0x30 : (c) + 0x57)

static unsigned char *buf;

/* Default mtu */
static int imtu = 672;
static int omtu = 0;

/* Default FCS option */
static int fcs = 0x01;

/* Default Transmission Window */
static int txwin_size = 63;

/* Default Max Transmission */
static int max_transmit = 3;

/* Default data size */
static long data_size = -1;
static long buffer_size = 2048;

/* Default addr and psm and cid */
static bdaddr_t bdaddr_local;
static unsigned short psm = 0;
static unsigned short cid = 0;

/* Default number of frames to send (-1 = infinite) */
static int num_frames = 2;

/* Default number of consecutive frames before the delay */
static int count = 1;

/* Default delay after sending count number of frames */
static unsigned long send_delay = 0;

/* Default delay before receiving */
static unsigned long recv_delay = 0;

/* Default delay before disconnecting */
static unsigned long disc_delay = 0;

/* Initial sequence value when sending frames */
static int seq_start = 0;

static const char *filename = NULL;

static int rfcmode = 0;
static int central = 0;
static int auth = 0;
static int encr = 0;
static int secure = 0;
static int socktype = SOCK_SEQPACKET;
static int linger = 0;
static int reliable = 0;
static int timestamp = 0;
static int defer_setup = 0;
static int priority = -1;
static int rcvbuf = 0;
static int chan_policy = -1;
static int bdaddr_type = 0;

static int socket_l2cap = -1; // L2CAP socket created by do_connect().
static int socket_l2cap_accepted = -1;  // L2CAP socket accepted in do_listen().

static void register_gap_service(void);
static void register_l2cap_service(void);

static struct l_dbus *dbus;

struct btp_adapter {
	struct l_dbus_proxy *proxy;
	struct l_dbus_proxy *ad_proxy;
	uint8_t index;
	uint32_t supported_settings;
	uint32_t current_settings;
	uint32_t default_settings;
	struct l_queue *devices;
};

struct btp_device {
	struct l_dbus_proxy *proxy;
	uint8_t address_type;
	bdaddr_t address;
};

static struct l_queue *adapters;
static char *socket_path;
static struct btp *btp;

static bool gap_service_registered;
static bool l2cap_service_registered;
static bool gatt_client_service_registered;
static bool gatt_server_service_registered;

struct ad_data {
	uint8_t data[25];
	uint8_t len;
};

struct service_data {
	char *uuid;
	struct ad_data data;
};

struct manufacturer_data {
	uint16_t id;
	struct ad_data data;
};

static struct ad {
	bool registered;
	char *type;
	char *local_name;
	uint16_t local_appearance;
	uint16_t duration;
	uint16_t timeout;
	struct l_queue *uuids;
	struct l_queue *services;
	struct l_queue *manufacturers;
	struct l_queue *solicits;
	bool tx_power;
	bool name;
	bool appearance;
} ad;

static struct btp_agent {
	bool registered;
	struct l_dbus_proxy *proxy;
	struct l_dbus_message *pending_req;
} ag;

static char *dupuuid2str(const uint8_t *uuid, uint8_t len)
{
	switch (len) {
	case 16:
		return l_strdup_printf("%hhx%hhx", uuid[0], uuid[1]);
	case 128:
		return l_strdup_printf("%hhx%hhx%hhx%hhx%hhx%hhx%hhx%hhx%hhx"
					"%hhx%hhx%hhx%hhx%hhx%hhx%hhx", uuid[0],
					uuid[1], uuid[2], uuid[3], uuid[4],
					uuid[5], uuid[6], uuid[6], uuid[8],
					uuid[7], uuid[10], uuid[11], uuid[12],
					uuid[13], uuid[14], uuid[15]);
	default:
		return NULL;
	}
}

struct lookup_table {
	const char *name;
	int flag;
};

static struct lookup_table l2cap_modes[] = {
	{ "basic",	BT_MODE_BASIC		},
	/* Not implemented
	{ "flowctl",	BT_MODE_FLOWCTL		},
	{ "retrans",	BT_MODE_RETRANS		},
	*/
	{ "ertm",	BT_MODE_ERTM		},
	{ "streaming",	BT_MODE_STREAMING	},
	{ "ext-flowctl",BT_MODE_EXT_FLOWCTL	},
	{ 0 }
};

static struct lookup_table chan_policies[] = {
	{ "bredr",	BT_CHANNEL_POLICY_BREDR_ONLY		},
	{ "bredr_pref",	BT_CHANNEL_POLICY_BREDR_PREFERRED	},
	{ "amp_pref",	BT_CHANNEL_POLICY_AMP_PREFERRED		},
	{ NULL,		0					},
};

static struct lookup_table bdaddr_types[] = {
	{ "bredr",	BDADDR_BREDR		},
	{ "le_public",	BDADDR_LE_PUBLIC	},
	{ "le_random",	BDADDR_LE_RANDOM	},
	{ NULL,		0			},
};

static int bt_mode_to_l2cap_mode(int mode)
{
	switch (mode) {
	case BT_MODE_BASIC:
		return L2CAP_MODE_BASIC;
	case BT_MODE_ERTM:
		return L2CAP_MODE_ERTM;
	case BT_MODE_STREAMING:
		return L2CAP_MODE_STREAMING;
	case BT_MODE_LE_FLOWCTL:
		return L2CAP_MODE_LE_FLOWCTL;
	case BT_MODE_EXT_FLOWCTL:
		return L2CAP_MODE_FLOWCTL;
	default:
		return mode;
	}
}

static int get_lookup_flag(struct lookup_table *table, char *name)
{
	int i;

	for (i = 0; table[i].name; i++)
		if (!strcasecmp(table[i].name, name))
			return table[i].flag;

	return -1;
}

static const char *get_lookup_str(struct lookup_table *table, int flag)
{
	int i;

	for (i = 0; table[i].name; i++)
		if (table[i].flag == flag)
			return table[i].name;

	return NULL;
}

static void print_lookup_values(struct lookup_table *table, char *header)
{
	int i;

	printf("%s\n", header);

	for (i = 0; table[i].name; i++)
		printf("\t%s\n", table[i].name);
}

static float tv2fl(struct timeval tv)
{
	return (float)tv.tv_sec + (float)(tv.tv_usec/1000000.0);
}

static char *ltoh(unsigned long c, char *s)
{
	int c1;

	c1     = (c >> 28) & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	c1     = (c >> 24) & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	c1     = (c >> 20) & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	c1     = (c >> 16) & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	c1     = (c >> 12) & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	c1     = (c >>  8) & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	c1     = (c >>  4) & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	c1     = c & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	*s     = 0;
	return s;
}

static char *ctoh(char c, char *s)
{
	char c1;

	c1     = (c >> 4) & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	c1     = c & 0x0f;
	*(s++) = NIBBLE_TO_ASCII (c1);
	*s     = 0;
	return s;
}

static void hexdump(unsigned char *s, unsigned long l)
{
	char bfr[80];
	char *pb;
	unsigned long i, n = 0;

	if (l == 0)
		return;

	while (n < l) {
		pb = bfr;
		pb = ltoh (n, pb);
		*(pb++) = ':';
		*(pb++) = ' ';
		for (i = 0; i < 16; i++) {
			if (n + i >= l) {
				*(pb++) = ' ';
				*(pb++) = ' ';
			} else
				pb = ctoh (*(s + i), pb);
			*(pb++) = ' ';
		}
		*(pb++) = ' ';
		for (i = 0; i < 16; i++) {
			if (n + i >= l)
				break;
			else
				*(pb++) = (isprint (*(s + i)) ? *(s + i) : '.');
		}
		*pb = 0;
		n += 16;
		s += 16;
		puts(bfr);
	}
}

static int getopts(int sk, struct l2cap_options *opts, bool connected)
{
	socklen_t optlen;
	int err;

	memset(opts, 0, sizeof(*opts));

	if (bdaddr_type == BDADDR_BREDR || rfcmode) {
		optlen = sizeof(*opts);
		return getsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, opts, &optlen);
	}

	optlen = sizeof(opts->imtu);
	err = getsockopt(sk, SOL_BLUETOOTH, BT_RCVMTU, &opts->imtu, &optlen);
	if (err < 0 || !connected)
		return err;

	optlen = sizeof(opts->omtu);
	return getsockopt(sk, SOL_BLUETOOTH, BT_SNDMTU, &opts->omtu, &optlen);
}

static int setopts(int sk, struct l2cap_options *opts)
{
	if (bdaddr_type == BDADDR_BREDR) {
		opts->mode = bt_mode_to_l2cap_mode(opts->mode);
		return setsockopt(sk, SOL_L2CAP, L2CAP_OPTIONS, opts,
								sizeof(*opts));
	}

	if (opts->mode) {
		if (setsockopt(sk, SOL_BLUETOOTH, BT_MODE, &opts->mode,
						sizeof(opts->mode)) < 0) {
			return -errno;
		}
	}

	return setsockopt(sk, SOL_BLUETOOTH, BT_RCVMTU, &opts->imtu,
							sizeof(opts->imtu));
}

bool use_color(void)
{
	return false;
}

static const struct bitfield_data phy_table[] = {
	{  0, "BR1M1SLOT" },
	{  1, "BR1M3SLOT" },
	{  2, "BR1M5SLOT" },
	{  3, "EDR2M1SLOT" },
	{  4, "EDR2M3SLOT" },
	{  5, "EDR2M5SLOT" },
	{  6, "EDR3M1SLOT" },
	{  7, "EDR3M3SLOT" },
	{  8, "EDR3M5SLOT" },
	{  9, "LE1MTX" },
	{ 10, "LE1MRX" },
	{ 11, "LE2MTX" },
	{ 12, "LE2MRX" },
	{ 13, "LECODEDTX" },
	{ 14, "LECODEDRX" },
	{},
};

static int print_info(int sk, struct l2cap_options *opts)
{
	struct sockaddr_l2 addr;
	socklen_t optlen;
	struct l2cap_conninfo conn;
	int prio, phy;
	char ba[18];

	/* Get connection information */
	memset(&conn, 0, sizeof(conn));
	optlen = sizeof(conn);

	if (getsockopt(sk, SOL_L2CAP, L2CAP_CONNINFO, &conn, &optlen) < 0) {
		syslog(LOG_ERR, "Can't get L2CAP connection information: "
				"%s (%d)", strerror(errno), errno);
		return -errno;
	}

	if (getsockopt(sk, SOL_SOCKET, SO_PRIORITY, &prio, &optlen) < 0) {
		syslog(LOG_ERR, "Can't get socket priority: %s (%d)",
							strerror(errno), errno);
		return -errno;
	}

	/* Check for remote address */
	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);

	if (getpeername(sk, (struct sockaddr *) &addr, &optlen) < 0) {
		syslog(LOG_ERR, "Can't get socket name: %s (%d)",
							strerror(errno), errno);
		return -errno;
	}

	ba2str(&addr.l2_bdaddr, ba);
	syslog(LOG_INFO, "Connected to %s (%s, psm %d, dcid %d)", ba,
		get_lookup_str(bdaddr_types, addr.l2_bdaddr_type),
		addr.l2_psm, addr.l2_cid);

	/* Check for socket address */
	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);

	if (getsockname(sk, (struct sockaddr *) &addr, &optlen) < 0) {
		syslog(LOG_ERR, "Can't get socket name: %s (%d)",
							strerror(errno), errno);
		return -errno;
	}

	ba2str(&addr.l2_bdaddr, ba);
	syslog(LOG_INFO, "Local device %s (%s, psm %d, scid %d)", ba,
		get_lookup_str(bdaddr_types, addr.l2_bdaddr_type),
		addr.l2_psm, addr.l2_cid);

	syslog(LOG_INFO, "Options [imtu %d, omtu %d, flush_to %d, mode %d, "
		"handle %d, class 0x%02x%02x%02x, priority %d, rcvbuf %d]",
		opts->imtu, opts->omtu, opts->flush_to, opts->mode,
		conn.hci_handle, conn.dev_class[2], conn.dev_class[1],
		conn.dev_class[0], prio, rcvbuf);


	if (!getsockopt(sk, SOL_BLUETOOTH, BT_PHY, &phy, &optlen)) {
		syslog(LOG_INFO, "Supported PHY: 0x%08x", phy);
		print_bitfield(2, phy, phy_table);
	}

	return 0;
}

static void dump_mode(int sk)
{
	socklen_t optlen;
	int opt, len;

	if (data_size < 0)
		data_size = imtu;

	if (defer_setup) {
		len = read(sk, buf, data_size);
		if (len < 0)
			syslog(LOG_ERR, "Initial read error: %s (%d)",
						strerror(errno), errno);
		else
			syslog(LOG_INFO, "Initial bytes %d", len);
	}

	syslog(LOG_INFO, "Receiving ...");
	while (1) {
		fd_set rset;

		FD_ZERO(&rset);
		FD_SET(sk, &rset);

		if (select(sk + 1, &rset, NULL, NULL, NULL) < 0)
			return;

		if (!FD_ISSET(sk, &rset))
			continue;

		len = read(sk, buf, data_size);
		if (len <= 0) {
			if (len < 0) {
				if (reliable && (errno == ECOMM)) {
					syslog(LOG_INFO, "L2CAP Error ECOMM - clearing error and continuing.");
					optlen = sizeof(opt);
					if (getsockopt(sk, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0) {
						syslog(LOG_ERR, "Couldn't getsockopt(SO_ERROR): %s (%d)",
							strerror(errno), errno);
						return;
					}
					continue;
				} else {
					syslog(LOG_ERR, "Read error: %s(%d)",
							strerror(errno), errno);
				}
			}
			return;
		}

		syslog(LOG_INFO, "Received %d bytes", len);
		hexdump(buf, len);
	}
}

static void btp_l2cap_read_commands(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	uint16_t commands = 0;

	l_debug("index: %d, length: %d\n", index, length);

	if (index != BTP_INDEX_NON_CONTROLLER) {
		btp_send_error(btp, BTP_L2CAP_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	commands |= (1 << BTP_OP_L2CAP_READ_SUPPORTED_COMMANDS);
	commands |= (1 << BTP_OP_L2CAP_CONNECT);
	commands |= (1 << BTP_OP_L2CAP_DISCONNECT);
	commands |= (1 << BTP_OP_L2CAP_SEND_DATA);
	commands |= (1 << BTP_OP_L2CAP_LISTEN);
	commands |= (1 << BTP_OP_L2CAP_ACCEPT_CONNECTION_REQUEST);
	commands |= (1 << BTP_OP_L2CAP_RECONFIGURE_REQUEST);
	commands |= (1 << BTP_OP_L2CAP_CREDITS);

	commands = L_CPU_TO_LE16(commands);

	btp_send(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_READ_SUPPORTED_COMMANDS,
			BTP_INDEX_NON_CONTROLLER, sizeof(commands), &commands);
}

static void btp_l2cap_disconnect(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	uint8_t status = BTP_ERROR_FAIL;

	if ((socket_l2cap > 0) || (socket_l2cap_accepted > 0)) {
		close(socket_l2cap);
		close(socket_l2cap_accepted);
		socket_l2cap = -1;
		socket_l2cap_accepted = -1;

		btp_send(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_DISCONNECT,
					index, 0, NULL);
		return;
	} else {
		btp_send_error(btp, BTP_L2CAP_SERVICE, index, status);
		return;
	}

}

static void do_send(int sk)
{
	uint32_t seq;
	int i, fd, len, buflen, size, sent;

	syslog(LOG_INFO, "Sending ...");

	if (data_size < 0)
		data_size = omtu;

	for (i = 6; i < data_size; i++)
		buf[i] = 0x7f;

	if (!count && send_delay)
		usleep(send_delay);

	seq = seq_start;
	while ((num_frames == -1) || (num_frames-- > 0)) {
		put_le32(seq, buf);
		put_le16(data_size, buf + 4);

		seq++;

		sent = 0;
		size = data_size;
		while (size > 0) {
			buflen = (size > omtu) ? omtu : size;

			len = send(sk, buf, buflen, 0);
			if (len < 0 || len != buflen) {
				syslog(LOG_ERR, "Send failed: %s (%d)",
							strerror(errno), errno);
				exit(1);
			}

			sent += len;
			size -= len;
		}

		if (num_frames && send_delay && count &&
						!(seq % (count + seq_start)))
			usleep(send_delay);
	}
}

static void btp_l2cap_send_data(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	uint8_t status = BTP_ERROR_FAIL;

	if ((socket_l2cap > 0) || (socket_l2cap_accepted > 0)) {
		if (socket_l2cap > 0) {
			l_info("btp_l2cap_send_data to socket_l2cap");
			do_send(socket_l2cap);
		}

		if (socket_l2cap_accepted > 0) {
			l_info("btp_l2cap_send_data to socket_l2cap_accepted");
			do_send(socket_l2cap_accepted);
		}

		btp_send(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_SEND_DATA,
						index, 0, NULL);
		return;
	} else {
		btp_send_error(btp, BTP_L2CAP_SERVICE, index, status);
		return;
	}
}

static int do_connect(const struct btp_l2cap_connect_cp *cp)
{
	struct sockaddr_l2 addr;
	struct l2cap_options opts;
	socklen_t optlen;
	int sk, opt;

	/* Create socket */
	sk = socket(PF_BLUETOOTH, socktype, BTPROTO_L2CAP);
	if (sk < 0) {
		syslog(LOG_ERR, "Can't create socket: %s (%d)",
							strerror(errno), errno);
		return -1;
	}

	/* Bind to local address */
	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, &bdaddr_local);
	addr.l2_bdaddr_type = cp->address_type;
	if (cid)
		addr.l2_cid = htobs(cid);

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		syslog(LOG_ERR, "Can't bind socket: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Get default options */
	if (getopts(sk, &opts, false) < 0) {
		syslog(LOG_ERR, "Can't get default L2CAP options: %s (%d)",
						strerror(errno), errno);
		goto error;
	}

	/* Set new options */
	opts.omtu = omtu;
	opts.imtu = imtu;
	opts.mode = rfcmode;

	opts.fcs = fcs;
	opts.txwin_size = txwin_size;
	opts.max_tx = max_transmit;

	if (setopts(sk, &opts) < 0) {
		syslog(LOG_ERR, "Can't set L2CAP options: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

#if 0
	/* Enable SO_TIMESTAMP */
	if (timestamp) {
		int t = 1;

		if (setsockopt(sk, SOL_SOCKET, SO_TIMESTAMP, &t, sizeof(t)) < 0) {
			syslog(LOG_ERR, "Can't enable SO_TIMESTAMP: %s (%d)",
							strerror(errno), errno);
			goto error;
		}
	}
#endif

	if (chan_policy != -1) {
		if (setsockopt(sk, SOL_BLUETOOTH, BT_CHANNEL_POLICY,
				&chan_policy, sizeof(chan_policy)) < 0) {
			syslog(LOG_ERR, "Can't enable chan policy : %s (%d)",
							strerror(errno), errno);
			goto error;
		}
	}

	/* Enable SO_LINGER */
	if (linger) {
		struct linger l = { .l_onoff = 1, .l_linger = linger };

		if (setsockopt(sk, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) < 0) {
			syslog(LOG_ERR, "Can't enable SO_LINGER: %s (%d)",
							strerror(errno), errno);
			goto error;
		}
	}

	/* Set link mode */
	opt = 0;
	if (reliable)
		opt |= L2CAP_LM_RELIABLE;
	if (central)
		opt |= L2CAP_LM_MASTER;
	if (auth)
		opt |= L2CAP_LM_AUTH;
	if (encr)
		opt |= L2CAP_LM_ENCRYPT;
	if (secure)
		opt |= L2CAP_LM_SECURE;

	if (setsockopt(sk, SOL_L2CAP, L2CAP_LM, &opt, sizeof(opt)) < 0) {
		syslog(LOG_ERR, "Can't set L2CAP link mode: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Set receive buffer size */
	if (rcvbuf && setsockopt(sk, SOL_SOCKET, SO_RCVBUF,
						&rcvbuf, sizeof(rcvbuf)) < 0) {
		syslog(LOG_ERR, "Can't set socket rcv buf size: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	optlen = sizeof(rcvbuf);
	if (getsockopt(sk, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen) < 0) {
		syslog(LOG_ERR, "Can't get socket rcv buf size: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Connect to remote device */
	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, &cp->address);
	addr.l2_bdaddr_type = cp->address_type;

	if (cid)
		addr.l2_cid = htobs(cid);
	else if (cp->psm)
		addr.l2_psm = htobs(cp->psm);
	else
		goto error;

	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0 ) {
		syslog(LOG_ERR, "Can't connect: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Get current options */
	if (getopts(sk, &opts, true) < 0) {
		syslog(LOG_ERR, "Can't get L2CAP options: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	if (print_info(sk, &opts) < 0)
		goto error;

	omtu = (opts.omtu > buffer_size) ? buffer_size : opts.omtu;
	imtu = (opts.imtu > buffer_size) ? buffer_size : opts.imtu;

	return sk;

error:
	close(sk);
	return -1;
}

static void btp_l2cap_connect(uint8_t index, const void *param, uint16_t length,
								void *user_data)
{
	uint8_t status = BTP_ERROR_FAIL;

	const struct btp_l2cap_connect_cp *cp = param;

	int socket_l2cap = do_connect(cp);
	l_info("btp_l2cap_connect: connected, socket: %d\n", socket_l2cap);

	if (socket_l2cap > 0) {
		btp_send(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_CONNECT, index, 0, NULL);
		return;
	} else {
		btp_send_error(btp, BTP_L2CAP_SERVICE, index, status);
		return;
	}
}

static void do_listen(void (*handler)(int sk))
{
	struct sockaddr_l2 addr;
	struct l2cap_options opts;
	socklen_t optlen;
	int sk, nsk, opt;

	/* Create socket */
	sk = socket(PF_BLUETOOTH, socktype, BTPROTO_L2CAP);
	if (sk < 0) {
		syslog(LOG_ERR, "Can't create socket: %s (%d)",
							strerror(errno), errno);
		exit(1);
	}

	/* Bind to local address */
	memset(&addr, 0, sizeof(addr));
	addr.l2_family = AF_BLUETOOTH;
	bacpy(&addr.l2_bdaddr, &bdaddr_local);
	addr.l2_bdaddr_type = bdaddr_type;
	if (cid)
		addr.l2_cid = htobs(cid);
	else if (psm)
		addr.l2_psm = htobs(psm);

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		syslog(LOG_ERR, "Can't bind socket: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Set link mode */
	opt = 0;
	if (reliable)
		opt |= L2CAP_LM_RELIABLE;
	if (central)
		opt |= L2CAP_LM_MASTER;
	if (auth)
		opt |= L2CAP_LM_AUTH;
	if (encr)
		opt |= L2CAP_LM_ENCRYPT;
	if (secure)
		opt |= L2CAP_LM_SECURE;

	if (opt && setsockopt(sk, SOL_L2CAP, L2CAP_LM, &opt, sizeof(opt)) < 0) {
		syslog(LOG_ERR, "Can't set L2CAP link mode: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Get default options */
	if (getopts(sk, &opts, false) < 0) {
		syslog(LOG_ERR, "Can't get default L2CAP options: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Set new options */
	opts.omtu = omtu;
	opts.imtu = imtu;
	if (rfcmode > 0)
		opts.mode = rfcmode;

	opts.fcs = fcs;
	opts.txwin_size = txwin_size;
	opts.max_tx = max_transmit;

	if (setopts(sk, &opts) < 0) {
		syslog(LOG_ERR, "Can't set L2CAP options: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	if (socktype == SOCK_DGRAM) {
		handler(sk);
		close(sk);
		return;
	}

	/* Enable deferred setup */
	opt = defer_setup;

	if (opt && setsockopt(sk, SOL_BLUETOOTH, BT_DEFER_SETUP,
						&opt, sizeof(opt)) < 0) {
		syslog(LOG_ERR, "Can't enable deferred setup : %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Listen for connections */
	if (listen(sk, 10)) {
		syslog(LOG_ERR, "Can not listen on the socket: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	/* Check for socket address */
	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);

	if (getsockname(sk, (struct sockaddr *) &addr, &optlen) < 0) {
		syslog(LOG_ERR, "Can't get socket name: %s (%d)",
							strerror(errno), errno);
		goto error;
	}

	psm = btohs(addr.l2_psm);
	cid = btohs(addr.l2_cid);

	syslog(LOG_INFO, "Waiting for connection on psm %d ...", psm);

	while (1) {
		memset(&addr, 0, sizeof(addr));
		optlen = sizeof(addr);

		nsk = accept(sk, (struct sockaddr *) &addr, &optlen);
		if (nsk < 0) {
			syslog(LOG_ERR, "Accept failed: %s (%d)",
							strerror(errno), errno);
			goto error;
		}

		/* Set receive buffer size */
		if (rcvbuf && setsockopt(nsk, SOL_SOCKET, SO_RCVBUF, &rcvbuf,
							sizeof(rcvbuf)) < 0) {
			syslog(LOG_ERR, "Can't set rcv buf size: %s (%d)",
							strerror(errno), errno);
			goto error;
		}

		optlen = sizeof(rcvbuf);
		if (getsockopt(nsk, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen)
									< 0) {
			syslog(LOG_ERR, "Can't get rcv buf size: %s (%d)",
							strerror(errno), errno);
			goto error;
		}

		/* Get current options */
		if (getopts(nsk, &opts, true) < 0) {
			syslog(LOG_ERR, "Can't get L2CAP options: %s (%d)",
							strerror(errno), errno);
			if (!defer_setup) {
				close(nsk);
				goto error;
			}
		}

		if (print_info(nsk, &opts) < 0) {
			close(nsk);
			goto error;
		}

		omtu = (opts.omtu > buffer_size) ? buffer_size : opts.omtu;
		imtu = (opts.imtu > buffer_size) ? buffer_size : opts.imtu;

#if 0
		/* Enable SO_TIMESTAMP */
		if (timestamp) {
			int t = 1;

			if (setsockopt(nsk, SOL_SOCKET, SO_TIMESTAMP, &t, sizeof(t)) < 0) {
				syslog(LOG_ERR, "Can't enable SO_TIMESTAMP: %s (%d)",
							strerror(errno), errno);
				goto error;
			}
		}
#endif

		/* Enable SO_LINGER */
		if (linger) {
			struct linger l = { .l_onoff = 1, .l_linger = linger };

			if (setsockopt(nsk, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) < 0) {
				syslog(LOG_ERR, "Can't enable SO_LINGER: %s (%d)",
							strerror(errno), errno);
				close(nsk);
				goto error;
			}
		}

		/* Handle deferred setup */
		if (defer_setup) {
			syslog(LOG_INFO, "Waiting for %d seconds",
							abs(defer_setup) - 1);
			sleep(abs(defer_setup) - 1);

			if (defer_setup < 0) {
				close(nsk);
				goto error;
			}
		}

		handler(nsk);
		socket_l2cap_accepted = nsk;
		close(sk);

		syslog(LOG_INFO, "Disconnect: %m");
		break;
	}

error:
	close(sk);
	return;
}

static void btp_l2cap_listen(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	const struct btp_l2cap_listen_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;

	psm = cp->psm;

	if (fork()) {
		/* Parent */
		btp_send(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_LISTEN,
						index, 0, NULL);
		return;
	}

	/* Child */
	do_listen(dump_mode);
	return;

failed:
	btp_send_error(btp, BTP_L2CAP_SERVICE, index, status);
}

static void btp_l2cap_reconfigure_request(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	const struct btp_l2cap_reconfigure_request_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;

	struct l2cap_options opts;

	/* Get default options */
	if (getopts(socket_l2cap_accepted, &opts, false) < 0) {
		syslog(LOG_ERR, "btp_l2cap_reconfigure_request, can't get default L2CAP options: %s (%d)",
						strerror(errno), errno);
		goto failed;
	}

	opts.imtu = cp->mtu;
	opts.omtu = cp->mtu;

	if (setopts(socket_l2cap_accepted, &opts) < 0) {
		syslog(LOG_ERR, "btp_l2cap_reconfigure_request, can't set L2CAP options: %s (%d)",
							strerror(errno), errno);
		goto failed;
	}

	btp_send(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_RECONFIGURE_REQUEST,
					index, 0, NULL);
	return;

failed:
	btp_send_error(btp, BTP_L2CAP_SERVICE, index, status);
}

static void register_l2cap_service(void)
{
	btp_register(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_READ_SUPPORTED_COMMANDS,
					btp_l2cap_read_commands, NULL, NULL);

	btp_register(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_CONNECT,
						btp_l2cap_connect, NULL, NULL);

	btp_register(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_DISCONNECT,
						btp_l2cap_disconnect, NULL, NULL);

	btp_register(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_SEND_DATA,
						btp_l2cap_send_data, NULL, NULL);

	btp_register(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_LISTEN,
						btp_l2cap_listen, NULL, NULL);

	btp_register(btp, BTP_L2CAP_SERVICE, BTP_OP_L2CAP_RECONFIGURE_REQUEST,
						btp_l2cap_reconfigure_request, NULL, NULL);
}

static bool match_dev_addr_type(const char *addr_type_str, uint8_t addr_type)
{
	if (addr_type == BTP_GAP_ADDR_PUBLIC && strcmp(addr_type_str, "public"))
		return false;

	if (addr_type == BTP_GAP_ADDR_RANDOM && strcmp(addr_type_str, "random"))
		return false;

	return true;
}

static struct btp_adapter *find_adapter_by_proxy(struct l_dbus_proxy *proxy)
{
	const struct l_queue_entry *entry;

	for (entry = l_queue_get_entries(adapters); entry;
							entry = entry->next) {
		struct btp_adapter *adapter = entry->data;

		if (adapter->proxy == proxy)
			return adapter;
	}

	return NULL;
}

static struct btp_adapter *find_adapter_by_index(uint8_t index)
{
	const struct l_queue_entry *entry;

	for (entry = l_queue_get_entries(adapters); entry;
							entry = entry->next) {
		struct btp_adapter *adapter = entry->data;

		if (adapter->index == index)
			return adapter;
	}

	return NULL;
}

static struct btp_adapter *find_adapter_by_path(const char *path)
{
	const struct l_queue_entry *entry;

	for (entry = l_queue_get_entries(adapters); entry;
							entry = entry->next) {
		struct btp_adapter *adapter = entry->data;

		if (!strcmp(l_dbus_proxy_get_path(adapter->proxy), path))
			return adapter;
	}

	return NULL;
}

static struct btp_device *find_device_by_address(struct btp_adapter *adapter,
							const bdaddr_t *addr,
							uint8_t addr_type)
{
	const struct l_queue_entry *entry;
	const char *str;
	char addr_str[18];

	if (!ba2str(addr, addr_str))
		return NULL;

	for (entry = l_queue_get_entries(adapter->devices); entry;
							entry = entry->next) {
		struct btp_device *device = entry->data;

		l_dbus_proxy_get_property(device->proxy, "Address", "s", &str);
		if (strcmp(str, addr_str))
			continue;

		l_dbus_proxy_get_property(device->proxy, "AddressType", "s",
									&str);
		if (match_dev_addr_type(str, addr_type))
			return device;
	}

	return NULL;
}

static bool match_device_paths(const void *device, const void *path)
{
	const struct btp_device *dev = device;

	return !strcmp(l_dbus_proxy_get_path(dev->proxy), path);
}

static struct btp_device *find_device_by_path(const char *path)
{
	const struct l_queue_entry *entry;
	struct btp_device *device;

	for (entry = l_queue_get_entries(adapters); entry;
							entry = entry->next) {
		struct btp_adapter *adapter = entry->data;

		device = l_queue_find(adapter->devices, match_device_paths,
									path);
		if (device)
			return device;
	}

	return NULL;
}

static bool match_adapter_dev_proxy(const void *device, const void *proxy)
{
	const struct btp_device *d = device;

	return d->proxy == proxy;
}

static bool match_adapter_dev(const void *device_a, const void *device_b)
{
	return device_a == device_b;
}

static struct btp_adapter *find_adapter_by_device(struct btp_device *device)
{
	const struct l_queue_entry *entry;

	for (entry = l_queue_get_entries(adapters); entry;
							entry = entry->next) {
		struct btp_adapter *adapter = entry->data;

		if (l_queue_find(adapter->devices, match_adapter_dev, device))
			return adapter;
	}

	return NULL;
}

static struct btp_device *find_device_by_proxy(struct l_dbus_proxy *proxy)
{
	const struct l_queue_entry *entry;
	struct btp_device *device;

	for (entry = l_queue_get_entries(adapters); entry;
							entry = entry->next) {
		struct btp_adapter *adapter = entry->data;

		device = l_queue_find(adapter->devices, match_adapter_dev_proxy,
									proxy);

		if (device)
			return device;
	}

	return NULL;
}

static void btp_gap_read_commands(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	uint16_t commands = 0;

	l_debug("index: %d, length: %d\n", index, length);

	if (index != BTP_INDEX_NON_CONTROLLER) {
		btp_send_error(btp, BTP_GAP_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	commands |= (1 << BTP_OP_GAP_READ_SUPPORTED_COMMANDS);
	commands |= (1 << BTP_OP_GAP_READ_CONTROLLER_INDEX_LIST);
	commands |= (1 << BTP_OP_GAP_READ_COTROLLER_INFO);
	commands |= (1 << BTP_OP_GAP_RESET);
	commands |= (1 << BTP_OP_GAP_SET_POWERED);
	commands |= (1 << BTP_OP_GAP_SET_CONNECTABLE);
	commands |= (1 << BTP_OP_GAP_SET_DISCOVERABLE);
	commands |= (1 << BTP_OP_GAP_SET_BONDABLE);
	commands |= (1 << BTP_OP_GAP_START_ADVERTISING);
	commands |= (1 << BTP_OP_GAP_STOP_ADVERTISING);
	commands |= (1 << BTP_OP_GAP_START_DISCOVERY);
	commands |= (1 << BTP_OP_GAP_STOP_DISCOVERY);
	commands |= (1 << BTP_OP_GAP_CONNECT);
	commands |= (1 << BTP_OP_GAP_DISCONNECT);
	commands |= (1 << BTP_OP_GAP_SET_IO_CAPA);
	commands |= (1 << BTP_OP_GAP_PAIR);
	commands |= (1 << BTP_OP_GAP_UNPAIR);

	commands = L_CPU_TO_LE16(commands);

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_READ_SUPPORTED_COMMANDS,
			BTP_INDEX_NON_CONTROLLER, sizeof(commands), &commands);
}

static void btp_gap_read_controller_index(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	const struct l_queue_entry *entry;
	struct btp_gap_read_index_rp *rp;
	uint8_t cnt;
	int i;

	l_debug("index: %d, length: %d\n", index, length);

	if (index != BTP_INDEX_NON_CONTROLLER) {
		btp_send_error(btp, BTP_GAP_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	cnt = l_queue_length(adapters);

	rp = l_malloc(sizeof(*rp) + cnt);

	rp->num = cnt;

	for (i = 0, entry = l_queue_get_entries(adapters); entry;
						i++, entry = entry->next) {
		struct btp_adapter *adapter = entry->data;

		rp->indexes[i] = adapter->index;
	}

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_READ_CONTROLLER_INDEX_LIST,
			BTP_INDEX_NON_CONTROLLER, sizeof(*rp) + cnt, rp);
}

static void btp_gap_read_info(uint8_t index, const void *param, uint16_t length,
								void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	struct btp_gap_read_info_rp rp;
	const char *str;
	uint8_t status = BTP_ERROR_FAIL;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	memset(&rp, 0, sizeof(rp));

	if (!l_dbus_proxy_get_property(adapter->proxy, "Address", "s", &str))
		goto failed;

	if (str2ba(str, &rp.address) < 0)
		goto failed;

	if (!l_dbus_proxy_get_property(adapter->proxy, "Name", "s", &str)) {
		goto failed;
	}

	snprintf((char *)rp.name, sizeof(rp.name), "%s", str);
	snprintf((char *)rp.short_name, sizeof(rp.short_name), "%s", str);
	rp.supported_settings = L_CPU_TO_LE32(adapter->supported_settings);
	rp.current_settings = L_CPU_TO_LE32(adapter->current_settings);

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_READ_COTROLLER_INFO, index,
							sizeof(rp), &rp);

	return;
failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void remove_device_setup(struct l_dbus_message *message,
							void *user_data)
{
	struct btp_device *device = user_data;

	l_dbus_message_set_arguments(message, "o",
					l_dbus_proxy_get_path(device->proxy));
}

static void remove_device_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct btp_device *device = user_data;
	struct btp_adapter *adapter = find_adapter_by_proxy(proxy);

	if (!adapter)
		return;

	if (l_dbus_message_is_error(result)) {
		const char *name;

		l_dbus_message_get_error(result, &name, NULL);

		l_error("Failed to remove device %s (%s)",
					l_dbus_proxy_get_path(device->proxy),
					name);
		return;
	}

	l_queue_remove(adapter->devices, device);
}

static void unreg_advertising_setup(struct l_dbus_message *message,
								void *user_data)
{
	struct l_dbus_message_builder *builder;

	builder = l_dbus_message_builder_new(message);
	l_dbus_message_builder_append_basic(builder, 'o', AD_PATH);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void ad_cleanup_service(void *service)
{
	struct service_data *s = service;

	l_free(s->uuid);
	l_free(s);
}

static void ad_cleanup(void)
{
	l_free(ad.local_name);
	l_queue_destroy(ad.uuids, l_free);
	l_queue_destroy(ad.services, ad_cleanup_service);
	l_queue_destroy(ad.manufacturers, l_free);
	l_queue_destroy(ad.solicits, l_free);

	memset(&ad, 0, sizeof(ad));
}

static void unreg_advertising_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	const char *path = l_dbus_proxy_get_path(proxy);
	struct btp_adapter *adapter = find_adapter_by_path(path);

	if (!adapter)
		return;

	if (l_dbus_message_is_error(result)) {
		const char *name;

		l_dbus_message_get_error(result, &name, NULL);

		l_error("Failed to stop advertising %s (%s)",
					l_dbus_proxy_get_path(proxy), name);
		return;
	}

	if (!l_dbus_object_remove_interface(dbus, AD_PATH, AD_IFACE))
		l_info("Unable to remove ad instance");
	if (!l_dbus_object_remove_interface(dbus, AD_PATH,
						L_DBUS_INTERFACE_PROPERTIES))
		l_info("Unable to remove propety instance");
	if (!l_dbus_unregister_interface(dbus, AD_IFACE))
		l_info("Unable to unregister ad interface");

	ad_cleanup();
}

static void unreg_agent_setup(struct l_dbus_message *message, void *user_data)
{
	struct l_dbus_message_builder *builder;

	builder = l_dbus_message_builder_new(message);

	l_dbus_message_builder_append_basic(builder, 'o', AG_PATH);

	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void reset_unreg_agent_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	if (l_dbus_message_is_error(result)) {
		const char *name;

		l_dbus_message_get_error(result, &name, NULL);

		l_error("Failed to unregister agent %s (%s)",
					l_dbus_proxy_get_path(proxy), name);
		return;
	}

	if (!l_dbus_object_remove_interface(dbus, AG_PATH,
						L_DBUS_INTERFACE_PROPERTIES))
		l_info("Unable to remove propety instance");
	if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
		l_info("Unable to remove agent instance");
	if (!l_dbus_unregister_interface(dbus, AG_IFACE))
		l_info("Unable to unregister agent interface");

	ag.registered = false;
}

static void update_current_settings(struct btp_adapter *adapter,
							uint32_t new_settings)
{
	struct btp_new_settings_ev ev;

	adapter->current_settings = new_settings;

	ev.current_settings = L_CPU_TO_LE32(adapter->current_settings);

	btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_NEW_SETTINGS, adapter->index,
							sizeof(ev), &ev);
}

static void btp_gap_reset(uint8_t index, const void *param, uint16_t length,
								void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct l_queue_entry *entry;
	uint8_t status;
	bool prop;
	uint32_t default_settings;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to remove devices */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
									!prop) {
		status = BTP_ERROR_FAIL;
		goto failed;
	}

	for (entry = l_queue_get_entries(adapter->devices); entry;
							entry = entry->next) {
		struct btp_device *device = entry->data;

		l_dbus_proxy_method_call(adapter->proxy, "RemoveDevice",
						remove_device_setup,
						remove_device_reply, device,
						NULL);
	}

	if (adapter->ad_proxy && ad.registered)
		if (!l_dbus_proxy_method_call(adapter->ad_proxy,
						"UnregisterAdvertisement",
						unreg_advertising_setup,
						unreg_advertising_reply,
						NULL, NULL)) {
			status = BTP_ERROR_FAIL;
			goto failed;
		}

	if (ag.proxy && ag.registered)
		if (!l_dbus_proxy_method_call(ag.proxy, "UnregisterAgent",
						unreg_agent_setup,
						reset_unreg_agent_reply,
						NULL, NULL)) {
			status = BTP_ERROR_FAIL;
			goto failed;
		}

	default_settings = adapter->default_settings;

	update_current_settings(adapter, default_settings);

	/* TODO for we assume all went well */
	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_RESET, index,
				sizeof(default_settings), &default_settings);
	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

struct set_setting_data {
	struct btp_adapter *adapter;
	uint8_t opcode;
	uint32_t setting;
	bool value;
};

static void set_setting_reply(struct l_dbus_proxy *proxy,
				struct l_dbus_message *result, void *user_data)
{
	struct set_setting_data *data = user_data;
	struct btp_adapter *adapter = data->adapter;
	uint32_t settings;

	if (l_dbus_message_is_error(result)) {
		btp_send_error(btp, BTP_GAP_SERVICE, data->adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	if (data->value)
		adapter->current_settings |= data->setting;
	else
		adapter->current_settings &= ~data->setting;

	settings = L_CPU_TO_LE32(adapter->current_settings);

	btp_send(btp, BTP_GAP_SERVICE, data->opcode, adapter->index,
						sizeof(settings), &settings);
}

static void btp_gap_set_powered(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_set_powered_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	struct set_setting_data *data;

	l_debug("index: %d, length: %d\n", index, length);

	if (length < sizeof(*cp))
		goto failed;

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	data = l_new(struct set_setting_data, 1);
	data->adapter = adapter;
	data->opcode = BTP_OP_GAP_SET_POWERED;
	data->setting = BTP_GAP_SETTING_POWERED;
	data->value = cp->powered;

	if (l_dbus_proxy_set_property(adapter->proxy, set_setting_reply,
					data, l_free, "Powered", "b",
					data->value))
		return;

	l_free(data);

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void btp_gap_set_connectable(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_set_connectable_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	uint32_t new_settings;

	l_debug("index: %d, length: %d\n", index, length);

	if (length < sizeof(*cp))
		goto failed;

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	new_settings = adapter->current_settings;

	if (cp->connectable)
		new_settings |= BTP_GAP_SETTING_CONNECTABLE;
	else
		new_settings &= ~BTP_GAP_SETTING_CONNECTABLE;

	update_current_settings(adapter, new_settings);

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_SET_CONNECTABLE, index,
					sizeof(new_settings), &new_settings);

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void btp_gap_set_discoverable(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_set_discoverable_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	struct set_setting_data *data;

	l_debug("index: %d, length: %d\n", index, length);

	if (length < sizeof(*cp))
		goto failed;

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	data = l_new(struct set_setting_data, 1);
	data->adapter = adapter;
	data->opcode = BTP_OP_GAP_SET_DISCOVERABLE;
	data->setting = BTP_GAP_SETTING_DISCOVERABLE;
	data->value = cp->discoverable;

	if (l_dbus_proxy_set_property(adapter->proxy, set_setting_reply,
					data, l_free, "Discoverable", "b",
					data->value))
		return;

	l_free(data);

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void btp_gap_set_bondable(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_set_bondable_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	struct set_setting_data *data;

	l_debug("index: %d, length: %d\n", index, length);

	if (length < sizeof(*cp))
		goto failed;

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	data = l_new(struct set_setting_data, 1);
	data->adapter = adapter;
	data->opcode = BTP_OP_GAP_SET_BONDABLE;
	data->setting = BTP_GAP_SETTING_BONDABLE;
	data->value = cp->bondable;

	if (l_dbus_proxy_set_property(adapter->proxy, set_setting_reply,
					data, l_free, "Pairable", "b",
					data->value))
		return;

	l_free(data);

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void ad_init(void)
{
	ad.uuids = l_queue_new();
	ad.services = l_queue_new();
	ad.manufacturers = l_queue_new();
	ad.solicits = l_queue_new();

	ad.local_appearance = UINT16_MAX;
}

static struct l_dbus_message *ad_release_call(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct l_dbus_message *reply;

	l_dbus_unregister_object(dbus, AD_PATH);
	l_dbus_unregister_interface(dbus, AD_IFACE);

	reply = l_dbus_message_new_method_return(message);
	l_dbus_message_set_arguments(reply, "");

	ad_cleanup();

	return reply;
}

static bool ad_type_getter(struct l_dbus *dbus, struct l_dbus_message *message,
				struct l_dbus_message_builder *builder,
				void *user_data)
{
	l_dbus_message_builder_append_basic(builder, 's', ad.type);

	return true;
}

static bool ad_serviceuuids_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	const struct l_queue_entry *entry;

	if (l_queue_isempty(ad.uuids))
		return false;

	l_dbus_message_builder_enter_array(builder, "s");

	for (entry = l_queue_get_entries(ad.uuids); entry; entry = entry->next)
		l_dbus_message_builder_append_basic(builder, 's', entry->data);

	l_dbus_message_builder_leave_array(builder);

	return true;
}

static bool ad_servicedata_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	const struct l_queue_entry *entry;
	size_t i;

	if (l_queue_isempty(ad.services))
		return false;

	l_dbus_message_builder_enter_array(builder, "{sv}");

	for (entry = l_queue_get_entries(ad.services); entry;
							entry = entry->next) {
		struct service_data *sd = entry->data;

		l_dbus_message_builder_enter_dict(builder, "sv");
		l_dbus_message_builder_append_basic(builder, 's', sd->uuid);
		l_dbus_message_builder_enter_variant(builder, "ay");
		l_dbus_message_builder_enter_array(builder, "y");

		for (i = 0; i < sd->data.len; i++)
			l_dbus_message_builder_append_basic(builder, 'y',
							&(sd->data.data[i]));

		l_dbus_message_builder_leave_array(builder);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);
	}
	l_dbus_message_builder_leave_array(builder);

	return true;
}

static bool ad_manufacturerdata_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	const struct l_queue_entry *entry;
	size_t i;

	if (l_queue_isempty(ad.manufacturers))
		return false;

	l_dbus_message_builder_enter_array(builder, "{qv}");

	for (entry = l_queue_get_entries(ad.manufacturers); entry;
							entry = entry->next) {
		struct manufacturer_data *md = entry->data;

		l_dbus_message_builder_enter_dict(builder, "qv");
		l_dbus_message_builder_append_basic(builder, 'q', &md->id);
		l_dbus_message_builder_enter_variant(builder, "ay");
		l_dbus_message_builder_enter_array(builder, "y");

		for (i = 0; i < md->data.len; i++)
			l_dbus_message_builder_append_basic(builder, 'y',
							&(md->data.data[i]));

		l_dbus_message_builder_leave_array(builder);
		l_dbus_message_builder_leave_variant(builder);
		l_dbus_message_builder_leave_dict(builder);
	}
	l_dbus_message_builder_leave_array(builder);

	return true;
}

static bool ad_solicituuids_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	const struct l_queue_entry *entry;

	if (l_queue_isempty(ad.solicits))
		return false;

	l_dbus_message_builder_enter_array(builder, "s");

	for (entry = l_queue_get_entries(ad.solicits); entry;
							entry = entry->next)
		l_dbus_message_builder_append_basic(builder, 's', entry->data);

	l_dbus_message_builder_leave_array(builder);

	return true;
}

static bool ad_includes_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	l_dbus_message_builder_enter_array(builder, "s");

	if (!(ad.tx_power || ad.name || ad.appearance))
		return false;

	if (ad.tx_power) {
		const char *str = "tx-power";

		l_dbus_message_builder_append_basic(builder, 's', str);
	}

	if (ad.name) {
		const char *str = "local-name";

		l_dbus_message_builder_append_basic(builder, 's', str);
	}

	if (ad.appearance) {
		const char *str = "appearance";

		l_dbus_message_builder_append_basic(builder, 's', str);
	}

	l_dbus_message_builder_leave_array(builder);

	return true;
}

static bool ad_localname_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	if (!ad.local_name)
		return false;

	l_dbus_message_builder_append_basic(builder, 's', ad.local_name);

	return true;
}

static bool ad_appearance_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	if (!ad.local_appearance)
		return false;

	l_dbus_message_builder_append_basic(builder, 'q', &ad.local_appearance);

	return true;
}

static bool ad_duration_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	if (!ad.duration)
		return false;

	l_dbus_message_builder_append_basic(builder, 'q', &ad.duration);

	return true;
}

static bool ad_timeout_getter(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	if (!ad.timeout)
		return false;

	l_dbus_message_builder_append_basic(builder, 'q', &ad.timeout);

	return true;
}

static void setup_ad_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_method(interface, "Release",
						L_DBUS_METHOD_FLAG_NOREPLY,
						ad_release_call, "", "");
	l_dbus_interface_property(interface, "Type", 0, "s", ad_type_getter,
									NULL);
	l_dbus_interface_property(interface, "ServiceUUIDs", 0, "as",
						ad_serviceuuids_getter, NULL);
	l_dbus_interface_property(interface, "ServiceData", 0, "a{sv}",
						ad_servicedata_getter, NULL);
	l_dbus_interface_property(interface, "ManufacturerData", 0,
					"a{qv}", ad_manufacturerdata_getter,
					NULL);
	l_dbus_interface_property(interface, "SolicitUUIDs", 0, "as",
						ad_solicituuids_getter, NULL);
	l_dbus_interface_property(interface, "Includes", 0, "as",
						ad_includes_getter, NULL);
	l_dbus_interface_property(interface, "LocalName", 0, "s",
						ad_localname_getter, NULL);
	l_dbus_interface_property(interface, "Appearance", 0, "q",
						ad_appearance_getter, NULL);
	l_dbus_interface_property(interface, "Duration", 0, "q",
						ad_duration_getter, NULL);
	l_dbus_interface_property(interface, "Timeout", 0, "q",
						ad_timeout_getter, NULL);
}

static void start_advertising_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	const char *path = l_dbus_proxy_get_path(proxy);
	struct btp_adapter *adapter = find_adapter_by_path(path);
	uint32_t new_settings;

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to start advertising (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	new_settings = adapter->current_settings;
	new_settings |= BTP_GAP_SETTING_ADVERTISING;
	update_current_settings(adapter, new_settings);

	ad.registered = true;

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_START_ADVERTISING,
					adapter->index, sizeof(new_settings),
					&new_settings);
}

static void create_advertising_data(uint8_t adv_data_len, const uint8_t *data)
{
	const uint8_t *ad_data;
	uint8_t ad_type, ad_len;
	uint8_t remaining_data_len = adv_data_len;

	while (remaining_data_len) {
		ad_type = data[adv_data_len - remaining_data_len];
		ad_len = data[adv_data_len - remaining_data_len + 1];
		ad_data = &data[adv_data_len - remaining_data_len + 2];

		switch (ad_type) {
		case AD_TYPE_INCOMPLETE_UUID16_SERVICE_LIST:
		{
			char *uuid = dupuuid2str(ad_data, 16);

			l_queue_push_tail(ad.uuids, uuid);

			break;
		}
		case AD_TYPE_SHORT_NAME:
			ad.local_name = malloc(ad_len + 1);
			memcpy(ad.local_name, ad_data, ad_len);
			ad.local_name[ad_len] = '\0';

			break;
		case AD_TYPE_TX_POWER:
			ad.tx_power = true;

			/* XXX Value is ommited cause, stack fills it */

			break;
		case AD_TYPE_SERVICE_DATA_UUID16:
		{
			struct service_data *sd;

			sd = l_new(struct service_data, 1);
			sd->uuid = dupuuid2str(ad_data, 16);
			sd->data.len = ad_len - 2;
			memcpy(sd->data.data, ad_data + 2, sd->data.len);

			l_queue_push_tail(ad.services, sd);

			break;
		}
		case AD_TYPE_APPEARANCE:
			memcpy(&ad.local_appearance, ad_data, ad_len);

			break;
		case AD_TYPE_MANUFACTURER_DATA:
		{
			struct manufacturer_data *md;

			md = l_new(struct manufacturer_data, 1);
			/* The first 2 octets contain the Company Identifier
			 * Code followed by additional manufacturer specific
			 * data.
			 */
			memcpy(&md->id, ad_data, 2);
			md->data.len = ad_len - 2;
			memcpy(md->data.data, ad_data + 2, md->data.len);

			l_queue_push_tail(ad.manufacturers, md);

			break;
		}
		case AD_TYPE_SOLICIT_UUID16_SERVICE_LIST:
		{
			char *uuid = dupuuid2str(ad_data, 16);

			l_queue_push_tail(ad.solicits, uuid);

			break;
		}
		default:
			l_info("Unsupported advertising data type");

			break;
		}
		/* Advertising entity data len + advertising entity header
		 * (type, len)
		 */
		remaining_data_len -= ad_len + 2;
	}
}

static void create_scan_response(uint8_t scan_rsp_len, const uint8_t *data)
{
	/* TODO */
}

static void start_advertising_setup(struct l_dbus_message *message,
							void *user_data)
{
	struct l_dbus_message_builder *builder;

	builder = l_dbus_message_builder_new(message);
	l_dbus_message_builder_append_basic(builder, 'o', AD_PATH);
	l_dbus_message_builder_enter_array(builder, "{sv}");
	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_leave_dict(builder);
	l_dbus_message_builder_leave_array(builder);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void btp_gap_start_advertising(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_start_adv_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to advertise */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
							!prop || ad.registered)
		goto failed;

	if (!l_dbus_register_interface(dbus, AD_IFACE, setup_ad_interface, NULL,
								false)) {
		l_info("Unable to register ad interface");
		goto failed;
	}

	if (!l_dbus_object_add_interface(dbus, AD_PATH, AD_IFACE, NULL)) {
		l_info("Unable to instantiate ad interface");

		if (!l_dbus_unregister_interface(dbus, AD_IFACE))
			l_info("Unable to unregister ad interface");

		goto failed;
	}

	if (!l_dbus_object_add_interface(dbus, AD_PATH,
						L_DBUS_INTERFACE_PROPERTIES,
						NULL)) {
		l_info("Unable to instantiate the properties interface");

		if (!l_dbus_object_remove_interface(dbus, AD_PATH, AD_IFACE))
			l_info("Unable to remove ad instance");
		if (!l_dbus_unregister_interface(dbus, AD_IFACE))
			l_info("Unable to unregister ad interface");

		goto failed;
	}

	ad_init();

	if (adapter->current_settings & BTP_GAP_SETTING_CONNECTABLE)
		ad.type = "peripheral";
	else
		ad.type = "broadcast";

	if (cp->adv_data_len > 0)
		create_advertising_data(cp->adv_data_len, cp->data);
	if (cp->scan_rsp_len > 0)
		create_scan_response(cp->scan_rsp_len,
						cp->data + cp->scan_rsp_len);

	if (!l_dbus_proxy_method_call(adapter->ad_proxy,
							"RegisterAdvertisement",
							start_advertising_setup,
							start_advertising_reply,
							NULL, NULL)) {
		if (!l_dbus_object_remove_interface(dbus, AD_PATH, AD_IFACE))
			l_info("Unable to remove ad instance");
		if (!l_dbus_unregister_interface(dbus, AD_IFACE))
			l_info("Unable to unregister ad interface");

		goto failed;
	}

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void stop_advertising_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	const char *path = l_dbus_proxy_get_path(proxy);
	struct btp_adapter *adapter = find_adapter_by_path(path);
	uint32_t new_settings;

	if (!adapter)
		return;

	if (l_dbus_message_is_error(result)) {
		const char *name;

		l_dbus_message_get_error(result, &name, NULL);

		l_error("Failed to stop advertising %s (%s)",
					l_dbus_proxy_get_path(proxy), name);
		return;
	}

	if (!l_dbus_object_remove_interface(dbus, AD_PATH, AD_IFACE))
		l_info("Unable to remove ad instance");
	if (!l_dbus_object_remove_interface(dbus, AD_PATH,
						L_DBUS_INTERFACE_PROPERTIES))
		l_info("Unable to remove propety instance");
	if (!l_dbus_unregister_interface(dbus, AD_IFACE))
		l_info("Unable to unregister ad interface");

	new_settings = adapter->current_settings;
	new_settings &= ~BTP_GAP_SETTING_ADVERTISING;
	update_current_settings(adapter, new_settings);

	ad_cleanup();

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_STOP_ADVERTISING,
					adapter->index, sizeof(new_settings),
					&new_settings);
}

static void btp_gap_stop_advertising(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	uint8_t status = BTP_ERROR_FAIL;
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
				!prop || !adapter->ad_proxy || !ad.registered)
		goto failed;

	if (!l_dbus_proxy_method_call(adapter->ad_proxy,
						"UnregisterAdvertisement",
						unreg_advertising_setup,
						stop_advertising_reply,
						NULL, NULL))
		goto failed;

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void start_discovery_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_proxy(proxy);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to start discovery (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_START_DISCOVERY,
						adapter->index, 0, NULL);
}

static void set_discovery_filter_setup(struct l_dbus_message *message,
							void *user_data)
{
	uint8_t flags = L_PTR_TO_UINT(user_data);
	struct l_dbus_message_builder *builder;

	if (!(flags & (BTP_GAP_DISCOVERY_FLAG_LE |
					BTP_GAP_DISCOVERY_FLAG_BREDR))) {
		l_info("Failed to start discovery - no transport set");
		return;
	}

	builder = l_dbus_message_builder_new(message);

	l_dbus_message_builder_enter_array(builder, "{sv}");
	l_dbus_message_builder_enter_dict(builder, "sv");

	/* Be in observer mode or in general mode (default in Bluez) */
	if (flags & BTP_GAP_DISCOVERY_FLAG_OBSERVATION) {
		l_dbus_message_builder_append_basic(builder, 's', "Transport");
		l_dbus_message_builder_enter_variant(builder, "s");

		if (flags & (BTP_GAP_DISCOVERY_FLAG_LE |
						BTP_GAP_DISCOVERY_FLAG_BREDR))
			l_dbus_message_builder_append_basic(builder, 's',
									"auto");
		else if (flags & BTP_GAP_DISCOVERY_FLAG_LE)
			l_dbus_message_builder_append_basic(builder, 's', "le");
		else if (flags & BTP_GAP_DISCOVERY_FLAG_BREDR)
			l_dbus_message_builder_append_basic(builder, 's',
								"bredr");

		l_dbus_message_builder_leave_variant(builder);
	}

	l_dbus_message_builder_leave_dict(builder);
	l_dbus_message_builder_leave_array(builder);

	/* TODO add passive, limited discovery */
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void set_discovery_filter_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_proxy(proxy);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to set discovery filter (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	l_dbus_proxy_method_call(adapter->proxy, "StartDiscovery", NULL,
					start_discovery_reply, NULL, NULL);
}

static void btp_gap_start_discovery(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_start_discovery_cp *cp = param;
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	/* Adapter needs to be powered to start discovery */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
									!prop) {
		btp_send_error(btp, BTP_GAP_SERVICE, index, BTP_ERROR_FAIL);
		return;
	}

	l_dbus_proxy_method_call(adapter->proxy, "SetDiscoveryFilter",
						set_discovery_filter_setup,
						set_discovery_filter_reply,
						L_UINT_TO_PTR(cp->flags), NULL);
}

static void clear_discovery_filter_setup(struct l_dbus_message *message,
							void *user_data)
{
	struct l_dbus_message_builder *builder;

	builder = l_dbus_message_builder_new(message);

	/* Clear discovery filter setup */
	l_dbus_message_builder_enter_array(builder, "{sv}");
	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_leave_dict(builder);
	l_dbus_message_builder_leave_array(builder);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void clear_discovery_filter_reaply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_proxy(proxy);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to set discovery filter (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_STOP_DISCOVERY,
						adapter->index, 0, NULL);
}

static void stop_discovery_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_proxy(proxy);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name;

		l_dbus_message_get_error(result, &name, NULL);
		l_error("Failed to stop discovery (%s)", name);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	l_dbus_proxy_method_call(adapter->proxy, "SetDiscoveryFilter",
						clear_discovery_filter_setup,
						clear_discovery_filter_reaply,
						NULL, NULL);
}

static void btp_gap_stop_discovery(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	/* Adapter needs to be powered to be able to remove devices */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
									!prop) {
		btp_send_error(btp, BTP_GAP_SERVICE, index, BTP_ERROR_FAIL);
		return;
	}

	l_dbus_proxy_method_call(adapter->proxy, "StopDiscovery", NULL,
					stop_discovery_reply, NULL, NULL);
}

static void connect_reply(struct l_dbus_proxy *proxy,
				struct l_dbus_message *result, void *user_data)
{
	uint8_t adapter_index = L_PTR_TO_UINT(user_data);
	struct btp_adapter *adapter = find_adapter_by_index(adapter_index);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to connect (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter_index,
								BTP_ERROR_FAIL);
		return;
	}

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_CONNECT, adapter_index, 0,
									NULL);
}

struct connect_device_data {
	bdaddr_t addr;
	uint8_t addr_type;
};

static void connect_device_destroy(void *connect_device_data)
{
	l_free(connect_device_data);
}

static void connect_device_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_proxy(proxy);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to connect device (%s), %s", name, desc);

		return;
	}
}

static void connect_device_setup(struct l_dbus_message *message,
								void *user_data)
{
	struct connect_device_data *cdd = user_data;
	struct l_dbus_message_builder *builder;
	char str_addr[18];

	ba2str(&cdd->addr, str_addr);

	builder = l_dbus_message_builder_new(message);

	l_dbus_message_builder_enter_array(builder, "{sv}");

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "Address");
	l_dbus_message_builder_enter_variant(builder, "s");
	l_dbus_message_builder_append_basic(builder, 's', str_addr);
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	l_dbus_message_builder_enter_dict(builder, "sv");
	l_dbus_message_builder_append_basic(builder, 's', "AddressType");
	l_dbus_message_builder_enter_variant(builder, "s");
	if (cdd->addr_type == BTP_GAP_ADDR_RANDOM)
		l_dbus_message_builder_append_basic(builder, 's', "random");
	else
		l_dbus_message_builder_append_basic(builder, 's', "public");
	l_dbus_message_builder_leave_variant(builder);
	l_dbus_message_builder_leave_dict(builder);

	l_dbus_message_builder_leave_array(builder);

	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void btp_gap_connect(uint8_t index, const void *param, uint16_t length,
								void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_connect_cp *cp = param;
	struct btp_device *device;
	struct connect_device_data *cdd;
	bool prop;
	uint8_t status = BTP_ERROR_FAIL;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to connect */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
									!prop)
		goto failed;

	device = find_device_by_address(adapter, &cp->address,
							cp->address_type);

	if (!device) {
		cdd = l_new(struct connect_device_data, 1);
		memcpy(&cdd->addr, &cp->address, sizeof(cdd->addr));
		cdd->addr_type = cp->address_type;

		l_dbus_proxy_method_call(adapter->proxy, "ConnectDevice",
							connect_device_setup,
							connect_device_reply,
							cdd,
							connect_device_destroy);

		btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_CONNECT,
						adapter->index, 0, NULL);
		return;
	}

	l_dbus_proxy_method_call(device->proxy, "Connect", NULL, connect_reply,
					L_UINT_TO_PTR(adapter->index), NULL);

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void disconnect_reply(struct l_dbus_proxy *proxy,
				struct l_dbus_message *result, void *user_data)
{
	uint8_t adapter_index = L_PTR_TO_UINT(user_data);
	struct btp_adapter *adapter = find_adapter_by_index(adapter_index);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to disconnect (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter_index,
								BTP_ERROR_FAIL);
		return;
	}

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_DISCONNECT, adapter_index, 0,
									NULL);
}

static void btp_gap_disconnect(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_disconnect_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	struct btp_device *device;
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to connect */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
									!prop)
		goto failed;

	device = find_device_by_address(adapter, &cp->address,
							cp->address_type);

	if (!device)
		goto failed;

	l_dbus_proxy_method_call(device->proxy, "Disconnect", NULL,
					disconnect_reply,
					L_UINT_TO_PTR(adapter->index), NULL);

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static struct l_dbus_message *ag_release_call(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct l_dbus_message *reply;

	reply = l_dbus_message_new_method_return(message);
	l_dbus_message_set_arguments(reply, "");

	return reply;
}

static struct l_dbus_message *ag_request_passkey_call(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct btp_gap_passkey_req_ev ev;
	struct btp_device *device;
	struct btp_adapter *adapter;
	const char *path, *str_addr, *str_addr_type;

	l_dbus_message_get_arguments(message, "o", &path);

	device = find_device_by_path(path);

	if (!l_dbus_proxy_get_property(device->proxy, "Address", "s", &str_addr)
		|| !l_dbus_proxy_get_property(device->proxy, "AddressType", "s",
		&str_addr_type)) {
		l_info("Cannot get device properties");

		return NULL;
	}

	ev.address_type = strcmp(str_addr_type, "public") ?
							BTP_GAP_ADDR_RANDOM :
							BTP_GAP_ADDR_PUBLIC;
	if (!str2ba(str_addr, &ev.address))
		return NULL;

	adapter = find_adapter_by_device(device);

	ag.pending_req = l_dbus_message_ref(message);

	btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_PASSKEY_REQUEST,
					adapter->index, sizeof(ev), &ev);

	return NULL;
}

static struct l_dbus_message *ag_display_passkey_call(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct btp_gap_passkey_display_ev ev;
	struct btp_device *device;
	struct btp_adapter *adapter;
	struct l_dbus_message *reply;
	const char *path, *str_addr, *str_addr_type;
	uint32_t passkey;
	uint16_t entered;

	reply = l_dbus_message_new_method_return(message);
	l_dbus_message_set_arguments(reply, "");

	l_dbus_message_get_arguments(message, "ouq", &path, &passkey, &entered);

	device = find_device_by_path(path);

	if (!l_dbus_proxy_get_property(device->proxy, "Address", "s", &str_addr)
		|| !l_dbus_proxy_get_property(device->proxy, "AddressType", "s",
		&str_addr_type)) {
		l_info("Cannot get device properties");

		return reply;
	}

	ev.passkey = L_CPU_TO_LE32(passkey);
	ev.address_type = strcmp(str_addr_type, "public") ?
							BTP_GAP_ADDR_RANDOM :
							BTP_GAP_ADDR_PUBLIC;
	if (str2ba(str_addr, &ev.address) < 0) {
		l_info("Incorrect device addres");

		return reply;
	}

	adapter = find_adapter_by_device(device);

	btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_PASSKEY_DISPLAY,
					adapter->index, sizeof(ev), &ev);

	return reply;
}

static struct l_dbus_message *ag_request_confirmation_call(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct btp_gap_passkey_confirm_ev ev;
	struct btp_device *device;
	struct btp_adapter *adapter;
	const char *path, *str_addr, *str_addr_type;
	uint32_t passkey;

	l_dbus_message_get_arguments(message, "ou", &path, &passkey);

	device = find_device_by_path(path);

	if (!l_dbus_proxy_get_property(device->proxy, "Address", "s", &str_addr)
		|| !l_dbus_proxy_get_property(device->proxy, "AddressType", "s",
		&str_addr_type)) {
		l_info("Cannot get device properties");

		return NULL;
	}

	ev.passkey = L_CPU_TO_LE32(passkey);
	ev.address_type = strcmp(str_addr_type, "public") ?
							BTP_GAP_ADDR_RANDOM :
							BTP_GAP_ADDR_PUBLIC;
	if (str2ba(str_addr, &ev.address) < 0) {
		l_info("Incorrect device address");

		return NULL;
	}

	adapter = find_adapter_by_device(device);

	ag.pending_req = l_dbus_message_ref(message);

	btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_PASSKEY_CONFIRM,
					adapter->index, sizeof(ev), &ev);

	return NULL;
}

static struct l_dbus_message *ag_request_authorization_call(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct l_dbus_message *reply;

	reply = l_dbus_message_new_method_return(message);
	l_dbus_message_set_arguments(reply, "");

	return reply;
}

static struct l_dbus_message *ag_authorize_service_call(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct l_dbus_message *reply;

	reply = l_dbus_message_new_method_return(message);
	l_dbus_message_set_arguments(reply, "");

	return reply;
}

static struct l_dbus_message *ag_cancel_call(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct l_dbus_message *reply;

	reply = l_dbus_message_new_method_return(message);
	l_dbus_message_set_arguments(reply, "");

	return reply;
}

static void setup_ag_interface(struct l_dbus_interface *iface)
{
	l_dbus_interface_method(iface, "Release", 0, ag_release_call, "", "");
	l_dbus_interface_method(iface, "RequestPasskey", 0,
					ag_request_passkey_call, "u", "o",
					"passkey", "device");
	l_dbus_interface_method(iface, "DisplayPasskey", 0,
					ag_display_passkey_call, "", "ouq",
					"device", "passkey", "entered");
	l_dbus_interface_method(iface, "RequestConfirmation", 0,
					ag_request_confirmation_call, "", "ou",
					"device", "passkey");
	l_dbus_interface_method(iface, "RequestAuthorization", 0,
					ag_request_authorization_call, "", "o",
					"device");
	l_dbus_interface_method(iface, "AuthorizeService", 0,
					ag_authorize_service_call, "", "os",
					"device", "uuid");
	l_dbus_interface_method(iface, "Cancel", 0, ag_cancel_call, "", "");
}

struct set_io_capabilities_data {
	uint8_t capa;
	struct btp_adapter *adapter;
};

static void set_io_capabilities_setup(struct l_dbus_message *message,
								void *user_data)
{
	struct set_io_capabilities_data *sicd = user_data;
	struct l_dbus_message_builder *builder;
	char *capa_str;

	builder = l_dbus_message_builder_new(message);

	l_dbus_message_builder_append_basic(builder, 'o', AG_PATH);

	switch (sicd->capa) {
	case BTP_GAP_IOCAPA_DISPLAY_ONLY:
		capa_str = "DisplayOnly";
		break;
	case BTP_GAP_IOCAPA_DISPLAY_YESNO:
		capa_str = "DisplayYesNo";
		break;
	case BTP_GAP_IOCAPA_KEYBOARD_ONLY:
		capa_str = "KeyboardOnly";
		break;
	case BTP_GAP_IOCAPA_KEYBOARD_DISPLAY:
		capa_str = "KeyboardDisplay";
		break;
	case BTP_GAP_IOCAPA_NO_INPUT_NO_OUTPUT:
	default:
		capa_str = "NoInputNoOutput";
		break;
	}

	l_dbus_message_builder_append_basic(builder, 's', capa_str);

	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void reg_def_req_default_agent_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
			l_info("Unable to remove agent instance");
		if (!l_dbus_unregister_interface(dbus, AG_IFACE))
			l_info("Unable to unregister agent interface");

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to request default agent (%s), %s", name, desc);

		btp_send_error(btp, BTP_CORE_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	register_gap_service();
	gap_service_registered = true;

	ag.registered = true;

	btp_send(btp, BTP_CORE_SERVICE, BTP_OP_CORE_REGISTER,
					BTP_INDEX_NON_CONTROLLER, 0, NULL);
}

static void set_io_req_default_agent_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct btp_adapter *adapter = user_data;

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		goto failed;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to set io capabilities (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		goto failed;
	}

	ag.registered = true;

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_SET_IO_CAPA,
						adapter->index, 0, NULL);

	return;

failed:
	if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
		l_info("Unable to remove agent instance");
	if (!l_dbus_unregister_interface(dbus, AG_IFACE))
		l_info("Unable to unregister agent interface");
}

static void request_default_agent_setup(struct l_dbus_message *message,
								void *user_data)
{
	struct l_dbus_message_builder *builder;

	builder = l_dbus_message_builder_new(message);

	l_dbus_message_builder_append_basic(builder, 'o', AG_PATH);

	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void set_io_capabilities_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct set_io_capabilities_data *sicd = user_data;

	if (!sicd->adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		goto failed;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to set io capabilities (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, sicd->adapter->index,
								BTP_ERROR_FAIL);
		goto failed;
	}

	if (l_dbus_proxy_method_call(ag.proxy, "RequestDefaultAgent",
						request_default_agent_setup,
						set_io_req_default_agent_reply,
						sicd->adapter, NULL))
		return;

failed:
	if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
		l_info("Unable to remove agent instance");
	if (!l_dbus_unregister_interface(dbus, AG_IFACE))
		l_info("Unable to unregister agent interface");
}

static void register_default_agent_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	const char *name, *desc;

	if (l_dbus_message_is_error(result)) {
		if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
			l_info("Unable to remove agent instance");
		if (!l_dbus_unregister_interface(dbus, AG_IFACE))
			l_info("Unable to unregister agent interface");

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to register default agent (%s), %s", name,
									desc);
		return;
	}

	if (!l_dbus_proxy_method_call(ag.proxy, "RequestDefaultAgent",
						request_default_agent_setup,
						reg_def_req_default_agent_reply,
						NULL, NULL)) {
		if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
			l_info("Unable to remove agent instance");
		if (!l_dbus_unregister_interface(dbus, AG_IFACE))
			l_info("Unable to unregister agent interface");
	}
}

static void set_io_capabilities_destroy(void *user_data)
{
	l_free(user_data);
}

static bool register_default_agent(struct btp_adapter *adapter, uint8_t capa,
				l_dbus_client_proxy_result_func_t set_io_cb)
{
	struct set_io_capabilities_data *data;

	if (!l_dbus_register_interface(dbus, AG_IFACE, setup_ag_interface, NULL,
								false)) {
		l_info("Unable to register agent interface");
		return false;
	}

	if (!l_dbus_object_add_interface(dbus, AG_PATH, AG_IFACE, NULL)) {
		l_info("Unable to instantiate agent interface");

		if (!l_dbus_unregister_interface(dbus, AG_IFACE))
			l_info("Unable to unregister agent interface");

		return false;
	}

	if (!l_dbus_object_add_interface(dbus, AG_PATH,
						L_DBUS_INTERFACE_PROPERTIES,
						NULL)) {
		l_info("Unable to instantiate the ag properties interface");

		if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
			l_info("Unable to remove agent instance");
		if (!l_dbus_unregister_interface(dbus, AG_IFACE))
			l_info("Unable to unregister agent interface");

		return false;
	}

	data = l_new(struct set_io_capabilities_data, 1);
	data->adapter = adapter;
	data->capa = capa;

	if (!l_dbus_proxy_method_call(ag.proxy, "RegisterAgent",
					set_io_capabilities_setup, set_io_cb,
					data, set_io_capabilities_destroy)) {
		if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
			l_info("Unable to remove agent instance");
		if (!l_dbus_unregister_interface(dbus, AG_IFACE))
			l_info("Unable to unregister agent interface");

		return false;
	}

	return true;
}

struct rereg_unreg_agent_data {
	struct btp_adapter *adapter;
	l_dbus_client_proxy_result_func_t cb;
	uint8_t capa;
};

static void rereg_unreg_agent_reply(struct l_dbus_proxy *proxy,
						struct l_dbus_message *result,
						void *user_data)
{
	struct rereg_unreg_agent_data *ruad = user_data;

	if (l_dbus_message_is_error(result)) {
		const char *name;

		l_dbus_message_get_error(result, &name, NULL);

		l_error("Failed to unregister agent %s (%s)",
					l_dbus_proxy_get_path(proxy), name);
		return;
	}

	if (!l_dbus_object_remove_interface(dbus, AG_PATH,
						L_DBUS_INTERFACE_PROPERTIES))
		l_info("Unable to remove propety instance");
	if (!l_dbus_object_remove_interface(dbus, AG_PATH, AG_IFACE))
		l_info("Unable to remove agent instance");
	if (!l_dbus_unregister_interface(dbus, AG_IFACE))
		l_info("Unable to unregister agent interface");

	ag.registered = false;

	if (!register_default_agent(ruad->adapter, ruad->capa, ruad->cb))
		btp_send_error(btp, BTP_GAP_SERVICE, ruad->adapter->index,
								BTP_ERROR_FAIL);
}

static void rereg_unreg_agent_destroy(void *rereg_unreg_agent_data)
{
	l_free(rereg_unreg_agent_data);
}

static void btp_gap_set_io_capabilities(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_set_io_capa_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	struct rereg_unreg_agent_data *data;
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	switch (cp->capa) {
	case BTP_GAP_IOCAPA_DISPLAY_ONLY:
	case BTP_GAP_IOCAPA_DISPLAY_YESNO:
	case BTP_GAP_IOCAPA_KEYBOARD_ONLY:
	case BTP_GAP_IOCAPA_NO_INPUT_NO_OUTPUT:
	case BTP_GAP_IOCAPA_KEYBOARD_DISPLAY:
		break;
	default:
		l_error("Wrong iocapa given!");

		goto failed;
	}

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to set io cap */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
									!prop)
		goto failed;

	if (ag.registered) {
		data = l_new(struct rereg_unreg_agent_data, 1);
		data->adapter = adapter;
		data->capa = cp->capa;
		data->cb = set_io_capabilities_reply;

		if (!l_dbus_proxy_method_call(ag.proxy, "UnregisterAgent",
						unreg_agent_setup,
						rereg_unreg_agent_reply, data,
						rereg_unreg_agent_destroy))
			goto failed;

		return;
	}

	if (!register_default_agent(adapter, cp->capa,
						set_io_capabilities_reply))
		goto failed;

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void pair_reply(struct l_dbus_proxy *proxy,
				struct l_dbus_message *result, void *user_data)
{
	uint8_t adapter_index = L_PTR_TO_UINT(user_data);
	struct btp_adapter *adapter = find_adapter_by_index(adapter_index);

	if (!adapter)
		return;

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to pair (%s), %s", name, desc);

		return;
	}
}

static void btp_gap_pair(uint8_t index, const void *param, uint16_t length,
								void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_pair_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	struct btp_device *device;
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to pair */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
									!prop)
		goto failed;

	device = find_device_by_address(adapter, &cp->address,
							cp->address_type);

	if (!device)
		goto failed;

	/* This command is asynchronous, send reply immediatelly to not block
	 * pairing process eg. passkey request.
	 */
	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_PAIR, adapter->index, 0,
									NULL);

	l_dbus_proxy_method_call(device->proxy, "Pair", NULL, pair_reply,
					L_UINT_TO_PTR(adapter->index), NULL);

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void unpair_reply(struct l_dbus_proxy *proxy,
				struct l_dbus_message *result, void *user_data)
{
	struct btp_device *device = user_data;
	struct btp_adapter *adapter = find_adapter_by_device(device);

	if (!adapter) {
		btp_send_error(btp, BTP_GAP_SERVICE, BTP_INDEX_NON_CONTROLLER,
								BTP_ERROR_FAIL);
		return;
	}

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to unpair (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_UNPAIR, adapter->index, 0,
									NULL);
}

static void unpair_setup(struct l_dbus_message *message, void *user_data)
{
	struct btp_device *device = user_data;
	const char *path = l_dbus_proxy_get_path(device->proxy);
	struct l_dbus_message_builder *builder;

	builder = l_dbus_message_builder_new(message);

	l_dbus_message_builder_append_basic(builder, 'o', path);

	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);
}

static void btp_gap_unpair(uint8_t index, const void *param, uint16_t length,
								void *user_data)
{
	struct btp_adapter *adapter = find_adapter_by_index(index);
	const struct btp_gap_pair_cp *cp = param;
	uint8_t status = BTP_ERROR_FAIL;
	struct btp_device *device;
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to unpair */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
									!prop)
		goto failed;

	device = find_device_by_address(adapter, &cp->address,
							cp->address_type);

	if (!device)
		goto failed;

	/* There is no direct unpair method, removing device will clear pairing
	 * information.
	 */
	l_dbus_proxy_method_call(adapter->proxy, "RemoveDevice", unpair_setup,
						unpair_reply, device, NULL);

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void passkey_entry_rsp_reply(struct l_dbus_message *result,
								void *user_data)
{
	struct btp_adapter *adapter = user_data;

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to reply with passkey (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	l_dbus_message_unref(ag.pending_req);

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_PASSKEY_ENTRY_RSP,
						adapter->index, 0, NULL);
}

static void btp_gap_passkey_entry_rsp(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	const struct btp_gap_passkey_entry_rsp_cp *cp = param;
	struct btp_adapter *adapter = find_adapter_by_index(index);
	struct l_dbus_message_builder *builder;
	uint8_t status = BTP_ERROR_FAIL;
	uint32_t passkey = L_CPU_TO_LE32(cp->passkey);
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to response with passkey */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
						!prop || !ag.pending_req)
		goto failed;

	builder = l_dbus_message_builder_new(ag.pending_req);
	l_dbus_message_builder_append_basic(builder, 'u', &passkey);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);

	l_dbus_send_with_reply(dbus, ag.pending_req, passkey_entry_rsp_reply,
								adapter, NULL);

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void passkey_confirm_rsp_reply(struct l_dbus_message *result,
								void *user_data)
{
	struct btp_adapter *adapter = user_data;

	if (l_dbus_message_is_error(result)) {
		const char *name, *desc;

		l_dbus_message_get_error(result, &name, &desc);
		l_error("Failed to confirm passkey (%s), %s", name, desc);

		btp_send_error(btp, BTP_GAP_SERVICE, adapter->index,
								BTP_ERROR_FAIL);
		return;
	}

	l_dbus_message_unref(ag.pending_req);

	btp_send(btp, BTP_GAP_SERVICE, BTP_OP_GAP_PASSKEY_CONFIRM_RSP,
						adapter->index, 0, NULL);
}

static void btp_gap_confirm_entry_rsp(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	const struct btp_gap_passkey_confirm_rsp_cp *cp = param;
	struct btp_adapter *adapter = find_adapter_by_index(index);
	struct l_dbus_message *reply;
	uint8_t status = BTP_ERROR_FAIL;
	bool prop;

	l_debug("index: %d, length: %d\n", index, length);

	if (!adapter) {
		status = BTP_ERROR_INVALID_INDEX;
		goto failed;
	}

	/* Adapter needs to be powered to be able to confirm passkey */
	if (!l_dbus_proxy_get_property(adapter->proxy, "Powered", "b", &prop) ||
						!prop || !ag.pending_req)
		goto failed;

	if (cp->match) {
		reply = l_dbus_message_new_method_return(ag.pending_req);
		l_dbus_message_set_arguments(reply, "");
	} else {
		reply = l_dbus_message_new_error(ag.pending_req,
						"org.bluez.Error.Rejected",
						"Passkey missmatch");
	}

	l_dbus_send_with_reply(dbus, ag.pending_req, passkey_confirm_rsp_reply,
								adapter, NULL);

	return;

failed:
	btp_send_error(btp, BTP_GAP_SERVICE, index, status);
}

static void btp_gap_device_found_ev(struct l_dbus_proxy *proxy)
{
	struct btp_device *device = find_device_by_proxy(proxy);
	struct btp_adapter *adapter = find_adapter_by_device(device);
	struct btp_device_found_ev ev;
	struct btp_gap_device_connected_ev ev_conn;
	const char *str, *addr_str;
	int16_t rssi;
	uint8_t address_type;
	bool connected;

	l_debug("proxy: %p", proxy);

	if (!l_dbus_proxy_get_property(proxy, "Address", "s", &addr_str) ||
					str2ba(addr_str, &ev.address) < 0)
		return;

	if (!l_dbus_proxy_get_property(proxy, "AddressType", "s", &str))
		return;

	address_type = strcmp(str, "public") ? BTP_GAP_ADDR_RANDOM :
							BTP_GAP_ADDR_PUBLIC;
	ev.address_type = address_type;

	if (!l_dbus_proxy_get_property(proxy, "RSSI", "n", &rssi))
		ev.rssi = 0x81;
	else
		ev.rssi = rssi;

	/* TODO Temporary set all flags */
	ev.flags = (BTP_EV_GAP_DEVICE_FOUND_FLAG_RSSI |
					BTP_EV_GAP_DEVICE_FOUND_FLAG_AD |
					BTP_EV_GAP_DEVICE_FOUND_FLAG_SR);

	/* TODO Add eir to device found event */
	ev.eir_len = 0;

	btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_DEVICE_FOUND, adapter->index,
						sizeof(ev) + ev.eir_len, &ev);

	if (l_dbus_proxy_get_property(proxy, "Connected", "b", &connected) &&
								connected) {
		ev_conn.address_type = address_type;
		str2ba(addr_str, &ev_conn.address);

		btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_DEVICE_CONNECTED,
				adapter->index, sizeof(ev_conn), &ev_conn);
	}
}

static void btp_gap_device_connection_ev(struct l_dbus_proxy *proxy,
								bool connected)
{
	struct btp_adapter *adapter;
	struct btp_device *device;
	const char *str_addr, *str_addr_type;
	uint8_t address_type;

	l_debug("proxy: %p, connected: %d\n", proxy, connected);

	device = find_device_by_proxy(proxy);
	adapter = find_adapter_by_device(device);

	if (!device || !adapter)
		return;

	if (!l_dbus_proxy_get_property(proxy, "Address", "s", &str_addr))
		return;

	if (!l_dbus_proxy_get_property(proxy, "AddressType", "s",
								&str_addr_type))
		return;

	address_type = strcmp(str_addr_type, "public") ? BTP_GAP_ADDR_RANDOM :
							BTP_GAP_ADDR_PUBLIC;

	if (connected) {
		struct btp_gap_device_connected_ev ev;

		str2ba(str_addr, &ev.address);
		ev.address_type = address_type;

		btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_DEVICE_CONNECTED,
					adapter->index, sizeof(ev), &ev);
	} else {
		struct btp_gap_device_disconnected_ev ev;

		str2ba(str_addr, &ev.address);
		ev.address_type = address_type;

		btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_DEVICE_DISCONNECTED,
					adapter->index, sizeof(ev), &ev);
	}
}

static void btp_identity_resolved_ev(struct l_dbus_proxy *proxy)
{
	struct btp_device *dev = find_device_by_proxy(proxy);
	struct btp_adapter *adapter = find_adapter_by_device(dev);
	struct btp_gap_identity_resolved_ev ev;
	char *str_addr, *str_addr_type;
	uint8_t identity_address_type;

	l_debug("proxy: %p", proxy);

	if (!l_dbus_proxy_get_property(proxy, "Address", "s", &str_addr))
		return;

	if (!l_dbus_proxy_get_property(proxy, "AddressType", "s",
								&str_addr_type))
		return;

	identity_address_type = strcmp(str_addr_type, "public") ?
				BTP_GAP_ADDR_RANDOM : BTP_GAP_ADDR_PUBLIC;

	str2ba(str_addr, &ev.identity_address);
	ev.identity_address_type = identity_address_type;

	memcpy(&ev.address, &dev->address, sizeof(ev.address));
	ev.address_type = dev->address_type;

	btp_send(btp, BTP_GAP_SERVICE, BTP_EV_GAP_IDENTITY_RESOLVED,
					adapter->index, sizeof(ev), &ev);
}

static void register_gap_service(void)
{
	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_READ_SUPPORTED_COMMANDS,
					btp_gap_read_commands, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE,
				BTP_OP_GAP_READ_CONTROLLER_INDEX_LIST,
				btp_gap_read_controller_index, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_READ_COTROLLER_INFO,
						btp_gap_read_info, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_RESET,
						btp_gap_reset, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_SET_POWERED,
					btp_gap_set_powered, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_SET_CONNECTABLE,
					btp_gap_set_connectable, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_SET_DISCOVERABLE,
					btp_gap_set_discoverable, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_SET_BONDABLE,
					btp_gap_set_bondable, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_START_ADVERTISING,
					btp_gap_start_advertising, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_STOP_ADVERTISING,
					btp_gap_stop_advertising, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_START_DISCOVERY,
					btp_gap_start_discovery, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_STOP_DISCOVERY,
					btp_gap_stop_discovery, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_CONNECT, btp_gap_connect,
								NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_DISCONNECT,
						btp_gap_disconnect, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_SET_IO_CAPA,
				btp_gap_set_io_capabilities, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_PAIR, btp_gap_pair, NULL,
									NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_UNPAIR, btp_gap_unpair,
								NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_PASSKEY_ENTRY_RSP,
					btp_gap_passkey_entry_rsp, NULL, NULL);

	btp_register(btp, BTP_GAP_SERVICE, BTP_OP_GAP_PASSKEY_CONFIRM_RSP,
					btp_gap_confirm_entry_rsp, NULL, NULL);
}

static void btp_core_read_commands(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	uint8_t commands = 0;

	l_debug("index: %d, length: %d\n", index, length);

	if (index != BTP_INDEX_NON_CONTROLLER) {
		btp_send_error(btp, BTP_CORE_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	commands |= (1 << BTP_OP_CORE_READ_SUPPORTED_COMMANDS);
	commands |= (1 << BTP_OP_CORE_READ_SUPPORTED_SERVICES);
	commands |= (1 << BTP_OP_CORE_REGISTER);
	commands |= (1 << BTP_OP_CORE_UNREGISTER);

	btp_send(btp, BTP_CORE_SERVICE, BTP_OP_CORE_READ_SUPPORTED_COMMANDS,
			BTP_INDEX_NON_CONTROLLER, sizeof(commands), &commands);
}

static void btp_core_read_services(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	uint8_t services = 0;

	l_debug("index: %d, length: %d\n", index, length);

	if (index != BTP_INDEX_NON_CONTROLLER) {
		btp_send_error(btp, BTP_CORE_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	services |= (1 << BTP_CORE_SERVICE);
	services |= (1 << BTP_GAP_SERVICE);
	// services |= (1 << BTP_GATT_SERVICE); // DEPRECATED by auto-pts
	services |= (1 << BTP_L2CAP_SERVICE);
	services |= (1 << BTP_GATT_CLIENT_SERVICE);
	services |= (1 << BTP_GATT_SERVER_SERVICE);

	btp_send(btp, BTP_CORE_SERVICE, BTP_OP_CORE_READ_SUPPORTED_SERVICES,
			BTP_INDEX_NON_CONTROLLER, sizeof(services), &services);
}

static void btp_core_register(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	const struct btp_core_register_cp  *cp = param;

	l_debug("index: %d, length: %d\n", index, length);

	if (length < sizeof(*cp))
		goto failed;

	if (index != BTP_INDEX_NON_CONTROLLER) {
		btp_send_error(btp, BTP_CORE_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	switch (cp->service_id) {
	case BTP_GAP_SERVICE:
		if (gap_service_registered)
			goto failed;

		if (!register_default_agent(NULL,
					BTP_GAP_IOCAPA_NO_INPUT_NO_OUTPUT,
					register_default_agent_reply))
			goto failed;

		return;
	case BTP_L2CAP_SERVICE:
		if (l2cap_service_registered)
			goto failed;

		register_l2cap_service();
		l2cap_service_registered = true;
		break;

	case BTP_GATT_CLIENT_SERVICE:
		if (gatt_client_service_registered)
			goto failed;

		gatt_client_service_registered = true;
		break;

	case BTP_GATT_SERVER_SERVICE:
		if (gatt_server_service_registered)
			goto failed;

		gatt_server_service_registered = true;
		break;

	case BTP_MESH_NODE_SERVICE:
	case BTP_MESH_MODEL_SERVICE:
	case BTP_GATT_SERVICE:
	case BTP_CORE_SERVICE:
	default:
		goto failed;
	}

	btp_send(btp, BTP_CORE_SERVICE, BTP_OP_CORE_REGISTER,
					BTP_INDEX_NON_CONTROLLER, 0, NULL);
	return;

failed:
	btp_send_error(btp, BTP_CORE_SERVICE, index, BTP_ERROR_FAIL);
}

static void btp_core_unregister(uint8_t index, const void *param,
					uint16_t length, void *user_data)
{
	const struct btp_core_unregister_cp  *cp = param;

	l_debug("index: %d, length: %d\n", index, length);

	if (length < sizeof(*cp))
		goto failed;

	if (index != BTP_INDEX_NON_CONTROLLER) {
		btp_send_error(btp, BTP_CORE_SERVICE, index,
						BTP_ERROR_INVALID_INDEX);
		return;
	}

	switch (cp->service_id) {
	case BTP_GAP_SERVICE:
		if (!gap_service_registered)
			goto failed;

		btp_unregister_service(btp, BTP_GAP_SERVICE);
		gap_service_registered = false;
		break;

	case BTP_L2CAP_SERVICE:
		if (!l2cap_service_registered)
			goto failed;
		l2cap_service_registered = false;
		break;

	case BTP_GATT_CLIENT_SERVICE:
		if (!gatt_client_service_registered)
			goto failed;
		gatt_client_service_registered = false;
		break;

	case BTP_GATT_SERVER_SERVICE:
		if (!gatt_server_service_registered)
			goto failed;
		gatt_server_service_registered = false;
		break;

	case BTP_MESH_NODE_SERVICE:
	case BTP_MESH_MODEL_SERVICE:
	case BTP_GATT_SERVICE:
	case BTP_CORE_SERVICE:
	default:
		goto failed;
	}

	btp_send(btp, BTP_CORE_SERVICE, BTP_OP_CORE_UNREGISTER,
					BTP_INDEX_NON_CONTROLLER, 0, NULL);
	return;

failed:
	btp_send_error(btp, BTP_CORE_SERVICE, index, BTP_ERROR_FAIL);
}

static void register_core_service(void)
{
	btp_register(btp, BTP_CORE_SERVICE,
					BTP_OP_CORE_READ_SUPPORTED_COMMANDS,
					btp_core_read_commands, NULL, NULL);

	btp_register(btp, BTP_CORE_SERVICE,
					BTP_OP_CORE_READ_SUPPORTED_SERVICES,
					btp_core_read_services, NULL, NULL);

	btp_register(btp, BTP_CORE_SERVICE, BTP_OP_CORE_REGISTER,
						btp_core_register, NULL, NULL);

	btp_register(btp, BTP_CORE_SERVICE, BTP_OP_CORE_UNREGISTER,
					btp_core_unregister, NULL, NULL);
}

static void signal_handler(uint32_t signo, void *user_data)
{
	switch (signo) {
	case SIGINT:
	case SIGTERM:
		l_info("Terminating");
		l_main_quit();
		break;
	}
}

static void btp_device_free(struct btp_device *device)
{
	l_debug("device: %p", device);

	l_free(device);
}

static void btp_adapter_free(struct btp_adapter *adapter)
{
	l_debug("adapter: %p", adapter);

	l_queue_destroy(adapter->devices,
				(l_queue_destroy_func_t)btp_device_free);
	l_free(adapter);
}

static void extract_settings(struct l_dbus_proxy *proxy, uint32_t *current,
						uint32_t *supported)
{
	bool prop;

	*supported = 0;
	*current = 0;

	/* TODO not all info is available via D-Bus API */
	*supported |=  BTP_GAP_SETTING_POWERED;
	*supported |=  BTP_GAP_SETTING_CONNECTABLE;
	*supported |=  BTP_GAP_SETTING_DISCOVERABLE;
	*supported |=  BTP_GAP_SETTING_BONDABLE;
	*supported |=  BTP_GAP_SETTING_SSP;
	*supported |=  BTP_GAP_SETTING_BREDR;
	*supported |=  BTP_GAP_SETTING_LE;
	*supported |=  BTP_GAP_SETTING_ADVERTISING;
	*supported |=  BTP_GAP_SETTING_SC;
	*supported |=  BTP_GAP_SETTING_PRIVACY;
	/* *supported |=  BTP_GAP_SETTING_STATIC_ADDRESS; */

	/* TODO not all info is availbe via D-Bus API so some are assumed to be
	 * enabled by bluetoothd or simply hardcoded until API is extended
	 */
	*current |=  BTP_GAP_SETTING_CONNECTABLE;
	*current |=  BTP_GAP_SETTING_SSP;
	*current |=  BTP_GAP_SETTING_BREDR;
	*current |=  BTP_GAP_SETTING_LE;
	*current |=  BTP_GAP_SETTING_PRIVACY;
	*current |=  BTP_GAP_SETTING_SC;
	/* *supported |=  BTP_GAP_SETTING_STATIC_ADDRESS; */

	if (l_dbus_proxy_get_property(proxy, "Powered", "b", &prop) && prop)
		*current |=  BTP_GAP_SETTING_POWERED;

	if (l_dbus_proxy_get_property(proxy, "Discoverable", "b", &prop) &&
									prop)
		*current |=  BTP_GAP_SETTING_DISCOVERABLE;

	if (l_dbus_proxy_get_property(proxy, "Pairable", "b", &prop) && prop)
		*current |=  BTP_GAP_SETTING_BONDABLE;
}

static void proxy_added(struct l_dbus_proxy *proxy, void *user_data)
{
	const char *interface = l_dbus_proxy_get_interface(proxy);
	const char *path = l_dbus_proxy_get_path(proxy);

	l_info("Proxy added: %s (%s)", interface, path);

	if (!strcmp(interface, "org.bluez.Adapter1")) {
		struct btp_adapter *adapter;

		adapter = l_new(struct btp_adapter, 1);
		adapter->proxy = proxy;
		adapter->index = l_queue_length(adapters);
		adapter->devices = l_queue_new();

		extract_settings(proxy, &adapter->current_settings,
						&adapter->supported_settings);

		adapter->default_settings = adapter->current_settings;

		l_queue_push_tail(adapters, adapter);
		return;
	}

	if (!strcmp(interface, "org.bluez.Device1")) {
		struct btp_adapter *adapter;
		struct btp_device *device;
		char *str, *str_addr, *str_addr_type;

		if (!l_dbus_proxy_get_property(proxy, "Adapter", "o", &str))
			return;

		adapter = find_adapter_by_path(str);
		if (!adapter)
			return;

		device = l_new(struct btp_device, 1);
		device->proxy = proxy;

		l_queue_push_tail(adapter->devices, device);

		btp_gap_device_found_ev(proxy);

		if (!l_dbus_proxy_get_property(proxy, "Address", "s",
								&str_addr))
			return;

		if (!l_dbus_proxy_get_property(proxy, "AddressType", "s",
								&str_addr_type))
			return;

		device->address_type = strcmp(str_addr_type, "public") ?
							BTP_GAP_ADDR_RANDOM :
							BTP_GAP_ADDR_PUBLIC;
		if (!str2ba(str_addr, &device->address))
			return;

		return;
	}

	if (!strcmp(interface, "org.bluez.LEAdvertisingManager1")) {
		struct btp_adapter *adapter;

		adapter = find_adapter_by_path(path);
		if (!adapter)
			return;

		adapter->ad_proxy = proxy;

		return;
	}

	if (!strcmp(interface, "org.bluez.AgentManager1")) {
		ag.proxy = proxy;

		return;
	}
}

static bool device_match_by_proxy(const void *a, const void *b)
{
	const struct btp_device *device = a;
	const struct l_dbus_proxy *proxy = b;

	return device->proxy == proxy;
}

static void proxy_removed(struct l_dbus_proxy *proxy, void *user_data)
{
	const char *interface = l_dbus_proxy_get_interface(proxy);
	const char *path = l_dbus_proxy_get_path(proxy);

	l_info("Proxy removed: %s (%s)", interface, path);

	if (!strcmp(interface, "org.bluez.Adapter1")) {
		l_info("Adapter removed, terminating.");
		l_main_quit();
		return;
	}

	if (!strcmp(interface, "org.bluez.Device1")) {
		struct btp_adapter *adapter;
		char *str;

		if (!l_dbus_proxy_get_property(proxy, "Adapter", "o", &str))
			return;

		adapter = find_adapter_by_path(str);
		if (!adapter)
			return;

		l_queue_remove_if(adapter->devices, device_match_by_proxy,
									proxy);

		return;
	}
}

static void property_changed(struct l_dbus_proxy *proxy, const char *name,
				struct l_dbus_message *msg, void *user_data)
{
	const char *interface = l_dbus_proxy_get_interface(proxy);
	const char *path = l_dbus_proxy_get_path(proxy);

	l_info("property_changed %s %s %s", name, path, interface);

	if (!strcmp(interface, "org.bluez.Adapter1")) {
		struct btp_adapter *adapter = find_adapter_by_proxy(proxy);
		uint32_t new_settings;

		if (!adapter)
			return;

		new_settings = adapter->current_settings;

		if (!strcmp(name, "Powered")) {
			bool prop;

			if (!l_dbus_message_get_arguments(msg, "b", &prop))
				return;

			if (prop)
				new_settings |= BTP_GAP_SETTING_POWERED;
			else
				new_settings &= ~BTP_GAP_SETTING_POWERED;
		} else if (!strcmp(name, "Discoverable")) {
			bool prop;

			if (!l_dbus_message_get_arguments(msg, "b", &prop))
				return;

			if (prop)
				new_settings |= BTP_GAP_SETTING_DISCOVERABLE;
			else
				new_settings &= ~BTP_GAP_SETTING_DISCOVERABLE;
		}

		if (!strcmp(name, "Pairable")) {
			bool prop;

			if (!l_dbus_message_get_arguments(msg, "b", &prop))
				return;

			if (prop)
				new_settings |= BTP_GAP_SETTING_BONDABLE;
			else
				new_settings &= ~BTP_GAP_SETTING_BONDABLE;
		}

		if (new_settings != adapter->current_settings)
			update_current_settings(adapter, new_settings);

		return;
	} else if (!strcmp(interface, "org.bluez.Device1")) {
		if (!strcmp(name, "RSSI")) {
			int16_t rssi;

			if (!l_dbus_message_get_arguments(msg, "n", &rssi))
				return;

			btp_gap_device_found_ev(proxy);
		} else if (!strcmp(name, "Connected")) {
			bool prop;

			if (!l_dbus_message_get_arguments(msg, "b", &prop))
				return;

			btp_gap_device_connection_ev(proxy, prop);
		} else if (!strcmp(name, "AddressType")) {
			/* Addres property change came first along with address
			 * type.
			 */
			btp_identity_resolved_ev(proxy);
		}
	}
}

static void client_connected(struct l_dbus *dbus, void *user_data)
{
	l_debug("D-Bus client connected");
}

static void client_disconnected(struct l_dbus *dbus, void *user_data)
{
	l_debug("D-Bus client disconnected, terminated");
	l_main_quit();
}

static void btp_disconnect_handler(struct btp *btp, void *user_data)
{
	l_debug("btp disconnected");
	l_main_quit();
}

static void client_ready(struct l_dbus_client *client, void *user_data)
{
	l_debug("D-Bus client ready, connecting BTP");

	btp = btp_new(socket_path);
	if (!btp) {
		l_error("Failed to connect BTP, terminating");
		l_main_quit();
		return;
	}

	btp_set_disconnect_handler(btp, btp_disconnect_handler, NULL, NULL);

	register_core_service();

	btp_send(btp, BTP_CORE_SERVICE, BTP_EV_CORE_READY,
					BTP_INDEX_NON_CONTROLLER, 0, NULL);
}

static void ready_callback(void *user_data)
{
	if (!l_dbus_object_manager_enable(dbus, "/"))
		l_info("Unable to register the ObjectManager");
}

static void usage(void)
{
	l_info("btpclient - Bluetooth tester");
	l_info("Usage:");
	l_info("\tbtpclient [options]");
	l_info("options:\n"
	"\t-s, --socket <socket>  Socket to use for BTP\n"
	"\t-q, --quiet            Don't emit any logs\n"
	"\t-v, --version          Show version\n"
	"\t-h, --help             Show help options");
}

static const struct option options[] = {
	{ "socket",	1, 0, 's' },
	{ "quiet",	0, 0, 'q' },
	{ "version",	0, 0, 'v' },
	{ "help",	0, 0, 'h' },
	{ 0, 0, 0, 0 }
};

int main(int argc, char *argv[])
{
	struct l_dbus_client *client;
	int opt;

	// l_log_set_stderr();
	l_log_set_syslog();
	// l_log_set_journal();
	l_debug_enable("*");

	// openlog("btpclient", LOG_PERROR | LOG_PID, LOG_LOCAL0);

	while ((opt = getopt_long(argc, argv, "+hs:vq", options, NULL)) != -1) {
		switch (opt) {
		case 's':
			socket_path = l_strdup(optarg);
			break;
		case 'q':
			l_log_set_null();
			break;
		case 'd':
			break;
		case 'v':
			l_info("%s", VERSION);
			return EXIT_SUCCESS;
		case 'h':
		default:
			usage();
			return EXIT_SUCCESS;
		}
	}

	if (!socket_path) {
		l_info("Socket option is required");
		l_info("Type --help for usage");
		return EXIT_FAILURE;
	}

	if (data_size < 0)
		buffer_size = (omtu > imtu) ? omtu : imtu;
	else
		buffer_size = data_size;

	if (!(buf = malloc(buffer_size))) {
		perror("Can't allocate data buffer");
		return EXIT_FAILURE;
	}

	hci_devba(0, &bdaddr_local);

	if (!l_main_init())
		return EXIT_FAILURE;

	adapters = l_queue_new();

	dbus = l_dbus_new_default(L_DBUS_SYSTEM_BUS);
	l_dbus_set_ready_handler(dbus, ready_callback, NULL, NULL);
	client = l_dbus_client_new(dbus, "org.bluez", "/org/bluez");

	l_dbus_client_set_connect_handler(client, client_connected, NULL, NULL);
	l_dbus_client_set_disconnect_handler(client, client_disconnected, NULL,
									NULL);

	l_dbus_client_set_proxy_handlers(client, proxy_added, proxy_removed,
						property_changed, NULL, NULL);

	l_dbus_client_set_ready_handler(client, client_ready, NULL, NULL);

	l_main_run_with_signal(signal_handler, NULL);

	l_dbus_client_destroy(client);
	l_dbus_destroy(dbus);
	btp_cleanup(btp);

	l_queue_destroy(adapters, (l_queue_destroy_func_t)btp_adapter_free);

	l_free(socket_path);

	l_main_exit();

	return EXIT_SUCCESS;
}
