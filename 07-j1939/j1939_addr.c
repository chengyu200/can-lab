/*
 * j1939_addr.c —— 地址声明（Address Claim）与地址仲裁
 *
 * 为什么 J1939 需要「地址声明」这一层？
 *   经典 CAN 只有 11/29 位 ID，没有源地址的概念；谁发的帧，接收方无从得知。
 *   J1939 把 29 位 ID 的最后 8 位固定为源地址 SA，于是「谁发的」有了答案。
 *   但地址只有 254 个可用（0x00~0xFD），且必须全网唯一 ——
 *   两个 ECU 撞到同一个地址，总线上就再也分不清谁是谁。
 *
 * 解决办法：每个 ECU 出厂带一个 64 位 NAME（全球唯一），
 * 上电后广播「我要用地址 X，我的 NAME 是 N」。
 * 如果同时有人也要 X，就比较 NAME：**数值小的赢**，输的一方必须换地址。
 *
 * 内核里这行判断就在 net/can/j1939/address-claim.c：
 *
 *     if (ecu->name > prev->name)   // 新来的名字更大
 *             j1939_ecu_unmap_locked(ecu);   // 新来的出局
 *
 * 本程序做两件事：
 *   1. 声明一个地址，并观察总线上所有的地址声明（打印 NAME 拆解与胜负）
 *   2. 每秒尝试发一帧业务报文，把「输掉仲裁后彻底发不出去」这个后果显式打出来
 *
 * 用法：
 *   j1939_addr <接口> <地址> <NAME>          例：j1939_addr vxcan0 0x80 0x1000
 *   j1939_addr <接口> <地址> <NAME> --quiet  不打印每秒探测，只看声明
 *
 *   --claim-period N   每 N 毫秒重播一次地址声明（默认 0 = 只播一次）
 *
 * 关于重播：真实 ECU 上电只播一次，之后靠「请求地址声明」
 * （PGN 0xEA00，数据 EE 00 00）让全网重新播报。本程序按规范实现了这个应答，
 * 但内核不会替你做 —— 它对 Request 不做任何自动响应。这一点很容易踩坑：
 * 很多实现上线后「莫名收不到别人」，就是因为没人应答这个请求。
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

static void hexdump(const uint8_t *d, int n, char *out, size_t len)
{
	size_t off = 0;

	for (int i = 0; i < n && off + 4 < len; i++)
		off += (size_t)snprintf(out + off, len - off, "%02X ", d[i]);
	if (off)
		out[off - 1] = '\0';
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
		fprintf(stderr, "[-] socket(CAN_J1939): %s\n"
			"    模块加载了吗？ modprobe can-j1939\n", strerror(errno));
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

	/* 广播（DA=0xFF）必须显式允许，否则 sendto 返回 EACCES */
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	return s;
}

/* J1939 的目的地址和 PGN 都在 sendto 的地址里，不在数据里 */
static int j1939_send(int s, uint32_t pgn, uint8_t da,
		      const uint8_t *data, size_t len)
{
	struct sockaddr_can a;

	memset(&a, 0, sizeof(a));
	a.can_family = AF_CAN;
	a.can_addr.j1939.name = 0;
	a.can_addr.j1939.pgn  = pgn;
	a.can_addr.j1939.addr = da;

	return (int)sendto(s, data, len, 0, (struct sockaddr *)&a, sizeof(a));
}

/*
 * 地址声明报文：PGN 0xEE00，数据就是 8 字节 NAME（小端）。
 * 内核会校验「数据里的 NAME」等于「bind 时的 NAME」，不等则返回 EPROTO。
 * 这是内核替你挡掉的一类错误：名字都写不一致的声明是没有意义的。
 */
static int j1939_claim(int s, uint64_t name)
{
	uint8_t d[8];

	memcpy(d, &name, sizeof(d));
	return j1939_send(s, PGN_ADDRESS_CLAIM, J1939_NO_ADDR, d, sizeof(d));
}

static void print_name(uint64_t name)
{
	struct j1939_name n;
	char wire[32];

	j1939_name_decode(name, &n);
	j1939_name_wire_str(name, wire, sizeof(wire));
	printf("NAME=0x%016llX  总线字节=%s\n",
	       (unsigned long long)name, wire);
	printf("      identity=%u  manufacturer=%u  function=%u  "
	       "ecu_inst=%u  arb_addr=%u\n",
	       n.identity, n.manufacturer, n.function,
	       n.ecu_instance, n.arbitrary_addr);
}

int main(int argc, char *argv[])
{
	if (argc < 4) {
		fprintf(stderr,
			"用法: %s <接口> <地址> <NAME> [--quiet]\n"
			"  地址/NAME 可写 0x80 / 128 两种形式\n"
			"  例:  %s vxcan0 0x80 0x1000\n",
			argv[0], argv[0]);
		return 1;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	const char *ifname = argv[1];
	unsigned sa = (unsigned)strtoul(argv[2], NULL, 0);
	uint64_t name = strtoull(argv[3], NULL, 0);
	int quiet = 0;
	int claim_period = 0;

	for (int i = 4; i < argc; i++) {
		if (!strcmp(argv[i], "--quiet"))
			quiet = 1;
		else if (!strcmp(argv[i], "--claim-period") && i + 1 < argc)
			claim_period = atoi(argv[++i]);
	}

	if (sa > J1939_MAX_UNICAST_ADDR) {
		fprintf(stderr, "[-] 地址 0x%02X 超出单播范围（0x00~0xFD）\n", sa);
		return 1;
	}
	if (name == 0) {
		fprintf(stderr, "[-] NAME 不能为 0\n");
		return 1;
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	int s = j1939_open(ifname, name, (uint8_t)sa);

	if (s < 0)
		return 1;

	printf("=== 地址声明 ===\n");
	printf("  接口 %s   期望地址 0x%02X\n", ifname, sa);
	print_name(name);

	/*
	 * 声明帧的 ID 可以直接算出来，便于和 candump 里的 18EEFFxx 对照：
	 *   优先级 6  | PF=0xEE（PDU1）| PS=0xFF（广播）| SA
	 */
	printf("  声明帧 ID 应为 0x%08X  (prio=6 PF=0xEE PS=0xFF SA=0x%02X)\n",
	       (6u << 26) | (0xEEu << 16) | (0xFFu << 8) | sa, sa);

	if (j1939_claim(s, name) < 0)
		printf("  [!] 发送声明失败: %s\n", strerror(errno));
	else
		printf("  已广播声明，等待 250 ms 看有无竞争者\n");

	printf("\n");
	fflush(stdout);

	/* 业务报文：EEC1，8 字节。用它来验证「到底能不能发」 */
	uint8_t eec1[8] = { 0 };
	unsigned rpm_raw = 800 * 8;	/* EEC1 转速分辨率 0.125 rpm/bit */
	int verdict = 0;		/* 0=未知 1=赢 -1=输 */
	int nprobe = 0;
	long long next_claim = now_ms() + claim_period;
	long long next_probe = now_ms() + 1000;

	while (!g_stop) {
		long long now = now_ms();

		if (claim_period > 0 && now >= next_claim) {
			j1939_claim(s, name);
			printf("[..] 重播地址声明（%d ms 周期）\n", claim_period);
			next_claim = now + claim_period;
		}

		struct pollfd pfd = { .fd = s, .events = POLLIN };
		int pr = poll(&pfd, 1, 200);

		if (pr > 0 && (pfd.revents & POLLIN)) {
			uint8_t buf[256];
			struct sockaddr_can from;
			struct iovec iov = { buf, sizeof(buf) };
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

			for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
				if (cm->cmsg_level == SOL_CAN_J1939 &&
				    cm->cmsg_type == SCM_J1939_DEST_ADDR) {
					/* 内核给出的是 8 位，但为了对齐按 u64 读 */
					da = *(uint8_t *)CMSG_DATA(cm);
				}
			}

			uint32_t pgn = from.can_addr.j1939.pgn;
			unsigned ssa = from.can_addr.j1939.addr;

			if (pgn == PGN_REQUEST && n >= 3) {
				uint32_t req = (uint32_t)buf[0] |
					       ((uint32_t)buf[1] << 8) |
					       ((uint32_t)buf[2] << 16);

				printf("\n[收到请求] SA=0x%02X 请求 PGN=0x%05X(%s)\n",
				       ssa, req, j1939_pgn_name(req));
				if (req == PGN_ADDRESS_CLAIM) {
					/*
					 * 规范要求：收到「请求地址声明」必须应答。
					 * 内核不做这件事，所以新上线的节点会以为总线上只有自己。
					 */
					j1939_claim(s, name);
					printf("  -> 已重播本机声明"
					       "（这是应用层义务，内核不会替你做）\n");
				}
				continue;
			}

			if (pgn == PGN_ADDRESS_CLAIM && n == 8) {
				uint64_t their;
				char h[32];

				memcpy(&their, buf, 8);
				hexdump(buf, 8, h, sizeof(h));
				printf("\n[收到地址声明] SA=0x%02X  DA=0x%02X  data=%s\n",
				       ssa, da, h);
				printf("  对方 ");
				print_name(their);

				if (ssa == sa) {
					/*
					 * 这正是仲裁发生的时刻。双方都声明了同一个地址，
					 * 谁赢只看 NAME 大小。
					 */
					int c = j1939_name_cmp(name, their);

					if (c == 0) {
						printf("  -> 同一个 NAME，视为同一 ECU，不算冲突\n");
					} else if (c < 0) {
						verdict = 1;
						printf("  -> 争用 0x%02X：本机 NAME 更小，**本机赢**，"
						       "对方应换地址\n", sa);
					} else {
						verdict = -1;
						printf("  -> 争用 0x%02X：本机 NAME 更大，**本机输**，"
						       "应改用其他地址\n", sa);
						printf("     内核已把本机从 0x%02X 上摘掉，"
						       "后续业务报文会被拒绝\n", sa);
					}
				} else {
					printf("  -> 对方占用 0x%02X（与本机 0x%02X 不冲突）\n",
					       ssa, sa);
				}
			} else {
				char h[64];

				hexdump(buf, (int)n, h, sizeof(h));
				printf("[RX] PGN=0x%05X(%-14s) SA=0x%02X DA=0x%02X len=%zd  %s\n",
				       pgn, j1939_pgn_name(pgn), ssa, da, n,
				       n <= 8 ? h : "(多帧，已由内核重组)");
			}
			continue;
		}

		/* 每秒探测一次：输了仲裁之后，这里会一直失败 */
		if (quiet || g_stop || now_ms() < next_probe)
			continue;

		next_probe = now_ms() + 1000;

		eec1[0] = 0x00;
		eec1[1] = 20 + 125;			/* 需求扭矩 % ，偏移 -125 */
		eec1[2] = 15 + 125;			/* 实际扭矩 % */
		eec1[3] = (uint8_t)(rpm_raw & 0xFF);	/* 转速低字节 */
		eec1[4] = (uint8_t)(rpm_raw >> 8);	/* 转速高字节 */
		eec1[5] = 0x00;
		eec1[6] = 0x00;
		eec1[7] = 0x00;

		int r = j1939_send(s, PGN_EEC1, J1939_NO_ADDR, eec1, sizeof(eec1));

		nprobe++;
		if (r < 0) {
			printf("[TX %2d] 发送 EEC1 失败: %s (errno=%d)\n",
			       nprobe, strerror(errno), errno);
			if (errno == EADDRNOTAVAIL)
				printf("        原因：本机 NAME 没有映射到有效单播地址"
				       "（地址被更强的 NAME 抢走，或声明还没生效）\n");
		} else if (nprobe % 5 == 1) {
			printf("[TX %2d] 发送 EEC1 成功 %d 字节，rpm=%u.%03u\n",
			       nprobe, r, rpm_raw / 8, (rpm_raw % 8) * 125);
		}
		rpm_raw += 8;	/* 每秒 +1 rpm，便于在接收端看出是活数据 */
	}

	printf("\n=== 退出 ===\n");
	if (verdict < 0)
		printf("  本次运行在 0x%02X 的争用中失败\n", sa);
	else if (verdict > 0)
		printf("  本次运行保住了 0x%02X\n", sa);
	close(s);
	return 0;
}
