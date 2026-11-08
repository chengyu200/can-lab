/*
 * j1939_node.c —— 一个完整的 J1939 节点：声明地址、收 PGN、发 PGN
 *
 * 这个程序覆盖三件事：
 *
 * 1. PDU1 与 PDU2 的区别（由 PF 决定，见 j1939_util.h 的注释）
 *      --pgn 0x0EA00 --dest 0x80   点对点，PS 是目标地址
 *      --pgn 0x0F004 --dest 0xFF   广播，PS 是组扩展
 *    同样的发送代码，内核会拼出完全不同的 29 位 ID。
 *
 * 2. 传输协议（TP）。J1939 一帧只有 8 字节，超过就要分片，
 *    而分片方式由「目标地址」自动决定 —— 这是和 ISO-TP 最大的不同：
 *
 *      DA = 0xFF（广播） -> BAM：不协商、不等流控、固定间隔猛发
 *      DA = 单播        -> RTS/CTS：先问对方能收多少，再按窗口发
 *
 *    所以想观察 BAM 和 RTS/CTS 的差异，只需要改 --dest 一个参数。
 *
 * 3. 接收端不用关心分片。内核把 TP 会话重组完才交给用户态，
 *     recvmsg 拿到的是整条消息（可能几百字节）。
 *     —— 这一点和阶段 5 的 ISO-TP 完全一致：传输层对应用层是透明的。
 *
 * 用法：
 *   j1939_node <接口> <地址> <NAME> [选项]
 *
 *   --pgn N       发送的 PGN（默认 0xF004，EEC1 发动机转速）
 *   --dest N      目标地址（默认 0xFF 广播）
 *   --size N      每次发送 N 字节（默认 8；>8 自动走 TP）
 *   --period N    发送周期 ms（默认 1000，0 表示只发一次）
 *   --count N     发送次数（默认 0 = 不限）
 *   --rx-pgn N    只收指定 PGN（默认 0 = 全收）
 *   --rx-only     只收不发（做接收端用）
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/j1939.h>

#include "j1939_util.h"

static volatile sig_atomic_t g_stop;

static void on_sig(int sig)
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

static int j1939_open(const char *ifname, uint64_t name, uint8_t sa)
{
	int s = socket(PF_CAN, SOCK_DGRAM, CAN_J1939);
	int one = 1;

	if (s < 0) {
		fprintf(stderr, "[-] socket(CAN_J1939): %s\n", strerror(errno));
		return -1;
	}

	struct sockaddr_can a;

	memset(&a, 0, sizeof(a));
	a.can_family  = AF_CAN;
	a.can_ifindex = (int)if_nametoindex(ifname);
	a.can_addr.j1939.name = name;
	a.can_addr.j1939.pgn  = J1939_NO_PGN;
	a.can_addr.j1939.addr = sa;

	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		fprintf(stderr, "[-] bind %s: %s\n", ifname, strerror(errno));
		close(s);
		return -1;
	}
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	return s;
}

static int j1939_send(int s, uint32_t pgn, uint8_t da,
		      const uint8_t *data, size_t len)
{
	struct sockaddr_can a;

	memset(&a, 0, sizeof(a));
	a.can_family = AF_CAN;
	a.can_addr.j1939.pgn  = pgn;
	a.can_addr.j1939.addr = da;

	return (int)sendto(s, data, len, 0, (struct sockaddr *)&a, sizeof(a));
}

static int j1939_claim(int s, uint64_t name)
{
	uint8_t d[8];

	memcpy(d, &name, sizeof(d));
	return j1939_send(s, PGN_ADDRESS_CLAIM, J1939_NO_ADDR, d, sizeof(d));
}

/*
 * EEC1（PGN 0xF004）的关键字段，用来验证「发出去的东西是不是真能被看懂」：
 *   byte 3-4：发动机转速，0.125 rpm/bit，小端
 *   按 SAE J1939-71 的定义，这里不是随便挑的两个字节。
 */
static void decode_eec1(const uint8_t *d, size_t n, char *out, size_t len)
{
	if (n < 5) {
		snprintf(out, len, "(短帧)");
		return;
	}

	unsigned raw = (unsigned)d[3] | ((unsigned)d[4] << 8);
	unsigned x1000 = raw % 8 * 125;

	snprintf(out, len, "rpm=%u.%03u  需求扭矩=%d%%  实际扭矩=%d%%",
		 raw / 8, x1000, (int)d[1] - 125, (int)d[2] - 125);
}

int main(int argc, char *argv[])
{
	if (argc < 4) {
		fprintf(stderr,
			"用法: %s <接口> <地址> <NAME> [--pgn N] [--dest N] "
			"[--size N] [--period N] [--count N] [--rx-pgn N] [--rx-only]\n",
			argv[0]);
		return 1;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	const char *ifname = argv[1];
	unsigned sa   = (unsigned)strtoul(argv[2], NULL, 0);
	uint64_t name = strtoull(argv[3], NULL, 0);

	uint32_t tx_pgn  = PGN_EEC1;
	unsigned dest    = J1939_NO_ADDR;
	size_t   size    = 8;
	int      period  = 1000;
	int      count   = 0;
	uint32_t rx_pgn  = 0;
	int      rx_only = 0;

	for (int i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "--pgn") && i + 1 < argc)
			tx_pgn = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dest") && i + 1 < argc)
			dest = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc)
			size = (size_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--period") && i + 1 < argc)
			period = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--count") && i + 1 < argc)
			count = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--rx-pgn") && i + 1 < argc)
			rx_pgn = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--rx-only"))
			rx_only = 1;
	}

	if (size > 1785) {
		fprintf(stderr, "[-] 单条 TP 消息上限 1785 字节（255 × 7）\n");
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	int s = j1939_open(ifname, name, (uint8_t)sa);

	if (s < 0)
		return 1;

	/* 收端过滤：pgn_mask 全 1 表示精确匹配，0 表示不关心 */
	if (rx_pgn) {
		struct j1939_filter f;

		memset(&f, 0, sizeof(f));
		f.pgn      = rx_pgn;
		f.pgn_mask = 0x3FFFF;
		setsockopt(s, SOL_CAN_J1939, SO_J1939_FILTER, &f, sizeof(f));
	}

	printf("=== J1939 节点 ===\n");
	printf("  接口 %s  SA=0x%02X  NAME=0x%016llX\n",
	       ifname, sa, (unsigned long long)name);
	printf("  发送 PGN=0x%05X(%s)  DA=0x%02X(%s)  size=%zu  period=%d ms\n",
	       tx_pgn, j1939_pgn_name(tx_pgn), dest, j1939_addr_str(dest),
	       size, period);
	if (rx_only) {
		printf("  只接收，不发送\n");
	} else {
		printf("  分片方式：%s\n",
		       dest == J1939_NO_ADDR ? "广播 -> BAM（不协商、固定间隔）"
					     : "单播 -> RTS/CTS（先协商窗口）");
	}
	if (rx_pgn)
		printf("  只接收 PGN=0x%05X\n", rx_pgn);
	else
		printf("  接收全部 PGN\n");
	printf("\n");

	if (j1939_claim(s, name) < 0) {
		printf("[!] 地址声明失败: %s\n", strerror(errno));
	} else {
		/* 规范要求声明后等 250 ms 再开始业务传输，避免仲裁期抢总线 */
		printf("已声明地址，等待 250 ms\n");
		usleep(250 * 1000);
	}

	uint8_t *buf = calloc(1, size ? size : 1);

	if (!buf) {
		perror("calloc");
		close(s);
		return 1;
	}

	/* 填一个可辨认的图案：首 8 字节是 0..7，之后每 7 字节一个递增序号 */
	for (size_t i = 0; i < size; i++)
		buf[i] = (uint8_t)(i & 0xFF);

	long long next_tx = now_ms();
	int sent = 0;
	unsigned long rx_total = 0, rx_tp = 0;

	while (!g_stop) {
		long long now = now_ms();
		int wait = 200;
		int do_tx = (!rx_only && period >= 0 && now >= next_tx &&
			     (count == 0 || sent < count));

		if (do_tx)
			wait = 0;

		struct pollfd pfd = { .fd = s, .events = POLLIN };
		int pr = poll(&pfd, 1, wait);

		if (pr > 0 && (pfd.revents & POLLIN)) {
			uint8_t rbuf[2048];
			struct sockaddr_can from;
			struct iovec iov = { rbuf, sizeof(rbuf) };
			char ctrl[128];
			struct msghdr msg;
			struct cmsghdr *cm;
			unsigned da = J1939_NO_ADDR;

			memset(&msg, 0, sizeof(msg));
			memset(&from, 0, sizeof(from));
			msg.msg_name    = &from;
			msg.msg_namelen = sizeof(from);
			msg.msg_iov     = &iov;
			msg.msg_iovlen  = 1;
			msg.msg_control = ctrl;
			msg.msg_controllen = sizeof(ctrl);

			ssize_t n = recvmsg(s, &msg, 0);

			if (n < 0)
				continue;

			for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm))
				if (cm->cmsg_level == SOL_CAN_J1939 &&
				    cm->cmsg_type == SCM_J1939_DEST_ADDR)
					da = *(uint8_t *)CMSG_DATA(cm);

			uint32_t pgn = from.can_addr.j1939.pgn;
			unsigned ssa = from.can_addr.j1939.addr;

			rx_total++;
			if (n > 8)
				rx_tp++;

			if (pgn == PGN_ADDRESS_CLAIM)
				continue;	/* 地址声明在 addr 程序里讲过了 */

			printf("[RX #%lu] PGN=0x%05X(%-14s) SA=0x%02X DA=0x%02X len=%zd%s",
			       rx_total, pgn, j1939_pgn_name(pgn), ssa, da, n,
			       n > 8 ? " [TP 重组]" : "");

			if (pgn == PGN_EEC1) {
				char d[80];

				decode_eec1(rbuf, (size_t)n, d, sizeof(d));
				printf("  %s", d);
			} else if (n <= 16) {
				printf("  ");
				for (ssize_t i = 0; i < n; i++)
					printf("%02X ", rbuf[i]);
			}
			printf("\n");
			continue;
		}

		if (do_tx) {
			/* 让数据每秒变化，接收端能看出这是活的数据而不是缓存 */
			if (size >= 5) {
				unsigned raw = (unsigned)(800 + sent) * 8;

				buf[3] = (uint8_t)(raw & 0xFF);
				buf[4] = (uint8_t)(raw >> 8);
			}

			long long t0 = now_ms();
			int r = j1939_send(s, tx_pgn, (uint8_t)dest, buf, size);
			long long dt = now_ms() - t0;

			sent++;

			if (r < 0) {
				printf("[TX %d] 失败: %s\n", sent, strerror(errno));
			} else if (size > 8) {
				/*
				 * BAM 的耗时是可以算的：每包 7 字节，内核按固定间隔发。
				 * 打印实测值，和下面 lab7.sh 里抓到的原始帧对照。
				 */
				printf("[TX %d] %zu 字节 -> %d 包，耗时 %lld ms\n",
				       sent, size, (int)((size + 6) / 7), dt);
			} else {
				printf("[TX %d] %zu 字节，耗时 %lld ms\n",
				       sent, size, dt);
			}

			if (count && sent >= count) {
				/* 发完最后一条，留点时间给接收端，然后退出 */
				usleep(300 * 1000);
				break;
			}
			next_tx = now + (period > 0 ? period : 1000000);
		}
	}

	printf("\n=== 统计 ===\n");
	printf("  发送 %d 条，接收 %lu 条（其中 %lu 条经 TP 重组）\n",
	       sent, rx_total, rx_tp);

	free(buf);
	close(s);
	return 0;
}
