/*
 * 阶段 2 —— 内核过滤 + poll 复用 + 用户态分发（can_router）
 *
 * 阶段 1 是「一收一发」，阶段 2 要回答三个真实问题：
 *
 *   1) 总线上几百种报文，我的进程凭什么不被淹掉？
 *      -> can_filter：让内核在 skb 进套接字队列之前就丢掉不关心的 ID。
 *         过滤器在驱动/内核里执行，不消耗用户态 CPU；不装过滤器的话，
 *         哪怕你 read() 得再快，队列也会被打满（ENOBUFS）。
 *
 *   2) 一个进程怎么同时处理多路报文？
 *      -> 一个套接字装多个过滤器（多个 filter 之间是「或」），
 *         再用 poll() 等数据到达，一次唤醒批量把队列抽干。
 *
 *   3) 分发怎么写才不像一堆 if/else？
 *      -> 查表：ID -> 处理函数。阶段 3 的 ECU 就是在这个骨架上长大的。
 *
 * 编译： make 02-router/can_router
 * 运行： ./02-router/can_router [-i 接口] [-v] [id ...]
 *        默认接口 vcan0，默认订阅 0x180 0x181 0x200
 *        -v 打印每一帧（默认只每秒打一行汇总，因为 10ms 周期的帧
 *           每秒 100 条，真的 ECU 也不会逐帧 printf）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "can_proto.h"

#define MAX_IDS 8

static volatile sig_atomic_t g_stop;

struct slot {
	canid_t id;
	const char *name;
	void (*fn)(const struct can_frame *f);	/* 语义解析，阶段 3 填内容 */
	unsigned long cnt;
	unsigned long snap;			/* 上一秒的 cnt，用来算速率 */
	int has_last;
	struct can_frame last;
};

static struct slot g_slot[MAX_IDS];
static int g_nslot;
static unsigned long g_unknown;

static void die(const char *what)
{
	fprintf(stderr, "[-] %s 失败: %s\n", what, strerror(errno));
	exit(EXIT_FAILURE);
}

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

/* ---------- 三个语义处理函数：阶段 3 会把这里换成真正的信号解码 ---------- */

static void print_payload(const struct can_frame *f)
{
	printf("dlc=%u data=", f->can_dlc);
	for (int i = 0; i < f->can_dlc && i < 8; i++)
		printf("%02X ", f->data[i]);
	printf("\n");
}

static void on_engine_fast(const struct can_frame *f)
{
	printf("  ENGINE_FAST  ");
	print_payload(f);
}

static void on_engine_slow(const struct can_frame *f)
{
	printf("  ENGINE_SLOW  ");
	print_payload(f);
}

static void on_dash_cmd(const struct can_frame *f)
{
	printf("  DASH_CMD     ");
	print_payload(f);
}

/* ---------- 打开套接字并装上内核过滤器 ---------- */

static int can_open_filtered(const char *ifname, const canid_t *ids, int nids)
{
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (s < 0)
		die("socket(PF_CAN, SOCK_RAW, CAN_RAW)");

	unsigned int ifindex = if_nametoindex(ifname);
	if (ifindex == 0) {
		errno = ENODEV;
		die(ifname);
	}

	struct sockaddr_can addr;
	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = (int)ifindex;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("bind");

	/*
	 * 过滤器语义： (rx_id & can_mask) == (can_id & can_mask) 才收。
	 *   mask = CAN_SFF_MASK (0x7FF) -> 11 位全比较 = 精确匹配某个 ID
	 *   mask = 0x7F0                -> 只比较高 7 位 = 收 0x180~0x18F 一整段
	 *   mask = 0                    -> 全收（等价于不装过滤器）
	 * 多个 can_filter 之间是「或」的关系。
	 */
	struct can_filter flt[MAX_IDS];
	for (int i = 0; i < nids; i++) {
		flt[i].can_id   = ids[i];
		flt[i].can_mask = CAN_SFF_MASK;
	}
	if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, flt,
		       (socklen_t)(nids * sizeof(flt[0]))) < 0)
		die("setsockopt(CAN_RAW_FILTER)");

	return s;
}

/* ---------- 分发 ---------- */

static void dispatch(const struct can_frame *f, int verbose)
{
	/* 扩展帧不在本实验台的协议里，直接记一笔走人 */
	if (f->can_id & CAN_EFF_FLAG) {
		g_unknown++;
		return;
	}

	canid_t id = f->can_id & CAN_SFF_MASK;	/* 必须先剥标志位再比 ID */

	for (int i = 0; i < g_nslot; i++) {
		if (g_slot[i].id != id)
			continue;
		g_slot[i].cnt++;
		g_slot[i].last = *f;
		g_slot[i].has_last = 1;
		if (verbose && g_slot[i].fn)
			g_slot[i].fn(f);
		return;
	}

	/* 装了精确过滤器却还能走到这里，说明过滤器配错了 —— 这本身是个好检查点 */
	g_unknown++;
	if (verbose)
		printf("  [意外 ID 0x%03X] 过滤器应该把它丢掉的\n", id);
}

static void report(long long t_ms)
{
	printf("[t=%3llds]", t_ms / 1000);
	for (int i = 0; i < g_nslot; i++) {
		unsigned long delta = g_slot[i].cnt - g_slot[i].snap;
		g_slot[i].snap = g_slot[i].cnt;
		printf("  %s:%lu(%lu/s)", g_slot[i].name, g_slot[i].cnt, delta);
	}
	if (g_unknown)
		printf("  unknown:%lu", g_unknown);
	printf("\n");
	fflush(stdout);
}

int main(int argc, char *argv[])
{
	const char *ifname = "vcan0";
	int verbose = 0;
	canid_t ids[MAX_IDS];
	int nids = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			ifname = argv[++i];
		} else if (isdigit((unsigned char)argv[i][0])) {
			/* 数字开头才当 ID，0x180 和 384 两种写法都认 */
			canid_t id = (canid_t)strtoul(argv[i], NULL, 0);
			if (id > CAN_SFF_MASK) {
				/*
				 * 经典坑：11 位标准帧的 ID 上限是 0x7FF。
				 * 传一个 0x999 进去，内核不会报错，而是把 bit11 以上
				 * 直接截掉 —— 总线上看到的是 0x199。发送端 rc=0，
				 * 接收端永远等不到 0x999。这里显式提示并归一化。
				 */
				fprintf(stderr,
					"[!] 0x%X 超出 11 位标准帧范围，总线上实际是 0x%X（内核静默截断）\n",
					id, id & CAN_SFF_MASK);
				id &= CAN_SFF_MASK;
			}
			if (nids < MAX_IDS)
				ids[nids++] = id;
		} else {
			/* 其余当接口名，方便 ./can_router vcan0 0x999 这种写法 */
			ifname = argv[i];
		}
	}
	if (nids == 0) {
		ids[nids++] = CAN_ID_ENGINE_FAST;
		ids[nids++] = CAN_ID_ENGINE_SLOW;
		ids[nids++] = CAN_ID_DASH_CMD;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	printf("=== 阶段 2：过滤器 + poll + 分发 ===\n");
	for (int i = 0; i < nids; i++)
		printf("  订阅 0x%03X/0x%03X  %s\n", ids[i], CAN_SFF_MASK,
		       can_id_name(ids[i]));

	int s = can_open_filtered(ifname, ids, nids);
	printf("  接口 %s 套接字 fd=%d 模式 %s\n", ifname, s,
	       verbose ? "逐帧打印" : "每秒汇总");
	printf("  等待数据（Ctrl-C 结束）...\n\n");

	for (int i = 0; i < nids; i++) {
		g_slot[i].id   = ids[i];
		g_slot[i].name = can_id_name(ids[i]);
		g_slot[i].fn   = NULL;
		if (ids[i] == CAN_ID_ENGINE_FAST) g_slot[i].fn = on_engine_fast;
		if (ids[i] == CAN_ID_ENGINE_SLOW) g_slot[i].fn = on_engine_slow;
		if (ids[i] == CAN_ID_DASH_CMD)    g_slot[i].fn = on_dash_cmd;
	}
	g_nslot = nids;

	struct pollfd pfd = { .fd = s, .events = POLLIN, .revents = 0 };
	long long t0 = now_ms();
	long long next_report = t0 + 1000;

	while (!g_stop) {
		int r = poll(&pfd, 1, 200);
		if (r < 0) {
			if (errno == EINTR)
				continue;	/* 收到 Ctrl-C，回到循环条件退出 */
			die("poll");
		}

		if (r > 0 && (pfd.revents & POLLIN)) {
			struct can_frame f;
			ssize_t n;

			/*
			 * 一次唤醒尽量把队列抽干：MSG_DONTWAIT 保证抽空后
			 * 立即返回 EAGAIN，不会卡在这里挡住下面的周期统计。
			 * 这是「事件驱动 + 批量处理」的标准写法。
			 */
			while ((n = recv(s, &f, sizeof(f), MSG_DONTWAIT)) > 0) {
				if (n != (ssize_t)sizeof(struct can_frame)) {
					/* 长度不对多半是 CAN FD 帧（72 字节），阶段 4 再处理 */
					continue;
				}
				dispatch(&f, verbose);
			}
			if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
				die("recv");
		}

		long long now = now_ms();
		if (now >= next_report) {
			report(now - t0);
			next_report += 1000;
		}
	}

	/* ---------- 收尾统计 ---------- */
	printf("\n=== 汇总 ===\n");
	for (int i = 0; i < g_nslot; i++) {
		printf("  0x%03X %-12s %6lu 帧", g_slot[i].id, g_slot[i].name,
		       g_slot[i].cnt);
		if (g_slot[i].has_last) {
			printf("   最后一帧 dlc=%u data=", g_slot[i].last.can_dlc);
			for (int k = 0; k < g_slot[i].last.can_dlc && k < 8; k++)
				printf("%02X ", g_slot[i].last.data[k]);
		}
		printf("\n");
	}
	printf("  未知/扩展帧 %lu\n", g_unknown);
	close(s);
	return 0;
}
