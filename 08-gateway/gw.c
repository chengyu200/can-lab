/*
 * gw.c —— 双段 CAN 网关：ID 路由、协议转换、断链处理
 *
 * 为什么网关值得单独做一个阶段？
 *   前面几个阶段都是「一条总线上的节点」。真实车上不是这样：
 *   动力总成、车身、信息娱乐各自是一条总线，速度和协议都不同，
 *   中间靠网关（CGW / 中央网关）转接。网关一旦挂掉，
 *   仪表就再也看不到发动机数据 —— 这是整车级的单点。
 *
 * 本程序做三件真实网关一定会做的事：
 *
 * 1. **白名单路由**。只转发规则表里列出的 ID，其余全部丢弃并计数。
 *    网关必须能回答「我丢了多少」，所以这里**不用**内核过滤器：
 *    内核丢了之后用户态就看不见了，账对不上。
 *    （代价是 CPU；量产网关会用硬件过滤 + 计数寄存器来两全。）
 *
 * 2. **协议转换**。A 段的私有报文 0x180 在 B 段变成 J1939 的 EEC1（PGN 0xF004）。
 *    这不是简单转发：要按 DBC 解出物理量，再按 J1939-71 的编码重新打包。
 *    量纲也不一样 —— 0x180 的转速是 0.25 rpm/bit，EEC1 是 0.125 rpm/bit。
 *    这类「同名不同码」是网关里最容易出错的地方。
 *
 * 3. **断链处理**。B 段 down 掉之后，write 返回 ENETDOWN。
 *    网关不能退出，也不能把错误当致命：A 段还在正常跑，
 *    网关要继续接收、继续计数，等 B 段回来再恢复。
 *    这和阶段 4 的 link_watch 不同 —— 那里接口是被删了，要重新 bind；
 *    这里接口还在，只是不通，socket 不需要重建。
 *
 * 用法：
 *   gw <A段接口> <B段接口> [--stats 秒] [--trace N | --verbose]
 *
 * 拓扑（topo.sh 会建好）：
 *
 *   [engine]---vxcan0 ~~ vxcan1---[gw]---vxcan2 ~~ vxcan3---[j1939 接收端]
 *              A 段（私有 CAN）              B 段（J1939 语义）
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
#include <linux/can/raw.h>

#include "can_proto.h"

/* J1939 EEC1 的 29 位 ID：优先级 6 | PF=0xF0 | PS=0x04(组扩展) | SA=0x00 */
#define EEC1_ID  ((6u << 26) | (0xF0u << 16) | (0x04u << 8) | 0x00u)

enum dir { DIR_A2B, DIR_B2A };
enum fwd_kind { FWD_COPY, FWD_REMAP, FWD_EEC1 };

struct rule {
	enum dir       dir;
	canid_t        from;
	enum fwd_kind  kind;
	canid_t        to;	/* FWD_REMAP 用 */
	const char    *note;
	unsigned long  n_in;
	unsigned long  n_out;
};

static struct rule g_rules[] = {
	{ .dir = DIR_A2B, .from = CAN_ID_ENGINE_FAST, .kind = FWD_EEC1,
	  .note = "0x180 ENGINE_FAST -> J1939 EEC1 (0xF004)" },
	{ .dir = DIR_A2B, .from = CAN_ID_ENGINE_SLOW, .kind = FWD_REMAP, .to = 0x281,
	  .note = "0x181 ENGINE_SLOW -> 0x281 透传（ID 重映射）" },
	{ .dir = DIR_B2A, .from = CAN_ID_DASH_CMD, .kind = FWD_COPY,
	  .note = "0x200 DASH_CMD -> 原样回 A 段" },
};

#define NRULES (sizeof(g_rules) / sizeof(g_rules[0]))

static volatile sig_atomic_t g_stop;
/*
 * 转换明细的打印额度：-1 = 不限，0 = 不打印，正数 = 只打印前 N 条。
 * 用额度而不是布尔开关，是因为 10 ms 周期的报文一秒就是 100 行，
 * 直接开 verbose 会把终端刷掉，反而看不见想看的那几条。
 */
static long g_trace;

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

static int open_raw(const char *ifname)
{
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	struct sockaddr_can a;
	unsigned idx = if_nametoindex(ifname);

	if (s < 0) {
		fprintf(stderr, "[-] socket: %s\n", strerror(errno));
		return -1;
	}
	if (!idx) {
		fprintf(stderr, "[-] 接口 %s 不存在\n", ifname);
		close(s);
		return -1;
	}

	memset(&a, 0, sizeof(a));
	a.can_family  = AF_CAN;
	a.can_ifindex = (int)idx;

	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		fprintf(stderr, "[-] bind %s: %s\n", ifname, strerror(errno));
		close(s);
		return -1;
	}
	return s;
}

/*
 * 协议转换的核心：解私有信号 -> 按 J1939 量纲重打包。
 * 注意两处「单位陷阱」：
 *   转速  0x180: 0.25 rpm/bit        EEC1: 0.125 rpm/bit
 *   扭矩  0x180: 百分比 0..100       EEC1: 1%/bit，偏移 -125（范围 -125..125）
 * 直接搬字节会得到 2 倍转速和离谱的扭矩，而且看起来「有数」，最难查。
 */
static int translate_eec1(const struct can_frame *in, struct can_frame *out)
{
	const struct can_msg_def *m = proto_lookup(CAN_ID_ENGINE_FAST);
	double phys[CAN_MAX_SIGNALS];
	unsigned raw_rpm;
	int thr, load;

	if (!m || in->can_dlc < m->dlc)
		return -EINVAL;

	proto_decode(m, in->data, phys);

	raw_rpm = (unsigned)(phys[EF_RPM] / 0.125 + 0.5);
	thr  = (int)(phys[EF_THROTTLE] + 0.5);
	load = (int)(phys[EF_LOAD] + 0.5);

	if (raw_rpm > 0xFFFF)
		raw_rpm = 0xFFFF;
	if (thr < -125)  thr = -125;
	if (thr > 125)   thr = 125;
	if (load < -125) load = -125;
	if (load > 125)  load = 125;

	memset(out, 0, sizeof(*out));
	out->can_id  = EEC1_ID | CAN_EFF_FLAG;
	out->can_dlc = 8;
	out->data[1] = (uint8_t)(thr + 125);	/* 驾驶员需求扭矩 */
	out->data[2] = (uint8_t)(load + 125);	/* 实际扭矩 */
	out->data[3] = (uint8_t)(raw_rpm & 0xFF);	/* 转速低字节 */
	out->data[4] = (uint8_t)(raw_rpm >> 8);		/* 转速高字节 */
	out->data[5] = 0x00;			/* 控制该参数的 ECU 地址 */
	out->data[6] = 0x00;
	out->data[7] = 0x00;
	return 0;
}

struct seg {
	const char *name;
	int fd;
	int up;			/* 上一次写成功过就算 up */
	unsigned long rx, tx_ok, tx_err, tx_drop;
	long long last_err_ms;
};

static int send_one(struct seg *to, const struct can_frame *f)
{
	ssize_t n = write(to->fd, f, sizeof(*f));

	if (n == (ssize_t)sizeof(*f)) {
		to->tx_ok++;
		if (!to->up) {
			to->up = 1;
			printf("[+] %s 段恢复，开始转发\n", to->name);
			fflush(stdout);
		}
		return 0;
	}

	to->tx_err++;
	if (n < 0) {
		int e = errno;

		/* 链路 down 不是致命错误：A 段还在跑，等 B 段回来就行 */
		if (e == ENETDOWN || e == ENXIO || e == ENOBUFS) {
			if (to->up) {
				to->up = 0;
				printf("[-] %s 段不可用（%s），暂停转发，继续接收\n",
				       to->name, strerror(e));
				fflush(stdout);
			}
			to->tx_drop++;
			return -e;
		}
		fprintf(stderr, "[!] 写 %s 段失败: %s\n", to->name, strerror(e));
		return -e;
	}
	return -EIO;
}

/* 处理一帧：查规则 -> 转换 -> 发到另一段 */
static void handle(struct seg *from, struct seg *to, struct can_frame *f,
		   enum dir dir)
{
	canid_t id = f->can_id & CAN_SFF_MASK;

	for (size_t i = 0; i < NRULES; i++) {
		struct rule *r = &g_rules[i];
		struct can_frame out;

		if (r->dir != dir || r->from != id)
			continue;

		r->n_in++;

		switch (r->kind) {
		case FWD_COPY:
			out = *f;
			break;
		case FWD_REMAP:
			out = *f;
			out.can_id = r->to;
			break;
		case FWD_EEC1:
			if (translate_eec1(f, &out) < 0) {
				if (g_trace == -1)
					printf("[skip] 0x%03X 长度不足，无法转换\n", id);
				return;
			}
			break;
		}

		if (g_trace != 0) {
			/*
			 * 同时打出「原始字节」和「目标字节」，这是本阶段最该盯着看的地方：
			 * 同一个物理量在两种协议里字节完全不同，凭直觉搬字节必错。
			 */
			printf("[%s->%s] 0x%03X ", from->name, to->name, id);
			for (int i = 0; i < f->can_dlc && i < 8; i++)
				printf("%02X ", f->data[i]);

			printf(" -> 0x%08X ", (unsigned)(out.can_id & CAN_EFF_MASK));
			for (int i = 0; i < out.can_dlc && i < 8; i++)
				printf("%02X ", out.data[i]);

			if (r->kind == FWD_EEC1) {
				double phys[CAN_MAX_SIGNALS];
				const struct can_msg_def *m = proto_lookup(id);
				unsigned raw = (unsigned)out.data[3] |
					       ((unsigned)out.data[4] << 8);

				if (m) {
					proto_decode(m, f->data, phys);
					printf("   rpm %.1f -> %.1f", phys[EF_RPM],
					       raw * 0.125);
				}
			}
			printf("\n");

			if (g_trace > 0)
				g_trace--;
		}

		if (send_one(to, &out) == 0)
			r->n_out++;
		return;
	}

	/* 不在白名单：丢弃并计数，但每秒汇总一次就够，避免刷屏 */
	from->tx_err++;
	if (g_trace == -1)
		printf("[drop] 0x%03X 不在路由表\n", id);
}

static void print_stats(struct seg *a, struct seg *b, long long uptime)
{
	char tbuf[32];
	struct tm tm;
	time_t t = time(NULL);
	unsigned long fwd = 0;

	localtime_r(&t, &tm);
	strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tm);
	printf("\n--- 统计 @%s（运行 %lld s）---\n", tbuf, uptime / 1000);
	printf("  A段 %-8s rx=%-6lu tx_ok=%-6lu tx_err=%-4lu 状态=%s\n",
	       a->name, a->rx, a->tx_ok, a->tx_err, a->up ? "UP" : "DOWN");
	printf("  B段 %-8s rx=%-6lu tx_ok=%-6lu tx_err=%-4lu 状态=%s\n",
	       b->name, b->rx, b->tx_ok, b->tx_err, b->up ? "UP" : "DOWN");

	for (size_t i = 0; i < NRULES; i++) {
		fwd += g_rules[i].n_out;
		printf("  规则 %-46s 进 %-6lu 出 %-6lu\n",
		       g_rules[i].note, g_rules[i].n_in, g_rules[i].n_out);
	}
	printf("  合计转发 %lu 帧\n", fwd);
	fflush(stdout);
}

int main(int argc, char *argv[])
{
	if (argc < 3) {
		fprintf(stderr,
			"用法: %s <A段接口> <B段接口> [--stats 秒] [--trace N] [--verbose]\n",
			argv[0]);
		return 1;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	const char *ifa = argv[1];
	const char *ifb = argv[2];
	int stats_sec = 5;

	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--stats") && i + 1 < argc)
			stats_sec = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--trace") && i + 1 < argc)
			g_trace = atol(argv[++i]);
		else if (!strcmp(argv[i], "--verbose"))
			g_trace = -1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	struct seg A = { .name = ifa, .up = 1 };
	struct seg B = { .name = ifb, .up = 1 };

	A.fd = open_raw(ifa);
	B.fd = open_raw(ifb);
	if (A.fd < 0 || B.fd < 0)
		return 1;

	printf("=== 双段网关 ===\n");
	printf("  A 段 %s  <->  B 段 %s   统计周期 %d s\n", ifa, ifb, stats_sec);
	printf("  路由表：\n");
	for (size_t i = 0; i < NRULES; i++)
		printf("    %s  %s\n",
		       g_rules[i].dir == DIR_A2B ? "A->B" : "B->A",
		       g_rules[i].note);
	printf("\n");

	long long t0 = now_ms();
	long long next_stats = t0 + (long long)stats_sec * 1000;

	while (!g_stop) {
		struct pollfd pfd[2] = {
			{ .fd = A.fd, .events = POLLIN },
			{ .fd = B.fd, .events = POLLIN },
		};

		int pr = poll(pfd, 2, 200);

		if (pr > 0) {
			for (int i = 0; i < 2; i++) {
				if (!(pfd[i].revents & POLLIN))
					continue;

				struct seg *from = (i == 0) ? &A : &B;
				struct seg *to   = (i == 0) ? &B : &A;
				enum dir dir = (i == 0) ? DIR_A2B : DIR_B2A;

				/* 一次唤醒把队列抽干，避免每帧一次 poll */
				for (;;) {
					struct can_frame f;
					ssize_t n = recv(from->fd, &f, sizeof(f),
							 MSG_DONTWAIT);

					if (n != (ssize_t)sizeof(f))
						break;

					from->rx++;

					/* 错误帧、远程帧不转发 */
					if (f.can_id & (CAN_ERR_FLAG | CAN_RTR_FLAG))
						continue;

					handle(from, to, &f, dir);
				}
			}
		}

		long long now = now_ms();

		if (stats_sec > 0 && now >= next_stats) {
			print_stats(&A, &B, now - t0);
			next_stats = now + (long long)stats_sec * 1000;
		}
	}

	print_stats(&A, &B, now_ms() - t0);
	close(A.fd);
	close(B.fd);
	return 0;
}
