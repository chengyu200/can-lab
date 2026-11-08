/*
 * isotp_test.c —— 用户态 ISO-TP 的验证驱动
 *
 * 用 fork 起一个接收端子进程，父进程做发送端，逐档跑边界长度。
 * 每一档都走完整的「分片 -> 流控 -> 组包 -> 比对」链路。
 *
 * 长度不是随便挑的：
 *   1    单帧最短
 *   7    单帧上限（SF 的长度字段只有 4 位）
 *   8    第一个必须走 FF 的长度，最容易写错
 *   20   典型的 UDS 多帧请求
 *   100  序号回绕 6 次
 *   1000 大包，压 STmin 与调度
 *   4000 接近 12 位长度上限，序号回绕 285 次
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "isotp_user.h"

#define TX_ID	0x700
#define RX_ID	0x701

static uint8_t txbuf[ISOTP_MAX_MSG];
static uint8_t rxbuf[ISOTP_MAX_MSG];
static size_t  g_len;

static long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* 可复现的数据，且相邻字节不同，能看出错位 */
static void fill(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (uint8_t)(i * 7 + 3);
}

static int child_recv(const char *ifname, int notify_fd)
{
	struct isotp_chan c;
	ssize_t n;

	if (isotp_open(&c, ifname, RX_ID, TX_ID) < 0) {
		fprintf(stderr, "  [-] 接收端打开 %s 失败: %s\n",
			ifname, strerror(errno));
		return 2;
	}

	/* 告诉父进程：我准备好了，可以发 */
	if (write(notify_fd, "r", 1) != 1)
		return 2;
	close(notify_fd);

	n = isotp_recv(&c, rxbuf, sizeof(rxbuf), 8000);

	if (n < 0) {
		fprintf(stderr, "  [-] 接收失败: %s\n", strerror(errno));
		return 3;
	}

	if ((size_t)n != g_len) {
		fprintf(stderr, "  [-] 长度不符：收到 %zd，期望 %zu\n",
			n, g_len);
		return 4;
	}

	if (memcmp(rxbuf, txbuf, g_len) != 0) {
		for (size_t i = 0; i < g_len; i++) {
			if (rxbuf[i] != txbuf[i]) {
				fprintf(stderr,
					"  [-] 内容不符：第 %zu 字节 收到 %02X 期望 %02X\n",
					i, rxbuf[i], txbuf[i]);
				break;
			}
		}
		return 5;
	}

	isotp_close(&c);
	return 0;
}

static int run_case(const char *txif, const char *rxif, size_t len,
		    int verbose, int no_peer)
{
	int fds[2];
	pid_t pid = -1;
	int status = 0;
	char ch;

	g_len = len;
	fill(txbuf, len);

	/*
	 * no_peer = 1 时不 fork 内置接收端，改由外部程序（isotp_fault）
	 * 扮演对端。故障注入必须这么测：坏行为在另一个进程里。
	 */
	if (!no_peer) {
		if (pipe(fds) < 0) {
			perror("pipe");
			return -1;
		}

		pid = fork();

		if (pid < 0) {
			perror("fork");
			return -1;
		}

		if (pid == 0) {
			close(fds[0]);
			_exit(child_recv(rxif, fds[1]));
		}

		close(fds[1]);

		/* 等子进程把 socket 建好，避免「发送早于接收端就绪」的假失败 */
		if (read(fds[0], &ch, 1) != 1) {
			fprintf(stderr, "  [-] 子进程未就绪\n");
			waitpid(pid, &status, 0);
			return -1;
		}
		close(fds[0]);
	}

	struct isotp_chan c;

	if (isotp_open(&c, txif, TX_ID, RX_ID) < 0) {
		fprintf(stderr, "  [-] 发送端打开 %s 失败: %s\n",
			txif, strerror(errno));
		if (pid > 0)
			waitpid(pid, &status, 0);
		return -1;
	}
	c.verbose = verbose;

	long long t0 = now_us();
	int rc = isotp_send(&c, txbuf, len);
	long long dt = now_us() - t0;

	if (pid > 0)
		waitpid(pid, &status, 0);

	int ok = (rc == 0);

	if (!no_peer && !(WIFEXITED(status) && WEXITSTATUS(status) == 0))
		ok = 0;

	printf("  %-6zu %-8s %8.2f ms  TX %lu 帧  RX %lu 帧  %s\n",
	       len,
	       ok ? "PASS" : "FAIL",
	       dt / 1000.0,
	       c.n_tx_frames, c.n_rx_frames,
	       ok ? "" : (errno == ETIMEDOUT ? "(超时)" : "(错误)"));

	isotp_close(&c);
	return ok ? 0 : -1;
}

/*
 * 独立验证 N_Bs 超时：故意不启动接收端，看发送端多久放弃。
 * 期望是 1 秒左右（ISOTP_N_BS_MS），太短说明没在等 FC，
 * 太长说明超时逻辑根本没生效。
 */
static int run_timeout_case(const char *txif)
{
	struct isotp_chan c;

	if (isotp_open(&c, txif, TX_ID, RX_ID) < 0) {
		perror("open");
		return -1;
	}

	fill(txbuf, 20);

	long long t0 = now_us();
	int rc = isotp_send(&c, txbuf, 20);
	long long dt = now_us() - t0;

	int ok = (rc < 0 && errno == ETIMEDOUT &&
		  dt > 800000 && dt < 1600000);

	printf("  %-6s %-8s %8.2f ms  TX %lu 帧  RX %lu 帧  期望 1000ms 超时\n",
	       "20", ok ? "PASS" : "FAIL", dt / 1000.0,
	       c.n_tx_frames, c.n_rx_frames);

	isotp_close(&c);
	return ok ? 0 : -1;
}

int main(int argc, char *argv[])
{
	const char *txif = "vxcan0";
	const char *rxif = "vxcan1";
	int verbose = 0;
	int no_peer = 0;
	int do_all = 1;
	int do_timeout = 0;
	long only_len = -1;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			txif = argv[++i];
		} else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
			rxif = argv[++i];
		} else if (!strcmp(argv[i], "-v")) {
			verbose = 1;
		} else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
			only_len = strtol(argv[++i], NULL, 0);
			do_all = 0;
		} else if (!strcmp(argv[i], "--timeout")) {
			do_timeout = 1;
			do_all = 0;
		} else if (!strcmp(argv[i], "--no-peer")) {
			no_peer = 1;
		} else {
			fprintf(stderr,
				"用法: %s [-i 发送接口] [-p 接收接口] [-v] "
				"[-l 长度 | --timeout | --no-peer]\n"
				"  --no-peer  不启动内置接收端，改由外部程序做对端\n"
				"             （配合 isotp_fault 做故障注入）\n",
				argv[0]);
			return 1;
		}
	}

	printf("ISO-TP 用户态实现验证  发送端 %s (0x%03X)  ->  "
	       "接收端 %s (0x%03X)\n\n", txif, TX_ID, rxif, RX_ID);
	printf("  %-6s %-8s %11s  %s\n", "长度", "结果", "耗时", "帧统计");

	int fail = 0;

	if (do_all) {
		const size_t lens[] = { 1, 7, 8, 20, 100, 1000, 4000 };

		for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
			if (run_case(txif, rxif, lens[i], verbose, no_peer) < 0)
				fail++;
	}

	if (only_len > 0) {
		if (run_case(txif, rxif, (size_t)only_len, verbose,
			     no_peer) < 0)
			fail++;
	}

	if (do_timeout) {
		if (run_timeout_case(txif) < 0)
			fail++;
	}

	printf("\n%s（失败 %d 项）\n", fail ? "有失败" : "全部通过", fail);
	return fail ? 1 : 0;
}
