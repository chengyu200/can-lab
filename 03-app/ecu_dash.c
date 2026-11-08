/*
 * 阶段 3 —— 节点 B：仪表盘 ecu_dash
 *
 * 这个程序演示车载节点的另外三件必备能力，和阶段的 «收 + 解 + 打» 不同：
 *
 *   1) 超时监控（timeout supervision）
 *      周期报文最核心的契约不是「内容对不对」，而是「有没有按时来」。
 *      发动机节点掉线时，总线不会报错，只是安静下来。仪表必须自己发现：
 *      超过 100 ms 没收到 0x180 就判超时，并且把显示冻结/标红。
 *      这是功能安全里最基础的一条 —— 沉默必须被当成故障，而不是「没事」。
 *
 *   2) 丢帧估计
 *      不靠序列号，只用到达间隔倒推：间隔 32 ms 而周期是 10 ms，
 *      就说明中间丢了 2 帧。真实项目里更可靠的做法是在 data 里放滚动
 *      计数器（counter/alive counter），阶段 3 的 0x180 保留位就是留给它的。
 *
 *   3) 闭环控制
 *      转速超过 3000 rpm 持续 1.5 秒 -> 发 0x200 请求降扭 25%；
 *      降到 2600 rpm 以下持续 3 秒 -> 发 0x200 解除。
 *      有迟滞（hysteresis）才不会在阈值附近抖动发命令。
 *
 * 运行： ./03-app/ecu_dash vcan0
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define FAST_PERIOD_MS		10
#define FAST_TIMEOUT_MS		100	/* 10 个周期没来就判超时 */
#define SLOW_TIMEOUT_MS		500

#define RPM_OVERSPEED		3000.0
#define RPM_RECOVER		2600.0
#define OVERSPEED_HOLD_MS	1500
#define RECOVER_HOLD_MS		3000
#define CMD_MIN_GAP_MS		4000	/* 两条命令之间至少隔 4 秒 */

static volatile sig_atomic_t g_stop;

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

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

/* ---------------- 打开套接字 ---------------- */

static int can_open(const char *ifname)
{
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (s < 0)
		die("socket");

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

	struct can_filter flt[2] = {
		{ .can_id = CAN_ID_ENGINE_FAST, .can_mask = CAN_SFF_MASK },
		{ .can_id = CAN_ID_ENGINE_SLOW, .can_mask = CAN_SFF_MASK },
	};

	if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, flt, sizeof(flt)) < 0)
		die("setsockopt(CAN_RAW_FILTER)");

	return s;
}

/* ---------------- 仪表状态 ---------------- */

struct dash {
	double rpm, throttle, load;		/* 来自 0x180 */
	double coolant, oil;
	unsigned flags;				/* 来自 0x181 */

	int have_fast, have_slow;
	long long last_fast_us, last_slow_us;

	/* 统计 */
	unsigned long rx_fast, rx_slow, lost, stall;
	unsigned long snap_fast;
	double gap_max_us;
	long long prev_fast_us;

	/* 滚动计数器 */
	unsigned last_seq;
	int has_seq;

	/* 超时状态 */
	int fast_stale, slow_stale;
	long long stale_since_us;

	/* 控制状态 */
	long long high_since_us, low_since_us, last_cmd_us;
	int derate_active;
};

static void send_cmd(int s, unsigned cmd, unsigned arg)
{
	const struct can_msg_def *m = proto_lookup(CAN_ID_DASH_CMD);
	struct can_frame f;
	double phys[DC_NSIG];

	phys[DC_CMD] = cmd;
	phys[DC_ARG] = arg;

	memset(&f, 0, sizeof(f));
	f.can_id  = m->id;
	f.can_dlc = m->dlc;
	proto_encode(m, phys, f.data);

	if (write(s, &f, sizeof(f)) != (ssize_t)sizeof(f))
		fprintf(stderr, "[!] 写 0x200 失败: %s\n", strerror(errno));
}

/* ---------------- 收到一帧 ---------------- */

static void on_frame(struct dash *d, const struct can_frame *f, long long ts)
{
	canid_t id = f->can_id & CAN_SFF_MASK;
	const struct can_msg_def *m = proto_lookup(id);
	double phys[CAN_MAX_SIGNALS];

	if (!m)
		return;

	proto_decode(m, f->data, phys);

	if (id == CAN_ID_ENGINE_FAST) {
		d->rpm      = phys[EF_RPM];
		d->throttle = phys[EF_THROTTLE];
		d->load     = phys[EF_LOAD];

		/*
		 * 丢帧判定用「协议里的滚动计数器」，不用到达间隔。
		 *
		 * 这是两种完全不同的故障，必须分开统计：
		 *   丢帧(lost)  = 发送端发了但没到总线上，或到了但没进本进程 → 计数器跳号
		 *   卡顿(stall) = 发了，但在路上耽搁了 → 计数器连续、间隔变大
		 * 只看间隔的话两者分不开：一帧晚到 20ms，和丢了一帧，
		 * 接收端看到的现象完全一样。
		 */
		unsigned seq = (unsigned)phys[EF_ALIVE];

		if (d->has_seq) {
			unsigned delta = (seq - d->last_seq) & 0xFF;

			if (delta == 0) {
				/* 重复帧（总线重传或对端逻辑 bug），不算丢帧 */
			} else if (delta < 128) {
				d->lost += delta - 1;
			}
			/* delta >= 128：计数器回退，多半是节点重启，不记丢帧 */
		}
		d->last_seq = seq;
		d->has_seq = 1;

		if (d->prev_fast_us) {
			double gap = (double)(ts - d->prev_fast_us);

			if (gap > d->gap_max_us)
				d->gap_max_us = gap;
			if (gap > FAST_PERIOD_MS * 2500.0)
				d->stall++;
		}
		d->prev_fast_us = ts;
		d->last_fast_us = ts;
		d->have_fast = 1;
		d->rx_fast++;

		if (d->fast_stale) {
			printf("\n  [恢复] 0x180 重新出现（中断了 %.0f ms）\n",
			       (double)(ts - d->stale_since_us) / 1000.0);
			d->fast_stale = 0;
		}
	} else if (id == CAN_ID_ENGINE_SLOW) {
		d->coolant = phys[ES_COOLANT];
		d->oil     = phys[ES_OIL_PRESS];
		d->flags   = (unsigned)phys[ES_FLAGS];

		d->last_slow_us = ts;
		d->have_slow = 1;
		d->rx_slow++;
		if (d->slow_stale)
			d->slow_stale = 0;
	}
}

/* ---------------- 超时监控 ---------------- */

static void check_timeout(struct dash *d, long long now)
{
	if (d->have_fast && !d->fast_stale &&
	    (now - d->last_fast_us) > FAST_TIMEOUT_MS * 1000LL) {
		d->fast_stale = 1;
		d->stale_since_us = d->last_fast_us;
		printf("\n  [!!] 0x180 超时 %lld ms —— 发动机节点失效/断线\n",
		       (now - d->last_fast_us) / 1000);
		fflush(stdout);
	}

	if (d->have_slow && !d->slow_stale &&
	    (now - d->last_slow_us) > SLOW_TIMEOUT_MS * 1000LL) {
		d->slow_stale = 1;
		printf("\n  [!!] 0x181 超时 —— 慢变量已冻结（显示的是旧值）\n");
		fflush(stdout);
	}
}

/* ---------------- 闭环：超速 -> 降扭 ---------------- */

static void control(struct dash *d, int s, long long now)
{
	if (d->fast_stale)		/* 数据不可信时不做控制决策 */
		return;

	if (d->rpm > RPM_OVERSPEED) {
		if (!d->high_since_us)
			d->high_since_us = now;
		d->low_since_us = 0;
	} else if (d->rpm < RPM_RECOVER) {
		if (!d->low_since_us)
			d->low_since_us = now;
		d->high_since_us = 0;
	} else {
		d->high_since_us = 0;	/* 落在迟滞带里，两个计时都清掉 */
		d->low_since_us = 0;
	}

	if (now - d->last_cmd_us < CMD_MIN_GAP_MS * 1000LL)
		return;

	if (!d->derate_active && d->high_since_us &&
	    now - d->high_since_us > OVERSPEED_HOLD_MS * 1000LL) {
		send_cmd(s, CMD_DERATE_REQ, 25);
		d->derate_active = 1;
		d->last_cmd_us = now;
		printf("\n  [TX] rpm>%.0f 持续 %dms  -> 请求降扭 25%%\n",
		       RPM_OVERSPEED, OVERSPEED_HOLD_MS);
		fflush(stdout);
	} else if (d->derate_active && d->low_since_us &&
		   now - d->low_since_us > RECOVER_HOLD_MS * 1000LL) {
		send_cmd(s, CMD_DERATE_CLR, 0);
		d->derate_active = 0;
		d->last_cmd_us = now;
		printf("\n  [TX] rpm<%.0f 持续 %dms  -> 解除降扭\n",
		       RPM_RECOVER, RECOVER_HOLD_MS);
		fflush(stdout);
	}
}

/* ---------------- 显示 ---------------- */

static void bar(char *out, size_t n, double v, double max, int width)
{
	int filled = (int)(v / max * width);

	if (filled < 0)
		filled = 0;
	if (filled > width)
		filled = width;

	int k = 0;

	out[k++] = '|';
	for (int i = 0; i < width && (size_t)k < n - 1; i++)
		out[k++] = (i < filled) ? '=' : ' ';
	out[k++] = '|';
	out[k] = '\0';
}

static void report(struct dash *d, long long t_sec)
{
	char b[64];
	unsigned long rate = d->rx_fast - d->snap_fast;

	d->snap_fast = d->rx_fast;

	bar(b, sizeof(b), d->rpm, 4500.0, 24);

	if (d->fast_stale) {
		printf("[t=%3llds] rpm=------  |                        | "
		       "水温 %5.1fC  油压 %4.0fkPa   【信号中断】\n",
		       t_sec, d->coolant, d->oil);
	} else {
		printf("[t=%3llds] rpm=%6.0f  %s 水温 %5.1fC  油压 %4.0fkPa  "
		       "MIL:%s  收 %lu/s  丢 %lu  卡 %lu  间隔max %.1fms\n",
		       t_sec, d->rpm, b, d->coolant, d->oil,
		       (d->flags & ES_FLAG_MIL) ? "ON " : "off",
		       rate, d->lost, d->stall, d->gap_max_us / 1000.0);
	}
	fflush(stdout);
}

/* ---------------- 主循环 ---------------- */

int main(int argc, char *argv[])
{
	const char *ifname = (argc > 1) ? argv[1] : "vcan0";

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	int s = can_open(ifname);

	struct dash d;

	memset(&d, 0, sizeof(d));

	printf("=== 阶段 3：仪表盘节点 ecu_dash ===\n");
	printf("  接口 %s   订阅 0x180/%dms 0x181   超时判据 %dms\n",
	       ifname, FAST_PERIOD_MS, FAST_TIMEOUT_MS);
	printf("  闭环：rpm>%.0f 持续 %dms -> 发 0x200 降扭；rpm<%.0f 持续 %dms -> 解除\n\n",
	       RPM_OVERSPEED, OVERSPEED_HOLD_MS, RPM_RECOVER, RECOVER_HOLD_MS);

	struct pollfd pfd = { .fd = s, .events = POLLIN, .revents = 0 };
	long long t0 = now_us();
	long long next_report = t0 + 1000000LL;

	while (!g_stop) {
		/*
		 * 50 ms 超时不是为了收包，是为了「即使总线上什么都没有，
		 * 也能定期醒来做超时判断」。纯阻塞的 recv 在节点掉线时
		 * 会永远等下去 —— 那就永远发现不了掉线。
		 */
		int r = poll(&pfd, 1, 50);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			die("poll");
		}

		if (r > 0 && (pfd.revents & POLLIN)) {
			struct can_frame f;
			ssize_t n;

			while ((n = recv(s, &f, sizeof(f), MSG_DONTWAIT)) > 0) {
				/* 时间戳要在 recv 之后立刻取，越晚取抖动越大 */
				on_frame(&d, &f, now_us());
			}
		}

		long long now = now_us();

		check_timeout(&d, now);
		control(&d, s, now);

		if (now >= next_report) {
			report(&d, (now - t0) / 1000000LL);
			next_report += 1000000LL;
		}
	}

	printf("\n=== 统计 ===\n");
	printf("  0x180 收 %lu 帧，0x181 收 %lu 帧\n", d.rx_fast, d.rx_slow);
	printf("  丢帧 %lu（计数器跳号，发送端真的没发），卡顿 %lu 次（间隔 > 2.5 个周期）\n",
	       d.lost, d.stall);
	printf("  相邻 0x180 最大间隔 %.2f ms（周期 %d ms）\n",
	       d.gap_max_us / 1000.0, FAST_PERIOD_MS);
	printf("  %.0f 帧/秒\n", (double)d.rx_fast /
	       (((double)(now_us() - t0)) / 1e6));

	close(s);
	return 0;
}
