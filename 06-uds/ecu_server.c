/*
 * ecu_server.c —— 一个最小的诊断 ECU
 *
 * 这个文件里最重要的一行不是 UDS 相关代码，而是传输层的封装：
 *
 *     struct ep 有两种实现：内核 CAN_ISOTP 和阶段 5 的手写版
 *
 * 上层（uds_handle）完全不知道自己跑在哪个传输层上。这正是分层
 * 的意义，也是我们花一个阶段手写 ISO-TP 的回报 —— 现在可以拿
 * 自己的实现去承载一个真实的协议，而不是只跑单元测试。
 *
 * 用法：
 *   ecu_server -i vxcan1               # 内核 ISO-TP
 *   ecu_server -i vxcan1 --user        # 阶段 5 的手写实现
 */
#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/isotp.h>

#include "isotp_user.h"
#include "uds.h"

#define REQ_ID	0x7E0		/* 收测试仪的请求 */
#define RESP_ID	0x7E8		/* 回测试仪 */

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------- 传输层封装 ---------- */
struct ep {
	int	user_mode;
	int	fd;
	struct isotp_chan uc;
};

static int ep_open(struct ep *e, const char *ifname,
		   canid_t rx_id, canid_t tx_id, int user_mode)
{
	memset(e, 0, sizeof(*e));
	e->user_mode = user_mode;

	if (user_mode) {
		if (isotp_open(&e->uc, ifname, tx_id, rx_id) < 0)
			return -1;
		return 0;
	}

	int s = socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP);

	if (s < 0) {
		fprintf(stderr, "[-] socket(CAN_ISOTP): %s（modprobe can-isotp）\n",
			strerror(errno));
		return -1;
	}

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = (int)if_nametoindex(ifname);
	addr.can_addr.tp.rx_id = rx_id;
	addr.can_addr.tp.tx_id = tx_id;

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[-] bind: %s\n", strerror(errno));
		close(s);
		return -1;
	}

	e->fd = s;
	return 0;
}

static ssize_t ep_recv(struct ep *e, void *buf, size_t n, int timeout_ms)
{
	if (e->user_mode)
		return isotp_recv(&e->uc, buf, n, timeout_ms);

	struct pollfd p = { .fd = e->fd, .events = POLLIN };
	int r = poll(&p, 1, timeout_ms);

	if (r == 0) {
		errno = ETIMEDOUT;
		return -1;
	}
	if (r < 0)
		return -1;

	return read(e->fd, buf, n);
}

static int ep_send(struct ep *e, const void *buf, size_t n)
{
	if (e->user_mode)
		return isotp_send(&e->uc, buf, n);

	ssize_t w = write(e->fd, buf, n);

	return (w == (ssize_t)n) ? 0 : -1;
}

static void hexdump(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02X ", p[i]);
}

int main(int argc, char *argv[])
{
	const char *ifname = "vxcan1";
	int user_mode = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			ifname = argv[++i];
		} else if (!strcmp(argv[i], "--user")) {
			user_mode = 1;
		} else {
			fprintf(stderr,
				"用法: %s [-i 接口] [--user]\n"
				"  --user  使用阶段 5 的手写 ISO-TP，而不是内核栈\n",
				argv[0]);
			return 1;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct ep e;

	if (ep_open(&e, ifname, REQ_ID, RESP_ID, user_mode) < 0)
		return 1;

	struct uds_ctx ctx;

	uds_init(&ctx);

	printf("诊断 ECU 就绪  接口 %s  收 0x%03X  回 0x%03X  传输层=%s\n",
	       ifname, REQ_ID, RESP_ID,
	       user_mode ? "手写 ISO-TP" : "内核 CAN_ISOTP");
	printf("VIN=%.17s  零件号=%s\n\n", ctx.vin, ctx.part);

	uint8_t req[256], resp[256];
	unsigned long seq = 0;

	while (!g_stop) {
		ssize_t n = ep_recv(&e, req, sizeof(req), 200);
		long long now = now_ms();

		if (n > 0) {
			seq++;
			printf("[%3lu] 收 %2zd 字节  ", seq, n);
			hexdump(req, (size_t)n);
			printf("\n");

			int r = uds_handle(&ctx, req, (size_t)n, resp,
					   sizeof(resp), now);

			if (r > 0) {
				if (ep_send(&e, resp, (size_t)r) < 0)
					printf("      [!] 发送失败: %s\n",
					       strerror(errno));

				printf("      发 %2d 字节  ", r);
				hexdump(resp, (size_t)r);

				if (resp[0] == 0x7F)
					printf("  <- %s / %s",
					       uds_sid_name(resp[1]),
					       uds_nrc_name(resp[2]));
				else
					printf("  <- %s  会话=%s%s",
					       uds_sid_name(req[0]),
					       uds_session_name(ctx.session),
					       ctx.security_unlocked ?
					       " 已解锁" : "");
				printf("\n");
			} else {
				printf("      （按 suppress 位要求，不回响应）\n");
			}
		} else if (n < 0 && errno != ETIMEDOUT && errno != EINTR) {
			printf("[-] 接收失败: %s\n", strerror(errno));
			break;
		}

		if (uds_check_s3(&ctx, now))
			printf("    ** S3 超时（%d ms 无请求），自动退回默认会话 **\n",
			       UDS_S3_MS);

		/* 模拟活数据：让 0x22 0x0100 每次读到的转速不一样 */
		ctx.rpm = (uint32_t)(800 + (now / 100) % 400);
	}

	printf("\n=== 统计 ===\n");
	printf("  请求 %lu 条，负响应 %lu 条\n", ctx.n_req, ctx.n_nrc);
	printf("  最终会话 %s，安全%s\n",
	       uds_session_name(ctx.session),
	       ctx.security_unlocked ? "已解锁" : "未解锁");

	if (user_mode) {
		printf("  传输层：TX %lu 帧 / RX %lu 帧，空闲轮询超时 %lu 次（正常）\n",
		       e.uc.n_tx_frames, e.uc.n_rx_frames, e.uc.n_timeout);
		isotp_close(&e.uc);
	} else {
		close(e.fd);
	}

	return 0;
}
