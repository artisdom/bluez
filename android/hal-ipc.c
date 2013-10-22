/*
 * Copyright (C) 2013 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdbool.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <stdint.h>

#include <cutils/properties.h>

#include "hal.h"
#include "hal-msg.h"
#include "hal-log.h"
#include "hal-ipc.h"

#define CONNECT_TIMEOUT (5 * 1000)
#define SERVICE_NAME "bluetoothd"

static int cmd_sk = -1;
static int notif_sk = -1;

static pthread_mutex_t cmd_sk_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t notif_th = 0;

static void notification_dispatch(struct hal_msg_hdr *msg, int fd)
{
	switch (msg->service_id) {
	case HAL_SERVICE_ID_BLUETOOTH:
		bt_notify_adapter(msg->opcode, msg->payload, msg->len);
		break;
	default:
		DBG("Unhandled notification service=%d opcode=0x%x",
						msg->service_id, msg->opcode);
		break;
	}
}

static void *notification_handler(void *data)
{
	struct msghdr msg;
	struct iovec iv;
	struct cmsghdr *cmsg;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	char buf[BLUEZ_HAL_MTU];
	struct hal_msg_hdr *hal_msg = (void *) buf;
	ssize_t ret;
	int fd;

	while (true) {
		memset(&msg, 0, sizeof(msg));
		memset(buf, 0, sizeof(buf));
		memset(cmsgbuf, 0, sizeof(cmsgbuf));

		iv.iov_base = hal_msg;
		iv.iov_len = sizeof(buf);

		msg.msg_iov = &iv;
		msg.msg_iovlen = 1;

		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);

		ret = recvmsg(notif_sk, &msg, 0);
		if (ret < 0) {
			error("Receiving notifications failed, aborting :%s",
							strerror(errno));
			exit(EXIT_FAILURE);
		}

		/* socket was shutdown */
		if (ret == 0) {
			if (cmd_sk == -1)
				break;

			error("Notification socket closed, aborting");
			exit(EXIT_FAILURE);
		}

		if (ret < (ssize_t) sizeof(*hal_msg)) {
			error("Too small notification (%zd bytes), aborting",
									ret);
			exit(EXIT_FAILURE);
		}

		if (hal_msg->opcode < HAL_MSG_MINIMUM_EVENT) {
			error("Invalid notification (0x%x), aborting",
							hal_msg->opcode);
			exit(EXIT_FAILURE);
		}

		if (ret != (ssize_t) (sizeof(*hal_msg) + hal_msg->len)) {
			error("Malformed notification(%zd bytes), aborting",
									ret);
			exit(EXIT_FAILURE);
		}

		fd = -1;

		/* Receive auxiliary data in msg */
		for (cmsg = CMSG_FIRSTHDR(&msg); !cmsg;
					cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET
					&& cmsg->cmsg_type == SCM_RIGHTS) {
				memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
				break;
			}
		}

		notification_dispatch(hal_msg, fd);
	}

	close(notif_sk);
	notif_sk = -1;

	DBG("exit");

	return NULL;
}

static int accept_connection(int sk)
{
	int err;
	struct pollfd pfd;
	int new_sk;

	memset(&pfd, 0 , sizeof(pfd));
	pfd.fd = sk;
	pfd.events = POLLIN;

	err = poll(&pfd, 1, CONNECT_TIMEOUT);
	if (err < 0) {
		err = errno;
		error("Failed to poll: %d (%s)", err, strerror(err));
		return -1;
	}

	if (err == 0) {
		error("bluetoothd connect timeout");
		return -1;
	}

	new_sk = accept(sk, NULL, NULL);
	if (new_sk < 0) {
		err = errno;
		error("Failed to accept socket: %d (%s)", err, strerror(err));
		return -1;
	}

	return new_sk;
}

bool hal_ipc_init(void)
{
	struct sockaddr_un addr;
	int sk;
	int err;

	sk = socket(AF_LOCAL, SOCK_SEQPACKET, 0);
	if (sk < 0) {
		err = errno;
		error("Failed to create socket: %d (%s)", err,
							strerror(err));
		return false;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;

	memcpy(addr.sun_path, BLUEZ_HAL_SK_PATH, sizeof(BLUEZ_HAL_SK_PATH));

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		err = errno;
		error("Failed to bind socket: %d (%s)", err, strerror(err));
		close(sk);
		return false;
	}

	if (listen(sk, 2) < 0) {
		err = errno;
		error("Failed to listen on socket: %d (%s)", err,
								strerror(err));
		close(sk);
		return false;
	}

	/* Start Android Bluetooth daemon service */
	property_set("ctl.start", SERVICE_NAME);

	cmd_sk = accept_connection(sk);
	if (cmd_sk < 0) {
		close(sk);
		return false;
	}

	notif_sk = accept_connection(sk);
	if (notif_sk < 0) {
		close(sk);
		close(cmd_sk);
		cmd_sk = -1;
		return false;
	}

	info("bluetoothd connected");

	close(sk);

	err = pthread_create(&notif_th, NULL, notification_handler, NULL);
	if (err < 0) {
		notif_th = 0;
		error("Failed to start notification thread: %d (%s)", -err,
							strerror(-err));
		close(cmd_sk);
		cmd_sk = -1;
		close(notif_sk);
		notif_sk = -1;
		return false;
	}

	return true;
}

void hal_ipc_cleanup(void)
{
	close(cmd_sk);
	cmd_sk = -1;

	shutdown(notif_sk, SHUT_RD);

	pthread_join(notif_th, NULL);
	notif_th = 0;
}

int hal_ipc_cmd(uint8_t service_id, uint8_t opcode, uint16_t len, void *param,
					size_t *rsp_len, void *rsp, int *fd)
{
	ssize_t ret;
	struct msghdr msg;
	struct iovec iv[2];
	struct hal_msg_hdr hal_msg;
	char cmsgbuf[CMSG_SPACE(sizeof(int))];
	struct hal_msg_rsp_error err;
	size_t err_len = sizeof(err);

	if (cmd_sk < 0) {
		error("Invalid cmd socket passed to hal_ipc_cmd, aborting");
		exit(EXIT_FAILURE);
	}

	if (!rsp || !rsp_len) {
		memset(&err, 0, sizeof(err));
		rsp_len = &err_len;
		rsp = &err;
	}

	memset(&msg, 0, sizeof(msg));
	memset(&hal_msg, 0, sizeof(hal_msg));

	hal_msg.service_id = service_id;
	hal_msg.opcode = opcode;
	hal_msg.len = len;

	iv[0].iov_base = &hal_msg;
	iv[0].iov_len = sizeof(hal_msg);

	iv[1].iov_base = param;
	iv[1].iov_len = len;

	msg.msg_iov = iv;
	msg.msg_iovlen = 2;

	pthread_mutex_lock(&cmd_sk_mutex);

	ret = sendmsg(cmd_sk, &msg, 0);
	if (ret < 0) {
		error("Sending command failed, aborting :%s", strerror(errno));
		pthread_mutex_unlock(&cmd_sk_mutex);
		exit(EXIT_FAILURE);
	}

	memset(&msg, 0, sizeof(msg));
	memset(&hal_msg, 0, sizeof(hal_msg));

	iv[0].iov_base = &hal_msg;
	iv[0].iov_len = sizeof(hal_msg);

	iv[1].iov_base = rsp;
	iv[1].iov_len = *rsp_len;

	msg.msg_iov = iv;
	msg.msg_iovlen = 2;

	if (fd) {
		memset(cmsgbuf, 0, sizeof(cmsgbuf));
		msg.msg_control = cmsgbuf;
		msg.msg_controllen = sizeof(cmsgbuf);
	}

	ret = recvmsg(cmd_sk, &msg, 0);
	if (ret < 0) {
		error("Receiving command response failed, aborting :%s",
							strerror(errno));
		pthread_mutex_unlock(&cmd_sk_mutex);
		exit(EXIT_FAILURE);
	}

	pthread_mutex_unlock(&cmd_sk_mutex);

	if (ret < (ssize_t) sizeof(hal_msg)) {
		error("Too small response received(%zd bytes), aborting", ret);
		exit(EXIT_FAILURE);
	}

	if (ret != (ssize_t) (sizeof(hal_msg) + hal_msg.len)) {
		error("Malformed response received(%zd bytes), aborting", ret);
		exit(EXIT_FAILURE);
	}

	if (hal_msg.opcode != opcode && hal_msg.opcode != HAL_MSG_OP_ERROR) {
		error("Invalid opcode received (%u vs %u), aborting",
						hal_msg.opcode, opcode);
		exit(EXIT_FAILURE);
	}

	if (hal_msg.opcode == HAL_MSG_OP_ERROR) {
		struct hal_msg_rsp_error *err = rsp;
		return err->status;
	}

	/* Receive auxiliary data in msg */
	if (fd) {
		struct cmsghdr *cmsg;

		*fd = -1;

		for (cmsg = CMSG_FIRSTHDR(&msg); !cmsg;
					cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level == SOL_SOCKET
					&& cmsg->cmsg_type == SCM_RIGHTS) {
				memcpy(fd, CMSG_DATA(cmsg), sizeof(int));
				break;
			}
		}
	}

	if (rsp_len)
		*rsp_len = hal_msg.len;

	return BT_STATUS_SUCCESS;
}
