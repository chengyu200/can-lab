/*
 * 阶段 3 附赠 —— 表驱动的通用解码器 can_decode
 *
 * 这个程序只有 80 行，但它说明了「把协议做成数据」的最大好处：
 * 同一张表，ecu_engine 用来编码、ecu_dash 用来解码，
 * 这里用来做一个人肉版 candump —— 不需要为每个报文写解析代码。
 *
 * 真实项目里这个位置放的是 cantools（Python）或 DBC + CANoe，
 * 原理完全一样：读表 -> 逐帧解析 -> 输出物理值。
 *
 * 运行：
 *   ./03-app/can_decode              # 打印协议定义（DBC 的样子）
 *   ./03-app/can_decode vcan0        # 挂着总线上逐帧解码
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "can_proto.h"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		proto_print_defs(stdout);
		return 0;
	}

	const char *ifname = argv[1];

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (s < 0) {
		perror("socket");
		return 1;
	}

	unsigned int ifindex = if_nametoindex(ifname);

	if (ifindex == 0) {
		fprintf(stderr, "[-] 找不到接口 %s\n", ifname);
		return 1;
	}

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = (int)ifindex;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}

	printf("=== 逐帧解码 %s（Ctrl-C 结束）===\n", ifname);

	struct timespec t0, ts;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (!g_stop) {
		struct can_frame f;
		ssize_t n = recv(s, &f, sizeof(f), 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("recv");
			break;
		}
		if (n != (ssize_t)sizeof(f)) {
			printf("       -  (CAN FD 帧，%zd 字节，本工具只解经典帧)\n", n);
			continue;
		}

		clock_gettime(CLOCK_MONOTONIC, &ts);

		double rel = (double)(ts.tv_sec - t0.tv_sec) +
			     (double)(ts.tv_nsec - t0.tv_nsec) / 1e9;

		char line[192];

		proto_format(&f, line, sizeof(line));

		/* 原始字节也留着：解码结果错了，得能回头对原始数据 */
		printf("%7.3f  %s   [", rel, line);
		for (int i = 0; i < f.can_dlc; i++)
			printf("%s%02X", i ? " " : "", f.data[i]);
		printf("]\n");
		fflush(stdout);
	}

	close(s);
	return 0;
}
