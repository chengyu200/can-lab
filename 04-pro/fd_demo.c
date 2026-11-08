/*
 * 阶段 4 —— CAN FD：从 8 字节到 64 字节
 *
 * 经典 CAN 每帧最多 8 字节，500 kbps 下传 64 字节要 8 帧、约 2 ms。
 * CAN FD（Flexible Data-rate）做了两件事：
 *
 *   1) 有效载荷扩到 64 字节（DLC 9..15 表示 12/16/20/24/32/48/64）
 *   2) 数据段可以切到更高的位速率（BRS 位），仲裁段仍用低速保证兼容
 *
 * 对应用编程来说，变化只有三点：
 *
 *   - 结构体从 struct can_frame 换成 struct canfd_frame（72 字节）
 *   - 套接字要开 CAN_RAW_FD_FRAMES，否则 write 72 字节直接 EINVAL
 *   - recv 的缓冲区必须给 72 字节。给 16 字节的话，FD 帧会被截断
 *     （内核返回 72，你只读到 16，剩下的静默丢掉）
 *
 * vcan0 的 MTU=72 说明这条虚拟总线支持 FD，可以完整练习。
 *
 * 运行： ./04-pro/fd_demo vcan0
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "can_proto.h"

static int g_ifindex;

static int can_open(const char *ifname, int fd_mode, int recv_own)
{
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (s < 0) {
		perror("socket");
		exit(1);
	}

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = g_ifindex;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		exit(1);
	}

	if (fd_mode) {
		int on = 1;

		if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) < 0) {
			perror("setsockopt(CAN_RAW_FD_FRAMES)");
			exit(1);
		}
	}

	if (recv_own) {
		int on = 1;

		setsockopt(s, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &on, sizeof(on));
	}

	printf("  打开 %s：FD 模式=%s  自收回环=%s\n", ifname,
	       fd_mode ? "开" : "关", recv_own ? "开" : "关");
	return s;
}

/* FD 的 DLC 编码 -> 字节数，经典帧是线性的，FD 从这里开始就是台阶 */
static int dlc_to_len(int dlc)
{
	static const int tbl[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8,
				     12, 16, 20, 24, 32, 48, 64 };

	return (dlc >= 0 && dlc < 16) ? tbl[dlc] : -1;
}

static void hexdump(const uint8_t *d, int len)
{
	for (int i = 0; i < len && i < 8; i++)
		printf("%02X ", d[i]);
	if (len > 8)
		printf("... (%d 字节，末尾 %02X)", len, d[len - 1]);
}

int main(int argc, char *argv[])
{
	const char *ifname = (argc > 1) ? argv[1] : "vcan0";

	unsigned int idx = if_nametoindex(ifname);

	if (idx == 0) {
		fprintf(stderr, "[-] 找不到接口 %s\n", ifname);
		return 1;
	}
	g_ifindex = (int)idx;

	printf("=== CAN FD 演示 ===\n");
	printf("  CAN_MTU=%zu  CANFD_MTU=%zu  （sizeof can_frame / canfd_frame）\n\n",
	       sizeof(struct can_frame), sizeof(struct canfd_frame));

	/* ---------- DLC 编码表 ---------- */
	printf("[A] DLC 编码：经典帧 0..8 一一对应，FD 的 9..15 是台阶\n");
	printf("    DLC :");
	for (int dlc = 0; dlc < 16; dlc++)
		printf(" %3d", dlc);
	printf("\n    字节:");
	for (int dlc = 0; dlc < 16; dlc++)
		printf(" %3d", dlc_to_len(dlc));
	printf("\n    注意 DLC=9 表示 12 字节而不是 9 —— 8 到 64 之间只有 7 个档位，\n");
	printf("    发送时给 13 字节会被就近向上取整到 16。\n\n");

	/* ---------- B: 64 字节 FD 帧，自收 ---------- */
	printf("[B] 发 64 字节 FD 帧（带 BRS），自己收回来看\n");

	int s = can_open(ifname, 1, 1);
	struct canfd_frame f;

	memset(&f, 0, sizeof(f));
	f.can_id = CAN_ID_ENGINE_FAST;
	f.len    = 64;
	f.flags  = CANFD_BRS | CANFD_FDF;
	for (int i = 0; i < 64; i++)
		f.data[i] = (uint8_t)i;

	printf("    write %zu 字节 ... ", sizeof(f));
	if (write(s, &f, sizeof(f)) != (ssize_t)sizeof(f)) {
		printf("失败: %s\n", strerror(errno));
		close(s);
		return 1;
	}
	printf("ok\n");

	struct canfd_frame r;
	ssize_t n = read(s, &r, sizeof(r));

	if (n < 0) {
		printf("    读失败: %s\n", strerror(errno));
	} else {
		printf("    读到 %zd 字节  id=0x%03X len=%u flags=0x%02X%s%s\n",
		       n, r.can_id & CAN_SFF_MASK, r.len, r.flags,
		       (r.flags & CANFD_BRS) ? " [BRS]" : "",
		       (r.flags & CANFD_FDF) ? " [FDF]" : "");
		printf("    数据: ");
		hexdump(r.data, r.len);
		printf("\n");
	}

	/* ---------- C: 8 字节的 FD 帧与经典帧怎么区分 ---------- */
	printf("\n[C] 同样是 8 字节，FD 帧和经典帧差在哪\n");

	memset(&f, 0, sizeof(f));
	f.can_id = CAN_ID_ENGINE_SLOW;	/* 借用同一个 ID，模拟两种帧型混跑 */
	f.len    = 8;
	f.flags  = CANFD_FDF;		/* 关键：这个标志位说明「我是 FD 帧」 */
	memcpy(f.data, "\x01\x02\x03\x04\x05\x06\x07\x08", 8);
	write(s, &f, sizeof(f));

	/* 经典帧也走同一个 FD 套接字：len<=8 且不带 FDF 就是经典帧 */
	struct can_frame c;

	memset(&c, 0, sizeof(c));
	c.can_id  = CAN_ID_ENGINE_SLOW;
	c.can_dlc = 8;
	memcpy(c.data, "\xAA\xBB\xCC\xDD\xEE\xFF\x11\x22", 8);
	write(s, &c, sizeof(c));

	for (int i = 0; i < 2; i++) {
		n = read(s, &r, sizeof(r));
		if (n < 0)
			break;

		if (n == (ssize_t)CANFD_MTU && (r.flags & CANFD_FDF))
			printf("    收到 CAN FD 帧   len=%u [FDF]\n", r.len);
		else if (n == (ssize_t)CAN_MTU)
			printf("    收到 经典帧      dlc=%u\n", r.len);
		else
			printf("    收到 未知帧型    %zd 字节\n", n);
	}

	/* ---------- D: 没有开 FD 的套接字写 72 字节会怎样 ---------- */
	printf("\n[D] 未开 CAN_RAW_FD_FRAMES 的套接字，写 72 字节会失败\n");

	int s2 = can_open(ifname, 0, 1);

	memset(&f, 0, sizeof(f));
	f.can_id = CAN_ID_ENGINE_FAST;
	f.len    = 16;
	f.data[0] = 0x42;

	errno = 0;
	ssize_t w = write(s2, &f, sizeof(f));

	printf("    write 返回 %zd，errno=%d (%s)\n", w, errno, strerror(errno));
	printf("    结论：帧长度和套接字模式必须匹配，否则内核直接拒收，不发到线上。\n");

	close(s2);
	close(s);

	printf("\n=== 演示结束 ===\n");
	return 0;
}
