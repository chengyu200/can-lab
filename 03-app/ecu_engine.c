/*
 * 阶段 3 —— 节点 A：发动机控制器 ecu_engine
 *
 * 这是第一个「真正的 ECU」程序，它同时做三件事，而这三件事在真实 ECU 里
 * 由 RTOS 的三个任务完成：
 *
 *   1) 模型任务：维护发动机内部状态（转速、水温、油压），
 *      由「驾驶员目标转速」驱动。这里用一个正弦扫频模拟油门被反复踩下松开，
 *      这样不用人工干预就能看到闭环在动。
 *
 *   2) 发送任务：10 ms 发 0x180（快变量），100 ms 发 0x181（慢变量）。
 *      周期发送用 clock_nanosleep 的 **绝对时间** 模式，见下面 loop() 的注释。
 *
 *   3) 接收任务：收 0x200 命令（降扭/解除降扭），改变自己的目标转速。
 *
 * 两个关键设计点，值得先记住：
 *
 *   A. 编码不写移位，走 proto_encode() + 信号表。
 *      这样「发送端」和「仪表盘」共用同一份布局定义，不会一边改成
 *      bit16 另一边还按 bit8 解 —— 联调里最常见的一类事故。
 *
 *   B. 不打开 CAN_RAW_RECV_OWN_MSGS。
 *      自己发的 0x180 不需要自己收。阶段 1 里为了让单进程跑通闭环才打开它；
 *      现在有第二个节点了，让总线去回环即可。
 *
 * 运行： ./03-app/ecu_engine vcan0
 * 退出： Ctrl-C，会打印周期抖动的统计结果
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "can_proto.h"

#define PERIOD_FAST_MS	10
#define PERIOD_SLOW_MS	100

static volatile sig_atomic_t g_stop;

/* ---------------- 发动机模型 ---------------- */

struct engine {
	double rpm;
	double target;		/* 驾驶员想要的转速（受降扭影响后） */
	double throttle;	/* % */
	double load;		/* % */
	double coolant;		/* degC */
	double oil;		/* kPa */
	unsigned flags;
	double derate;		/* 0.0 ~ 1.0，当前降扭比例 */
};

static double clampd(double v, double lo, double hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/*
 * 用真实时间推进模型。dt 单位秒。
 *
 * 模型本身不重要（这不是仿真软件），重要的是它让信号「像真的」：
 * 转速带惯性、水温有热容、油压随转速走。有了这些，
 * 后面测出来的时序、抖动、超时才有意义。
 */
static void simulate(struct engine *e, double t, double dt)
{
	/* 驾驶员：24 秒一个来回，目标转速在怠速 800 与 4200 之间扫 */
	double sweep = 0.5 - 0.5 * cos(2.0 * M_PI * t / 24.0);

	e->target = (800.0 + 3400.0 * sweep) * (1.0 - e->derate);

	/* P 控制器：目标转速与实际转速之差 -> 节气门开度 */
	e->throttle = clampd(50.0 + (e->target - e->rpm) * 0.25, 0.0, 100.0);

	/* 发动机：稳态转速由节气门决定，一阶惯性 0.5 秒跟上去 */
	double rpm_ss = e->throttle / 100.0 * 5200.0;

	e->rpm += (rpm_ss - e->rpm) * dt / 0.5;
	if (e->rpm < 600.0)
		e->rpm = 600.0;

	/* 负荷：跟节气门正相关，加一点噪声让数值不是死板的 */
	e->load = clampd(e->throttle * 0.95 + 4.0, 0.0, 110.0);

	/* 水温：热源来自负荷，散热正比于与环境温差 */
	double heat = e->load / 100.0 * 5.0;
	double cool = (e->coolant - 25.0) * 0.08;

	e->coolant += (heat - cool) * dt;
	e->coolant = clampd(e->coolant, 20.0, 130.0);

	/* 油压：怠速就有基础压力，随转速上升 */
	e->oil = 120.0 + e->rpm * 0.06;

	/* 位域：一个字节里塞多个布尔量，这是 CAN 报文节省带宽的常规手段 */
	e->flags = 0;
	if (e->coolant > 105.0)
		e->flags |= ES_FLAG_OVERHEAT;
	if (e->coolant > 112.0)
		e->flags |= ES_FLAG_MIL;
	if (e->oil < 150.0)
		e->flags |= ES_FLAG_OIL_LOW;
}

/* ---------------- 时间工具 ---------------- */

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static void ts_add_ms(struct timespec *t, long ms)
{
	t->tv_nsec += ms * 1000000L;
	while (t->tv_nsec >= 1000000000L) {
		t->tv_nsec -= 1000000000L;
		t->tv_sec++;
	}
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

/* ---------------- 发送 ---------------- */

static int send_msg(int s, canid_t id, const double *phys)
{
	const struct can_msg_def *m = proto_lookup(id);
	struct can_frame f;

	if (!m) {
		fprintf(stderr, "[-] 0x%03X 不在协议表里\n", id);
		return -1;
	}

	memset(&f, 0, sizeof(f));
	f.can_id  = m->id;
	f.can_dlc = m->dlc;
	proto_encode(m, phys, f.data);

	/*
	 * 必须一次写完整的 sizeof(struct can_frame)。
	 * CAN 套接字不接受「分批写半个帧」，写少了就是 EMSGSIZE。
	 */
	if (write(s, &f, sizeof(f)) != (ssize_t)sizeof(f)) {
		fprintf(stderr, "[!] write 0x%03X: %s\n", id, strerror(errno));
		return -1;
	}
	return 0;
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

	/* 只关心 0x200：命令通道。其余 ID 在内核就被丢掉 */
	struct can_filter flt = {
		.can_id   = CAN_ID_DASH_CMD,
		.can_mask = CAN_SFF_MASK,
	};

	if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, &flt, sizeof(flt)) < 0)
		die("setsockopt(CAN_RAW_FILTER)");

	return s;
}

/* ---------------- 命令处理 ---------------- */

static void handle_cmd(struct engine *e, const struct can_frame *f)
{
	const struct can_msg_def *m = proto_lookup(f->can_id & CAN_SFF_MASK);
	double phys[CAN_MAX_SIGNALS];

	proto_decode(m, f->data, phys);

	unsigned cmd = (unsigned)phys[DC_CMD];
	unsigned arg = (unsigned)phys[DC_ARG];

	switch (cmd) {
	case CMD_DERATE_REQ:
		e->derate = clampd(arg / 100.0, 0.0, 0.9);
		printf("\n  [RX] 收到降扭请求 arg=%u%%  -> 目标转速乘 %.2f\n",
		       arg, 1.0 - e->derate);
		break;
	case CMD_DERATE_CLR:
		e->derate = 0.0;
		printf("\n  [RX] 收到解除降扭  -> 恢复全功率\n");
		break;
	default:
		printf("\n  [RX] 未知命令 cmd=0x%02X arg=%u（协议版本不一致？）\n",
		       cmd, arg);
		break;
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

	struct engine e = {
		.rpm     = 800.0,
		.coolant = 25.0,
		.oil     = 160.0,
	};

	printf("=== 阶段 3：发动机节点 ecu_engine ===\n");
	printf("  接口 %s   0x180/%dms  0x181/%dms   收 0x200 命令\n",
	       ifname, PERIOD_FAST_MS, PERIOD_SLOW_MS);
	printf("  Ctrl-C 结束\n\n");

	long long t0 = now_us();
	long long t_next_report = t0 + 1000000LL;

	/* 周期调度的基准时刻：绝对时间，不是相对延时 */
	struct timespec next;

	clock_gettime(CLOCK_MONOTONIC, &next);

	unsigned long tick = 0;
	unsigned long ntx_fast = 0, ntx_slow = 0, ncmd = 0;
	unsigned alive = 0;
	double lat_max = 0.0, lat_sum = 0.0;
	unsigned long lat_n = 0;
	int fast_fail = 0, slow_fail = 0;

	while (!g_stop) {
		ts_add_ms(&next, PERIOD_FAST_MS);

		/*
		 * 【本阶段最重要的三行】
		 *
		 * clock_nanosleep + TIMER_ABSTIME 是「睡到某个绝对时刻」，
		 * 而 sleep(10ms) 是「睡 10 毫秒」—— 后者会把本次循环的处理耗时
		 * 累加进周期，跑一分钟就漂出几百毫秒，还越漂越远。
		 *
		 * 绝对时间模式下，next 是理论时刻：即使某次被调度延迟了，
		 * 下一次仍然对齐到原有时基，误差不会累积（只会抖动）。
		 * 这也是所有实时系统里写周期任务的标准做法。
		 */
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL)
		       == EINTR && !g_stop)
			;

		if (g_stop)
			break;

		/* 抖动 = 实际醒来时刻 - 理论时刻 */
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		double late = (double)(now.tv_sec - next.tv_sec) * 1e6 +
			      (double)(now.tv_nsec - next.tv_nsec) / 1e3;

		lat_sum += late;
		lat_n++;
		if (late > lat_max)
			lat_max = late;

		double t = (double)(now_us() - t0) / 1e6;

		simulate(&e, t, PERIOD_FAST_MS / 1000.0);

		/* ---- 0x180 快变量 ---- */
		double fast[EF_NSIG];

		fast[EF_RPM]      = e.rpm;
		fast[EF_THROTTLE] = e.throttle;
		fast[EF_LOAD]     = e.load;
		/*
		 * 滚动计数器：接收端靠它精确算丢帧。
		 * 到 255 自然回绕，& 0xFF 让回绕不影响差值计算。
		 */
		fast[EF_ALIVE]    = (double)(alive++ & 0xFF);
		if (send_msg(s, CAN_ID_ENGINE_FAST, fast) < 0)
			fast_fail++;
		ntx_fast++;

		/* ---- 0x181 慢变量：每 10 个 tick 发一次 ---- */
		if (++tick % (PERIOD_SLOW_MS / PERIOD_FAST_MS) == 0) {
			double slow[ES_NSIG];

			slow[ES_COOLANT]   = e.coolant;
			slow[ES_OIL_PRESS] = e.oil;
			slow[ES_FLAGS]     = (double)e.flags;
			if (send_msg(s, CAN_ID_ENGINE_SLOW, slow) < 0)
				slow_fail++;
			ntx_slow++;
		}

		/* ---- 收命令：非阻塞，抽干队列 ---- */
		struct can_frame f;

		while (recv(s, &f, sizeof(f), MSG_DONTWAIT) > 0) {
			handle_cmd(&e, &f);
			ncmd++;
		}

		/* ---- 每秒一行状态 ---- */
		long long now_ll = now_us();

		if (now_ll >= t_next_report) {
			unsigned long left = (unsigned long)((t_next_report - t0) / 1000000LL);

			printf("[t=%3lus] rpm=%6.0f thr=%5.1f%% load=%5.1f%% "
			       "coolant=%5.1fC oil=%5.0fkPa flags=0x%02X derate=%2.0f%% "
			       "| tx %lu/%lu 抖动(avg/max)=%.0f/%.0fus\n",
			       left, e.rpm, e.throttle, e.load, e.coolant, e.oil,
			       e.flags, e.derate * 100.0,
			       ntx_fast, ntx_slow,
			       lat_n ? lat_sum / (double)lat_n : 0.0, lat_max);
			fflush(stdout);
			t_next_report += 1000000LL;
		}
	}

	/* ---------- 收尾 ---------- */
	double secs = (double)(now_us() - t0) / 1e6;

	printf("\n=== 统计 ===\n");
	printf("  运行 %.1f 秒\n", secs);
	printf("  0x180 发送 %lu 帧（%.1f 帧/秒，失败 %d 次）\n",
	       ntx_fast, secs > 0 ? (double)ntx_fast / secs : 0.0, fast_fail);
	printf("  0x181 发送 %lu 帧（%.1f 帧/秒，失败 %d 次）\n",
	       ntx_slow, secs > 0 ? (double)ntx_slow / secs : 0.0, slow_fail);
	printf("  收到命令 %lu 条\n", ncmd);
	printf("  调度抖动：平均 %.1f us，最大 %.1f us\n",
	       lat_n ? lat_sum / (double)lat_n : 0.0, lat_max);
	printf("  注意：这是用户态抖动。把 10 ms 交给内核（BCM）能做到几十微秒，\n");
	printf("        见阶段 4 的 bcm_periodic，用 bcm_periodic mon 可以量出来对比。\n");

	close(s);
	return 0;
}
