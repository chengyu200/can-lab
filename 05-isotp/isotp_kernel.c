/*
 * isotp_kernel.c —— 同一件事，交给内核做
 *
 * 与 isotp_user.c 对照着看：
 *
 *   用户态版：自己拼 FF/CF、自己等 FC、自己守 STmin、自己校验序号
 *   内核版  ：socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP) 之后，write 就是
 *             「一条完整消息」，read 拿到的也是「一条完整消息」
 *
 * 中间那 400 行全部由 net/can/isotp.c 承担。这个程序存在的意义
 * 不是"更短的写法"，而是给你一把尺子：量出内核替你做了多少事，
 * 以及这些事在什么条件下会出问题。
 *
 * 模式：
 *   send : 发一条指定长度的消息，打印耗时
 *   recv : 收一条消息并校验内容，可用 --bs/--stmin 控制回给对端的流控
 */
#define _GNU_SOURCE

#include <errno.h>
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

#define MAXLEN	4096

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void fill(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t)(i * 7 + 3);
}

static uint8_t checksum(const uint8_t *p, size_t n)
{
	uint8_t s = 0;

	for (size_t i = 0; i < n; i++)
		s = (uint8_t)(s + p[i]);
	return s;
}

static int open_isotp(const char *ifname, canid_t rx_id, canid_t tx_id,
		      int bs, int stmin)
{
	int s = socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP);

	if (s < 0) {
		fprintf(stderr, "[-] socket(CAN_ISOTP) 失败: %s\n"
			"    模块加载了吗？modprobe can-isotp\n",
			strerror(errno));
		return -1;
	}

	/*
	 * 顺序要紧：流控参数必须在 bind 之前设。
	 * bind 之后 ISO-TP 连接就已经建立，再改 setsockopt 会返回
	 * EISCONN（Transport endpoint is already connected）。
	 * 这个坑在文档里不显眼，但第一次写必然踩到。
	 */
	if (bs >= 0 || stmin >= 0) {
		struct can_isotp_fc_options fco;

		memset(&fco, 0, sizeof(fco));
		fco.bs     = (uint8_t)(bs < 0 ? 0 : bs);
		fco.stmin  = (uint8_t)(stmin < 0 ? 0 : stmin);
		fco.wftmax = 0;

		if (setsockopt(s, SOL_CAN_ISOTP, CAN_ISOTP_RECV_FC,
			       &fco, sizeof(fco)) < 0) {
			fprintf(stderr, "[-] 设置流控参数失败: %s\n",
				strerror(errno));
			close(s);
			return -1;
		}

		printf("本端流控：BS=%u  STmin=0x%02X(%d ms)\n",
		       fco.bs, fco.stmin, fco.stmin);
	}

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = (int)if_nametoindex(ifname);
	addr.can_addr.tp.rx_id = rx_id;
	addr.can_addr.tp.tx_id = tx_id;

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[-] bind 失败: %s\n", strerror(errno));
		close(s);
		return -1;
	}

	return s;
}

static int do_send(const char *ifname, canid_t rx_id, canid_t tx_id,
		   size_t len, int repeat)
{
	int s = open_isotp(ifname, rx_id, tx_id, -1, -1);

	if (s < 0)
		return 1;

	uint8_t *buf = malloc(len);

	if (!buf) {
		close(s);
		return 1;
	}

	fill(buf, len);

	printf("内核 ISO-TP 发送  接口 %s  0x%03X -> 0x%03X  %zu 字节 × %d\n\n",
	       ifname, tx_id, rx_id, len, repeat);

	int fail = 0;

	for (int i = 0; i < repeat; i++) {
		long long t0 = now_us();
		ssize_t w = write(s, buf, len);
		long long dt = now_us() - t0;

		if (w < 0) {
			printf("  [%d] 失败: %s（%.2f ms）\n", i,
			       strerror(errno), dt / 1000.0);
			fail++;
		} else {
			printf("  [%d] write 返回 %zd  %.2f ms  %.1f KB/s\n",
			       i, w, dt / 1000.0,
			       dt > 0 ? (double)len * 1000000.0 / dt / 1024.0 : 0.0);
		}
	}

	free(buf);
	close(s);
	return fail ? 1 : 0;
}

static int do_recv(const char *ifname, canid_t rx_id, canid_t tx_id,
		   int bs, int stmin, int count, int timeout_ms)
{
	int s = open_isotp(ifname, rx_id, tx_id, bs, stmin);

	if (s < 0)
		return 1;

	printf("内核 ISO-TP 接收  接口 %s  等 0x%03X，回 0x%03X\n\n",
	       ifname, rx_id, tx_id);

	uint8_t *buf = malloc(MAXLEN);

	if (!buf) {
		close(s);
		return 1;
	}

	int fail = 0;

	for (int i = 0; i < count; i++) {
		struct pollfd p = { .fd = s, .events = POLLIN };
		int r = poll(&p, 1, timeout_ms);

		if (r <= 0) {
			printf("  [%d] 超时（%d ms 内没有完整消息）\n",
			       i, timeout_ms);
			fail++;
			continue;
		}

		/*
		 * 注意这里的差别：read 一次就拿到整条消息。
		 * 内核已经替我们收完所有 CF、校验了序号、回了 FC。
		 */
		ssize_t n = read(s, buf, MAXLEN);

		if (n < 0) {
			printf("  [%d] 失败: %s\n", i, strerror(errno));
			fail++;
			continue;
		}

		uint8_t expect = checksum(buf, (size_t)n);
		uint8_t want = 0;

		/* 与发送端同样的生成算法，用来验证内容 */
		for (ssize_t k = 0; k < n; k++)
			want = (uint8_t)(want + (uint8_t)(k * 7 + 3));

		printf("  [%d] 收到 %zd 字节  和=%02X %s  首 8 字节 ",
		       i, n, expect, expect == want ? "OK" : "MISMATCH");

		for (ssize_t k = 0; k < n && k < 8; k++)
			printf("%02X ", buf[k]);
		printf("\n");

		if (expect != want)
			fail++;
	}

	free(buf);
	close(s);
	return fail ? 1 : 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"用法:\n"
		"  %s send -i 接口 [-s rx_id] [-d tx_id] [-l 长度] [-n 次数]\n"
		"  %s recv -i 接口 [-s rx_id] [-d tx_id] [--bs N] [--stmin N]\n"
		"          [-n 条数] [-t 超时ms]\n"
		"\n"
		"  -s 是本端接收用的 ID，-d 是本端发送用的 ID。\n"
		"  两个端点互填：发送端 -s 0x701 -d 0x700，接收端 -s 0x700 -d 0x701。\n",
		prog, prog);
}

int main(int argc, char *argv[])
{
	const char *ifname = "vxcan0";
	canid_t rx_id = 0x700, tx_id = 0x701;
	size_t len = 100;
	int repeat = 1, count = 1, timeout_ms = 5000;
	int bs = -1, stmin = -1;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	const char *mode = argv[1];

	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			ifname = argv[++i];
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			rx_id = (canid_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			tx_id = (canid_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
			len = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			count = atoi(argv[++i]);
			repeat = count;
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			timeout_ms = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--bs") && i + 1 < argc) {
			bs = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--stmin") && i + 1 < argc) {
			stmin = (int)strtol(argv[++i], NULL, 0);
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (len == 0 || len > MAXLEN) {
		fprintf(stderr, "长度必须在 1..%d\n", MAXLEN);
		return 1;
	}

	if (!strcmp(mode, "send"))
		return do_send(ifname, rx_id, tx_id, len, repeat);
	if (!strcmp(mode, "recv"))
		return do_recv(ifname, rx_id, tx_id, bs, stmin, count, timeout_ms);

	usage(argv[0]);
	return 1;
}
