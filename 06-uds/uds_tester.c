/*
 * uds_tester.c —— 一个最小的诊断仪
 *
 * 它做两件事：
 *   1. 交互模式：手敲十六进制，逐条发请求
 *   2. 自动模式：跑一套完整的诊断序列，覆盖正响应和各类负响应
 *
 * 自动序列的设计思路是「先证明不该通过的确实不通过」：
 * 没解锁时写 VIN 必须被拒，复位后会话必须掉回默认，DID 不存在必须
 * 报 0x31 —— 这些负向用例比正向用例更能说明协议真的在工作。
 *
 * 用法：
 *   uds_tester -i vxcan0 --auto
 *   uds_tester -i vxcan0            # 交互，输入 "10 03" 这样的十六进制
 *   uds_tester -i vxcan0 --s3       # 验证 S3 超时（要等 6 秒）
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/isotp.h>

#include "uds.h"

#define REQ_ID	0x7E0
#define RESP_ID	0x7E8

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int open_isotp(const char *ifname)
{
	int s = socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP);

	if (s < 0) {
		fprintf(stderr, "[-] socket: %s（modprobe can-isotp）\n",
			strerror(errno));
		return -1;
	}

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = (int)if_nametoindex(ifname);
	addr.can_addr.tp.rx_id = RESP_ID;
	addr.can_addr.tp.tx_id = REQ_ID;

	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[-] bind: %s\n", strerror(errno));
		close(s);
		return -1;
	}

	return s;
}

static void hexdump(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02X ", p[i]);
}

/*
 * 发一条请求并等响应。
 * 特别处理 NRC 0x78：服务器说"还在处理"，此时必须继续等 P2* 那么久，
 * 而不是直接判超时。这是 UDS 里最容易被忽略的一条时序规则。
 */
static int transact(int s, const uint8_t *q, size_t qn,
		    uint8_t *r, size_t rmax, int quiet)
{
	if (!quiet) {
		printf("  > ");
		hexdump(q, qn);
		printf("\n");
	}

	ssize_t w = write(s, q, qn);

	if (w != (ssize_t)qn) {
		printf("  ! 发送失败: %s\n", strerror(errno));
		return -1;
	}

	long long t0 = now_ms();
	int timeout = UDS_P2_MS * 4;

	for (;;) {
		struct pollfd p = { .fd = s, .events = POLLIN };
		int pr = poll(&p, 1, timeout);

		if (pr <= 0) {
			printf("  ! 超时（等了 %d ms）\n", timeout);
			return -1;
		}

		ssize_t n = read(s, r, rmax);

		if (n <= 0) {
			printf("  ! 接收失败: %s\n", strerror(errno));
			return -1;
		}

		long long dt = now_ms() - t0;

		if (n >= 3 && r[0] == 0x7F && r[2] == UDS_NRC_PENDING) {
			printf("    (NRC 0x78 responsePending，服务器还在处理，"
			       "等待延长到 %d ms)\n", UDS_P2STAR_MS);
			timeout = UDS_P2STAR_MS;
			t0 = now_ms();
			continue;
		}

		if (!quiet) {
			printf("  < ");
			hexdump(r, (size_t)n);
			printf("   (%lld ms)\n", dt);

			if (r[0] == 0x7F)
				printf("    负响应：%s / NRC 0x%02X %s\n",
				       uds_sid_name(r[1]), r[2],
				       uds_nrc_name(r[2]));
		}

		return (int)n;
	}
}

/* 期望正响应（首字节 = SID + 0x40） */
static int expect_pos(int s, const char *name, const uint8_t *q, size_t qn,
		      uint8_t *r, int *rn)
{
	printf("\n[用例] %s\n", name);

	int n = transact(s, q, qn, r, 256, 0);

	if (n <= 0) {
		printf("  => FAIL（没有响应）\n");
		return -1;
	}

	*rn = n;

	if (r[0] == (uint8_t)(q[0] + 0x40)) {
		printf("  => PASS\n");
		return 0;
	}

	printf("  => FAIL（期望 %02X，实际 %02X）\n",
	       q[0] + 0x40, r[0]);
	return -1;
}

/* 期望指定 NRC */
static int expect_nrc(int s, const char *name, const uint8_t *q, size_t qn,
		      uint8_t want)
{
	printf("\n[用例] %s\n", name);

	uint8_t r[256];
	int n = transact(s, q, qn, r, sizeof(r), 0);

	if (n < 3) {
		printf("  => FAIL（响应太短）\n");
		return -1;
	}

	if (r[0] == 0x7F && r[2] == want) {
		printf("  => PASS（正确拒绝：%s）\n", uds_nrc_name(want));
		return 0;
	}

	printf("  => FAIL（期望 NRC %02X，实际 %02X）\n",
	       want, r[0] == 0x7F ? r[2] : 0);
	return -1;
}

static int parse_hex(const char *s, uint8_t *out, int max)
{
	int n = 0;

	while (*s && n < max) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s || *s == '\n')
			break;

		unsigned v;

		if (sscanf(s, "%2x", &v) != 1)
			break;
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

static int run_auto(int s)
{
	uint8_t r[256];
	int rn = 0, fail = 0;
	uint8_t q[64];

	/* ---- 1. 进扩展会话 ---- */
	q[0] = 0x10; q[1] = 0x03;
	if (expect_pos(s, "进扩展会话 10 03", q, 2, r, &rn) < 0)
		fail++;
	else
		printf("     P2=%dms  P2*=%dms\n",
		       (r[2] << 8) | r[3], (r[4] << 8) | r[5]);

	/* ---- 2. 读 VIN ---- */
	q[0] = 0x22; q[1] = 0xF1; q[2] = 0x90;
	if (expect_pos(s, "读 VIN 22 F1 90", q, 3, r, &rn) < 0) {
		fail++;
	} else {
		printf("     VIN = %.*s\n", rn - 3, r + 3);
	}

	/* ---- 3. 未解锁就写 VIN，必须被拒 ---- */
	uint8_t wq[3 + 17];

	wq[0] = 0x2E; wq[1] = 0xF1; wq[2] = 0x90;
	memcpy(wq + 3, "LSVBB987654321098", 17);

	if (expect_nrc(s, "未解锁写 VIN，应被拒（33）", wq, sizeof(wq),
		       UDS_NRC_SECURITY_DENIED) < 0)
		fail++;

	/* ---- 4. 请求种子 ---- */
	q[0] = 0x27; q[1] = 0x01;

	if (expect_pos(s, "请求安全种子 27 01", q, 2, r, &rn) < 0) {
		fail++;
	} else {
		uint32_t seed = ((uint32_t)r[2] << 24) | ((uint32_t)r[3] << 16) |
				((uint32_t)r[4] << 8) | r[5];
		uint32_t key = uds_key_from_seed(seed);

		printf("     seed=0x%08X  key=0x%08X（算法见 uds.h）\n",
		       seed, key);

		/* ---- 5. 发密钥 ---- */
		uint8_t kq[6];

		kq[0] = 0x27; kq[1] = 0x02;
		kq[2] = (uint8_t)(key >> 24);
		kq[3] = (uint8_t)(key >> 16);
		kq[4] = (uint8_t)(key >> 8);
		kq[5] = (uint8_t)key;

		if (expect_pos(s, "发送密钥 27 02", kq, 6, r, &rn) < 0)
			fail++;
	}

	/* ---- 6. 解锁后写 VIN，应该成功 ---- */
	if (expect_pos(s, "解锁后写 VIN 2E F1 90", wq, sizeof(wq),
		       r, &rn) < 0)
		fail++;

	/* ---- 7. 读回验证 ---- */
	q[0] = 0x22; q[1] = 0xF1; q[2] = 0x90;

	if (expect_pos(s, "读回 VIN 验证写入", q, 3, r, &rn) < 0) {
		fail++;
	} else {
		printf("     VIN = %.*s %s\n", rn - 3, r + 3,
		       (rn == 20 && !memcmp(r + 3, "LSVBB987654321098", 17)) ?
		       "(与写入一致)" : "(不一致！)");
	}

	/* ---- 8. 读 DTC ---- */
	q[0] = 0x19; q[1] = 0x02; q[2] = 0xFF;

	if (expect_pos(s, "读全部 DTC 19 02 FF", q, 3, r, &rn) < 0) {
		fail++;
	} else {
		int cnt = (rn - 3) / 4;

		printf("     共 %d 条 DTC：\n", cnt);
		for (int i = 0; i < cnt; i++) {
			const uint8_t *d = r + 3 + i * 4;

			printf("       %02X %02X %02X  status=0x%02X\n",
			       d[0], d[1], d[2], d[3]);
		}
	}

	/* ---- 9. 读活数据 ---- */
	q[0] = 0x22; q[1] = 0x01; q[2] = 0x00;

	if (expect_pos(s, "读实时转速 22 01 00", q, 3, r, &rn) < 0) {
		fail++;
	} else {
		printf("     rpm = %d\n", (r[3] << 8) | r[4]);
	}

	/* ---- 10. 不存在的 DID，必须回 0x31 ---- */
	q[0] = 0x22; q[1] = 0xDE; q[2] = 0xAD;

	if (expect_nrc(s, "读不存在的 DID，应回 31", q, 3,
		       UDS_NRC_OUT_OF_RANGE) < 0)
		fail++;

	/* ---- 11. 长度错的请求，必须回 0x13 ---- */
	q[0] = 0x10; q[1] = 0x03; q[2] = 0x00; q[3] = 0x00;

	if (expect_nrc(s, "会话控制多带字节，应回 13", q, 4,
		       UDS_NRC_INCORRECT_LENGTH) < 0)
		fail++;

	/* ---- 12. 不支持的服务 ---- */
	q[0] = 0x31;

	if (expect_nrc(s, "不支持的服务 31，应回 11", q, 1,
		       UDS_NRC_SERVICE_UNSUPPORTED) < 0)
		fail++;

	/* ---- 13. ECU 复位，会话应回默认 ---- */
	q[0] = 0x11; q[1] = 0x01;

	if (expect_pos(s, "ECU 复位 11 01", q, 2, r, &rn) < 0)
		fail++;

	/* ---- 14. 复位后写 VIN，应因会话不对被拒 ---- */
	if (expect_nrc(s, "复位后写 VIN，应回 7E（会话不对）", wq,
		       sizeof(wq), UDS_NRC_CONDITIONS) < 0)
		fail++;

	/* ---- 15. 复位后读 VIN 仍应成功（数据不丢） ---- */
	q[0] = 0x22; q[1] = 0xF1; q[2] = 0x90;

	if (expect_pos(s, "复位后读 VIN", q, 3, r, &rn) < 0) {
		fail++;
	} else {
		printf("     VIN = %.*s\n", rn - 3, r + 3);
	}

	printf("\n===== 自动序列结束：%s（失败 %d 项）=====\n",
	       fail ? "有失败" : "全部通过", fail);
	return fail ? 1 : 0;
}

/* 验证 S3：进扩展会话后闲置，看服务器是否自动退回默认 */
static int run_s3(int s)
{
	uint8_t r[256];
	int rn = 0, fail = 0;
	uint8_t q[8];

	q[0] = 0x10; q[1] = 0x03;
	if (expect_pos(s, "进扩展会话", q, 2, r, &rn) < 0)
		return 1;

	printf("\n[等待] 闲置 %d ms，期间不发任何报文...\n", UDS_S3_MS + 1000);
	fflush(stdout);
	usleep((useconds_t)(UDS_S3_MS + 1000) * 1000);

	printf("\n[用例] 超时后写 VIN（应因会话已回默认而被拒 7E）\n");

	uint8_t wq[3 + 17];

	wq[0] = 0x2E; wq[1] = 0xF1; wq[2] = 0x90;
	memcpy(wq + 3, "LSVBB987654321098", 17);

	if (expect_nrc(s, "S3 超时后写 VIN，应回 7E", wq, sizeof(wq),
		       UDS_NRC_CONDITIONS) < 0)
		fail++;

	printf("\n===== S3 验证：%s =====\n", fail ? "失败" : "通过");
	return fail ? 1 : 0;
}

int main(int argc, char *argv[])
{
	const char *ifname = "vxcan0";
	int auto_mode = 0, s3_mode = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			ifname = argv[++i];
		} else if (!strcmp(argv[i], "--auto")) {
			auto_mode = 1;
		} else if (!strcmp(argv[i], "--s3")) {
			s3_mode = 1;
		} else {
			fprintf(stderr,
				"用法: %s [-i 接口] [--auto | --s3]\n",
				argv[0]);
			return 1;
		}
	}

	int s = open_isotp(ifname);

	if (s < 0)
		return 1;

	printf("诊断仪  接口 %s  发 0x%03X  收 0x%03X\n",
	       ifname, REQ_ID, RESP_ID);
	printf("P2=%dms  P2*=%dms  S3=%dms\n", UDS_P2_MS, UDS_P2STAR_MS,
	       UDS_S3_MS);

	if (auto_mode)
		return run_auto(s);

	if (s3_mode)
		return run_s3(s);

	printf("\n交互模式：输入十六进制字节，如 \"10 03\"，直接回车退出\n");

	char line[256];

	for (;;) {
		printf("\nuds> ");
		fflush(stdout);

		if (!fgets(line, sizeof(line), stdin))
			break;

		uint8_t q[64];
		int qn = parse_hex(line, q, sizeof(q));

		if (qn == 0)
			break;

		uint8_t r[256];

		transact(s, q, (size_t)qn, r, sizeof(r), 0);
	}

	close(s);
	return 0;
}
