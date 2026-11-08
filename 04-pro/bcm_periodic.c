/*
 * 阶段 4 —— 把周期性工作交给内核：CAN_BCM（Broadcast Manager）
 *
 * 阶段 3 里，10 ms 的周期是用户态进程自己用 clock_nanosleep 撑起来的。
 * 这在实验台上够用，但在真实控制器上有三个绕不过去的毛病：
 *
 *   1) 进程被调度延迟、被换出、被更高优先级任务抢占 -> 这一帧就晚了。
 *      抖动直接进总线，仪表看到的转速就会一跳一跳。
 *   2) 进程崩了，报文立刻停 —— 总线上其他节点只能靠超时发现。
 *   3) 每帧一次 write，10 ms 一帧就是每秒 100 次系统调用。
 *
 * CAN_BCM 把这件事搬进内核：你告诉内核「这个 ID、这个周期、这份内容」，
 * 之后由内核的 hrtimer 定时发送，进程可以完全不管。
 * 同一套机制还提供反向能力：按 ID 监控，超时自动上报。
 *
 * 三种用法（本程序三个子命令）：
 *
 *   tx  周期发送：TX_SETUP 建一个循环发送任务
 *   rx  超时监控：RX_SETUP 订阅某个 ID，超过 ival1 没来就收到 RX_TIMEOUT
 *   mon 时序测量：普通原始套接字，统计相邻帧间隔（用来对比 tx 和阶段 3）
 *
 * 用法：
 *   ./04-pro/bcm_periodic tx  vcan0 10      # 内核每 10ms 发 0x181
 *   ./04-pro/bcm_periodic rx  vcan0 300     # 0x180 超过 300ms 不来就报警
 *   ./04-pro/bcm_periodic mon vcan0 10      # 统计 0x180 的到达间隔
 *
 * 关键认知：BCM 发的是**固定内容**。
 * 内容一旦交给内核就改不了了（除非重新 TX_SETUP，那样反而比用户态更慢）。
 * 所以 BCM 适合「内容不变、周期严格」的报文（心跳、固定状态广播），
 * 不适合转速这类每帧都在变的量 —— 这正好解释了阶段 3 为什么必须用户态发。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <time.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/bcm.h>

#include "can_proto.h"

#define MAX_BCM_MSG 4096

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void die(const char *what)
{
	fprintf(stderr, "[-] %s 失败: %s\n", what, strerror(errno));
	exit(EXIT_FAILURE);
}

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int if_index(const char *ifname)
{
	unsigned int idx = if_nametoindex(ifname);

	if (idx == 0) {
		errno = ENODEV;
		die(ifname);
	}
	return (int)idx;
}

/* ---------------- BCM 报文缓冲区 ----------------
 *
 * 这里有个真实的坑：struct bcm_msg_head 的最后一个成员是柔性数组 frames[]，
 * 而 ival1/ival2 里的 tv_sec/tv_usec 是 long。在 64 位平台上
 * long 是 8 字节，count 后面会插入 4 字节填充，
 * 于是 sizeof(struct bcm_msg_head) == offsetof(frames) == 56。
 * 在 32 位平台上这两个数都不等于 56。
 *
 * 所以**不能**写 struct { struct bcm_msg_head h; struct can_frame f; } ——
 * 编译器会按自己的对齐规则把 f 放到它认为合适的地方，
 * 而内核读 frames[] 用的是 offsetof(frames)。两边一旦不一致，
 * 内核读到的是垃圾，现象是「BCM 建好了但发出来全是 0」。
 *
 * 正确做法：按 offsetof + sizeof(can_frame) 分配，用 frames[0] 访问。
 */
static struct bcm_msg_head *bcm_alloc(void)
{
	size_t sz = offsetof(struct bcm_msg_head, frames) + sizeof(struct can_frame);
	struct bcm_msg_head *mh = calloc(1, sz);

	if (!mh)
		die("calloc");

	return mh;
}

static size_t bcm_size(void)
{
	return offsetof(struct bcm_msg_head, frames) + sizeof(struct can_frame);
}

/* ---------------- tx：内核周期发送 ---------------- */

static int cmd_tx(const char *ifname, int period_ms)
{
	int s = socket(PF_CAN, SOCK_DGRAM, CAN_BCM);

	if (s < 0)
		die("socket(PF_CAN, SOCK_DGRAM, CAN_BCM)");

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = if_index(ifname);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("connect");

	struct bcm_msg_head *mh = bcm_alloc();
	struct can_frame *f = &mh->frames[0];

	/*
	 * 内容固定：水温 90degC、油压 300kPa、flags=0。
	 * 之所以选 0x181，是因为它就是「慢变量、内容几乎不变」的那个报文；
	 * 0x180 的转速每 10ms 都在变，交给 BCM 没有意义（见文件头注释）。
	 */
	double slow[ES_NSIG] = { 90.0, 300.0, 0.0 };

	proto_encode(proto_lookup(CAN_ID_ENGINE_SLOW), slow, f->data);
	f->can_id  = CAN_ID_ENGINE_SLOW;
	f->can_dlc = 8;

	mh->opcode = TX_SETUP;
	mh->flags  = SETTIMER | STARTTIMER;
	mh->count  = 0;			/* 不切换周期，一直用 ival2 */
	mh->ival1.tv_sec  = 0;
	mh->ival1.tv_usec = 0;
	mh->ival2.tv_sec  = period_ms / 1000;
	mh->ival2.tv_usec = (period_ms % 1000) * 1000;
	mh->can_id = CAN_ID_ENGINE_SLOW;
	mh->nframes = 1;

	printf("=== BCM 周期发送 ===\n");
	printf("  接口 %s  0x%03X 每 %d ms（由内核 hrtimer 驱动）\n",
	       ifname, CAN_ID_ENGINE_SLOW, period_ms);
	printf("  字节: ");
	for (int i = 0; i < f->can_dlc; i++)
		printf("%02X ", f->data[i]);
	printf("（内容固定，之后不再变化）\n");
	printf("  本进程不参与发送，Ctrl-C 结束并删除任务\n\n");

	if (write(s, mh, bcm_size()) != (ssize_t)bcm_size())
		die("write(TX_SETUP)");

	/*
	 * 进程在这里什么都不做。故意留一个空转，就是为了证明：
	 * 帧不是这个进程发的，是内核发的。可以 sleep，也可以去干别的。
	 */
	while (!g_stop) {
		struct timespec ts = { 0, 200 * 1000 * 1000 };

		nanosleep(&ts, NULL);
	}

	/*
	 * 退出前显式删除任务。
	 *
	 * 实测：直接 SIGKILL 也不会留下野任务 —— 套接字关闭时内核会把挂在
	 * 它上面的 BCM 任务一起释放（bcm_release）。但显式 TX_DELETE 仍然值得写：
	 * 同一个进程里想「暂停发送但保留套接字」时，只能靠它。
	 */
	mh->opcode = TX_DELETE;
	if (write(s, mh, bcm_size()) < 0)
		fprintf(stderr, "[!] TX_DELETE: %s\n", strerror(errno));
	else
		printf("\n任务已删除（TX_DELETE）\n");

	close(s);
	free(mh);
	return 0;
}

/* ---------------- rx：内核超时监控 ---------------- */

static const char *opcode_name(unsigned op)
{
	switch (op) {
	case TX_STATUS:  return "TX_STATUS";
	case TX_EXPIRED: return "TX_EXPIRED";
	case RX_STATUS:  return "RX_STATUS";
	case RX_TIMEOUT: return "RX_TIMEOUT";
	case RX_CHANGED: return "RX_CHANGED";
	default:         return "?";
	}
}

static int cmd_rx(const char *ifname, int timeout_ms)
{
	int s = socket(PF_CAN, SOCK_DGRAM, CAN_BCM);

	if (s < 0)
		die("socket(PF_CAN, SOCK_DGRAM, CAN_BCM)");

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = if_index(ifname);
	if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("connect");

	struct bcm_msg_head *mh = bcm_alloc();

	mh->opcode = RX_SETUP;
	mh->flags  = 0;
	mh->can_id = CAN_ID_ENGINE_FAST;
	mh->nframes = 0;		/* 监控模式不需要带帧 */
	/*
	 * ival1 = 超时门限：这么久没收到 0x180，内核主动上报 RX_TIMEOUT。
	 * ival2 = 0：不做节流，每收到一帧都上报 RX_CHANGED
	 *         （想省 CPU 就把它设成 100ms，只告诉你「还在来」）。
	 */
	mh->ival1.tv_sec  = timeout_ms / 1000;
	mh->ival1.tv_usec = (timeout_ms % 1000) * 1000;
	mh->ival2.tv_sec  = 0;
	mh->ival2.tv_usec = 0;
	mh->flags = SETTIMER | STARTTIMER;

	printf("=== BCM 超时监控 ===\n");
	printf("  接口 %s  监控 0x%03X，超过 %d ms 未收到即上报 RX_TIMEOUT\n",
	       ifname, CAN_ID_ENGINE_FAST, timeout_ms);
	printf("  这个定时器在内核里跑：本进程即使被 Ctrl-Z 挂起也照样能报警\n\n");

	if (write(s, mh, bcm_size()) < 0)
		die("write(RX_SETUP)");

	long long t0 = now_us();
	unsigned long nchanged = 0, ntimeout = 0;
	int down = 0;

	while (!g_stop) {
		ssize_t n = read(s, mh, MAX_BCM_MSG);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("read");
		}

		double rel = (double)(now_us() - t0) / 1e6;

		switch (mh->opcode) {
		case RX_CHANGED:
			nchanged++;
			if (down) {
				printf("[%6.3fs] 0x180 恢复\n", rel);
				down = 0;
			}
			break;
		case RX_TIMEOUT:
			ntimeout++;
			if (!down) {
				printf("[%6.3fs] !! 0x180 超时 %d ms，内核判定链路静默\n",
				       rel, timeout_ms);
				down = 1;
			}
			break;
		default:
			printf("[%6.3fs] %s（nframes=%u）\n", rel,
			       opcode_name(mh->opcode), mh->nframes);
			break;
		}
	}

	printf("\n=== 统计 ===\n");
	printf("  RX_CHANGED %lu 次，RX_TIMEOUT %lu 次\n", nchanged, ntimeout);

	close(s);
	free(mh);
	return 0;
}

/* ---------------- mon：时序测量 ---------------- */

struct gap_stat {
	unsigned long n;
	double sum, sumsq, min, max;
};

static void gap_add(struct gap_stat *g, double gap_ms)
{
	if (gap_ms <= 0)
		return;
	if (g->n == 0 || gap_ms < g->min)
		g->min = gap_ms;
	if (gap_ms > g->max)
		g->max = gap_ms;
	g->sum += gap_ms;
	g->sumsq += gap_ms * gap_ms;
	g->n++;
}

static void gap_report(struct gap_stat *g)
{
	if (g->n < 2) {
		printf("  窗口内帧数不足\n");
		return;
	}

	double mean = g->sum / (double)g->n;
	double var = g->sumsq / (double)g->n - mean * mean;
	double sd = var > 0 ? sqrt(var) : 0.0;

	printf("  %6lu 个间隔  avg=%7.3f ms  min=%7.3f  max=%7.3f  抖动(std)=%.3f ms\n",
	       g->n, mean, g->min, g->max, sd);
	memset(g, 0, sizeof(*g));
}

static int cmd_mon(const char *ifname, int expect_ms, canid_t id)
{
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (s < 0)
		die("socket");

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = if_index(ifname);
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		die("bind");

	struct can_filter flt = {
		.can_id   = id,
		.can_mask = CAN_SFF_MASK,
	};

	if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, &flt, sizeof(flt)) < 0)
		die("setsockopt");

	printf("=== 时序测量 ===\n");
	printf("  接口 %s  统计 0x%03X 相邻帧间隔（期望 %d ms），每 2 秒一行\n\n",
	       ifname, id, expect_ms);

	long long t0 = now_us();
	long long prev = 0, next_report = t0 + 2000000LL;
	struct gap_stat g;

	memset(&g, 0, sizeof(g));

	while (!g_stop) {
		struct can_frame f;
		ssize_t n = recv(s, &f, sizeof(f), 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("recv");
		}

		long long now = now_us();

		if (prev)
			gap_add(&g, (double)(now - prev) / 1000.0);
		prev = now;

		if (now >= next_report) {
			printf("[t=%5.1fs]", (double)(now - t0) / 1e6);
			gap_report(&g);
			fflush(stdout);
			next_report += 2000000LL;
		}
	}

	printf("\n");
	gap_report(&g);
	close(s);
	return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[])
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/*
	 * 重定向到文件时 stdout 默认是块缓冲，进程被 kill 掉会丢掉最后一段。
	 * 诊断工具类程序一律改行缓冲，否则「程序挂了，日志也是空的」。
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc < 2) {
		fprintf(stderr,
			"用法:\n"
			"  %s tx  <接口> [周期ms=10]           内核周期发送 0x181\n"
			"  %s rx  <接口> [超时ms=300]          内核监控 0x180 超时\n"
			"  %s mon <接口> [周期ms=10] [ID=0x180] 测量到达间隔\n",
			argv[0], argv[0], argv[0]);
		return 1;
	}

	const char *mode = argv[1];
	const char *ifname = (argc > 2) ? argv[2] : "vcan0";

	if (!strcmp(mode, "tx"))
		return cmd_tx(ifname, (argc > 3) ? atoi(argv[3]) : 10);
	if (!strcmp(mode, "rx"))
		return cmd_rx(ifname, (argc > 3) ? atoi(argv[3]) : 300);
	if (!strcmp(mode, "mon"))
		return cmd_mon(ifname, (argc > 3) ? atoi(argv[3]) : 10,
			       (argc > 4) ? (canid_t)strtoul(argv[4], NULL, 0)
					  : CAN_ID_ENGINE_FAST);

	fprintf(stderr, "[-] 未知模式 %s\n", mode);
	return 1;
}
