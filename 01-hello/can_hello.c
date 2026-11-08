/*
 * 阶段 1 —— SocketCAN 最小收发（can_hello）
 *
 * 目标：用尽量少的代码走通「socket -> bind -> write -> read」全流程，
 *       同时把 SocketCAN 的四个关键对象看清楚：
 *
 *         socket(PF_CAN, SOCK_RAW, CAN_RAW)   一个 CAN 套接字
 *         struct sockaddr_can { can_family, can_ifindex }  绑到哪个接口
 *         struct can_frame    { can_id, can_dlc, data[8] } 一帧
 *         控制选项             LOOPBACK / RECV_OWN_MSGS / SO_RCVTIMEO
 *
 * 编译： make 01-hello/can_hello
 * 运行： ./01-hello/can_hello [ifname]        默认 vcan0
 *
 * ★ 关于「回环」有两把独立的开关，非常容易搞混：
 *
 *     CAN_RAW_LOOPBACK      默认 1   本帧要不要被送回本接口（别人能不能看见）
 *     CAN_RAW_RECV_OWN_MSGS 默认 0   发送者自己的套接字要不要收自己发的帧
 *
 *   默认组合 = 「别人看得见，自己看不见」。
 *   所以单进程想跑通收发闭环，必须显式打开 RECV_OWN_MSGS。
 *   只开 LOOPBACK 是不够的 —— 这是新手第一个坑，本示例用 [A] 和 [D]
 *   两个小节把差别演出来。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/can.h>
#include <linux/can/raw.h>

static void die(const char *what)
{
	fprintf(stderr, "[-] %s 失败: %s\n", what, strerror(errno));
	exit(EXIT_FAILURE);
}

/*
 * 打开一个绑定到 ifname 的 CAN_RAW 套接字。
 *
 * 注意 bind 的是「接口索引」而不是接口名：
 *   CAN 套接字不是 IP 套接字，没有地址/端口的概念，
 *   bind 的语义是「从此我只收发这个网络接口上的帧」。
 */
static int can_open(const char *ifname, int loopback, int recv_own)
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

	/* 开关一：发出去的帧要不要送回本接口 */
	int v = loopback ? 1 : 0;
	if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &v, sizeof(v)) < 0)
		die("setsockopt(CAN_RAW_LOOPBACK)");

	/* 开关二：自己的套接字要不要收自己发的帧 */
	v = recv_own ? 1 : 0;
	if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &v, sizeof(v)) < 0)
		die("setsockopt(CAN_RAW_RECV_OWN_MSGS)");

	/* 收超时：没有帧时 read() 返回 -1/EAGAIN，而不是永久阻塞 */
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
		die("setsockopt(SO_RCVTIMEO)");

	printf("    接口 %-6s ifindex=%-3u fd=%d  loopback=%s recv_own=%s\n",
	       ifname, ifindex, s,
	       loopback ? "on" : "off", recv_own ? "on" : "off");
	return s;
}

/* 把一帧格式化成 candump 风格的一行，方便和 candump 的输出对照 */
static void fmt_frame(const struct can_frame *f, char *out, size_t n)
{
	size_t off = 0;

	if (f->can_id & CAN_EFF_FLAG)	/* 扩展帧：29 位 ID */
		off += snprintf(out + off, n - off, "%08X",
				(unsigned)(f->can_id & CAN_EFF_MASK));
	else				/* 标准帧：11 位 ID */
		off += snprintf(out + off, n - off, "     %03X",
				(unsigned)(f->can_id & CAN_SFF_MASK));

	off += snprintf(out + off, n - off, "  [%u]", f->can_dlc);

	for (int i = 0; i < f->can_dlc && i < 8; i++)
		off += snprintf(out + off, n - off, " %02X", f->data[i]);

	if (f->can_id & CAN_RTR_FLAG)
		off += snprintf(out + off, n - off, "  (RTR 远程请求帧)");
}

/* 发送一帧，然后立刻尝试收回环帧 */
static void tx_rx(int s, const struct can_frame *out, const char *note)
{
	char line[160];

	fmt_frame(out, line, sizeof(line));
	printf("  TX  %s   %s\n", line, note ? note : "");

	/* 一次 write 必须写完整帧（16 字节的 struct can_frame），
	 * 内核不接受「半帧」。write() 和 send() 在这里完全等价。 */
	if (write(s, out, sizeof(*out)) != (ssize_t)sizeof(*out))
		die("write");

	struct can_frame in;
	ssize_t n = read(s, &in, sizeof(in));
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			printf("  RX  <1 秒内没有帧回来>\n");
		else
			die("read");
		return;
	}

	fmt_frame(&in, line, sizeof(line));
	printf("  RX  %s   <- 本套接字收到\n", line);
}

int main(int argc, char *argv[])
{
	const char *ifname = (argc > 1) ? argv[1] : "vcan0";

	printf("=== 阶段 1：SocketCAN 最小收发 ===\n\n");

	/* ================= A. 标准帧：完整收发闭环 ================= */
	printf("[A] 标准帧（11 位 ID），loopback=on + recv_own=on\n");
	int s = can_open(ifname, 1, 1);

	struct can_frame f;
	memset(&f, 0, sizeof(f));
	/*
	 * 重要：标准 SocketCAN 里 can_id 直接写 11 位 ID 本身，
	 * 内核会用 CAN_SFF_MASK 截断。你在某些老代码里看到的
	 * "id << 21" 是别的私有实现（SLCAN / 自定义驱动）的打包方式，
	 * 不是 SocketCAN 的用法，不要照抄。
	 */
	f.can_id  = 0x123;
	f.can_dlc = 8;
	const uint8_t payload_a[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03 };
	memcpy(f.data, payload_a, 8);
	tx_rx(s, &f, "can_id=0x123 dlc=8");

	/* ================= B. 扩展帧 ================= */
	printf("\n[B] 扩展帧（29 位 ID），同一个套接字\n");
	memset(&f, 0, sizeof(f));
	f.can_id  = 0x18FF50E5 | CAN_EFF_FLAG;	/* 标志位和 ID 在同一个 32 位字段里 */
	f.can_dlc = 8;
	const uint8_t payload_b[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
	memcpy(f.data, payload_b, 8);
	tx_rx(s, &f, "can_id=0x18FF50E5 | CAN_EFF_FLAG");

	/*
	 * can_id 这 32 位其实是「3 个标志位 + 29 位 ID」：
	 *   bit31 CAN_EFF_FLAG  扩展帧
	 *   bit30 CAN_RTR_FLAG  远程请求帧（只问数据，不带数据）
	 *   bit29 CAN_ERR_FLAG  错误帧（只能收，不能发）
	 * 所以读一帧时永远要先剥掉高位再比较 ID，否则 0x123 != 0x123。
	 */

	/* ================= C. 零长度帧 ================= */
	printf("\n[C] DLC=0（经典 CAN 允许零长度帧，常用来做心跳/唤醒）\n");
	memset(&f, 0, sizeof(f));
	f.can_id  = 0x123;
	f.can_dlc = 0;
	tx_rx(s, &f, "can_dlc=0");

	close(s);

	/* ================= D. 只关 RECV_OWN_MSGS ================= */
	printf("\n[D] loopback=on 但 recv_own=off：帧照发，自己不收自己的\n");
	s = can_open(ifname, 1, 0);
	memset(&f, 0, sizeof(f));
	f.can_id  = 0x123;
	f.can_dlc = 1;
	f.data[0] = 0xAA;
	tx_rx(s, &f, "recv_own=off");
	printf("    说明：这一帧确实上了总线，用 candump 另开一个套接字能看到；\n"
	       "          本进程之所以收不到，是因为 RECV_OWN_MSGS 关着。\n"
	       "          真实 ECU 通常就是这个配置——只关心别人发的。\n");
	close(s);

	/* ================= E. 再关 LOOPBACK ================= */
	printf("\n[E] loopback=off：连回环都没有了\n");
	s = can_open(ifname, 0, 1);
	memset(&f, 0, sizeof(f));
	f.can_id  = 0x123;
	f.can_dlc = 2;
	f.data[0] = 0xBB;
	f.data[1] = 0xCC;
	tx_rx(s, &f, "loopback=off, recv_own=on");
	printf("    说明：vcan 的「总线」本身就是回环，关掉 LOOPBACK 等于这一帧\n"
	       "          被直接丢弃——连 candump 都看不到它（可以自己验证）。\n"
	       "          真实硬件不同：控制器关掉 LOOPBACK 只是不往自己这边回，\n"
	       "          总线上其他节点照样收得到。这是 vcan 特有的语义差异。\n");
	close(s);

	printf("\n完成。\n");
	return 0;
}
