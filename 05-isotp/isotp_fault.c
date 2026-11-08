/*
 * isotp_fault.c —— 可控的（以及故意坏的）ISO-TP 接收端
 *
 * 正确路径谁都会写，真正花时间的是错误路径。ISO-TP 的错误处理分散在
 * 四个地方，而且互相牵连：
 *
 *   发送端等不到 FC      -> N_Bs 超时（这里用 nofc 制造）
 *   接收端缓冲区不够     -> 回 OVFLW（这里用 ovflw 制造）
 *   接收端忙不过来       -> 回 WAIT，发送端继续等（这里用 wait 制造）
 *   接收端发现丢帧       -> 序号不符，整条丢弃（这里用 drop 制造）
 *
 * 每种故障都对应真实场景：ECU 复位、缓冲区小、主循环被高优先级任务
 * 占住、总线干扰。在 vxcan 上这些都是软件可控的，比在真车上复现容易得多。
 *
 * 用法：
 *   isotp_fault -i vxcan1 -s 0x700 -d 0x701 --mode normal [--bs N] [--stmin N]
 *   isotp_fault ... --mode nofc     发送端应报告 N_Bs 超时（约 1 秒）
 *   isotp_fault ... --mode ovflw    发送端应立即中止
 *   isotp_fault ... --mode wait     连回 3 个 WAIT 再继续
 *   isotp_fault ... --mode drop --drop-at 2   第 2 个 CF 后故意错序
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/can.h>

#include "isotp_user.h"

enum mode { M_NORMAL, M_NOFC, M_OVFLW, M_WAIT, M_DROP };

static const char *mode_name(enum mode m)
{
	switch (m) {
	case M_NORMAL: return "normal（正常回 FC）";
	case M_NOFC:   return "nofc（不回 FC，制造 N_Bs 超时）";
	case M_OVFLW:  return "ovflw（回溢出，发送端应中止）";
	case M_WAIT:   return "wait（先回 3 次 WAIT 再继续）";
	case M_DROP:   return "drop（故意错序，触发序号校验）";
	}
	return "?";
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void fc_raw(struct isotp_chan *c, uint8_t fs, uint8_t bs, uint8_t stmin)
{
	struct can_frame f;

	memset(&f, 0, sizeof(f));
	f.can_id  = c->tx_id;
	f.can_dlc = 3;
	f.data[0] = (uint8_t)(0x30 | fs);
	f.data[1] = bs;
	f.data[2] = stmin;

	if (write(c->fd, &f, sizeof(f)) != (ssize_t)sizeof(f))
		perror("write FC");

	printf("  -> 回 FC：fs=%u(0=继续 1=等待 2=溢出)  bs=%u  stmin=0x%02X\n",
	       fs, bs, stmin);
}

int main(int argc, char *argv[])
{
	const char *ifname = "vxcan1";
	canid_t rx_id = 0x700, tx_id = 0x701;
	enum mode mode = M_NORMAL;
	int bs = 0, stmin = 0, drop_at = 2;
	int timeout_ms = 10000;

	/*
	 * 行缓冲。这个程序经常被 timeout/pkill 打断，块缓冲会把
	 * 还没刷出的日志全部丢掉，看起来像"什么都没发生"。
	 */
	setvbuf(stdout, NULL, _IOLBF, 0);

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			ifname = argv[++i];
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			rx_id = (canid_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			tx_id = (canid_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			const char *m = argv[++i];

			if (!strcmp(m, "normal"))
				mode = M_NORMAL;
			else if (!strcmp(m, "nofc"))
				mode = M_NOFC;
			else if (!strcmp(m, "ovflw"))
				mode = M_OVFLW;
			else if (!strcmp(m, "wait"))
				mode = M_WAIT;
			else if (!strcmp(m, "drop"))
				mode = M_DROP;
			else {
				fprintf(stderr, "未知模式 %s\n", m);
				return 1;
			}
		} else if (!strcmp(argv[i], "--bs") && i + 1 < argc) {
			bs = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--stmin") && i + 1 < argc) {
			stmin = (int)strtol(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--drop-at") && i + 1 < argc) {
			drop_at = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			timeout_ms = atoi(argv[++i]);
		} else {
			fprintf(stderr,
				"用法: %s [-i 接口] [-s rx_id] [-d tx_id] "
				"[--mode normal|nofc|ovflw|wait|drop] "
				"[--bs N] [--stmin N] [--drop-at N] [-t ms]\n",
				argv[0]);
			return 1;
		}
	}

	struct isotp_chan c;

	if (isotp_open(&c, ifname, tx_id, rx_id) < 0) {
		perror("isotp_open");
		return 1;
	}

	printf("坏接收端就绪  接口 %s  等 0x%03X  回 0x%03X\n",
	       ifname, rx_id, tx_id);
	printf("故障模式：%s\n\n", mode_name(mode));

	uint8_t buf[ISOTP_MAX_MSG];
	size_t need = 0, got = 0;
	int seq_expect = 1;
	int cf_seen = 0;
	int cf_in_block = 0;
	int wait_sent = 0;
	long long t_ff = 0;

	for (;;) {
		struct pollfd p = { .fd = c.fd, .events = POLLIN };
		int r = poll(&p, 1, timeout_ms);

		if (r == 0) {
			printf("[-] 总超时（%d ms 内没有新帧）\n", timeout_ms);
			break;
		}
		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}

		struct can_frame f;
		ssize_t n = read(c.fd, &f, sizeof(f));

		if (n != (ssize_t)sizeof(f))
			continue;
		if ((f.can_id & CAN_SFF_MASK) != rx_id)
			continue;

		unsigned pci = f.data[0] >> 4;

		/* ---------- 首帧 ---------- */
		if (pci == 1) {
			need = ((size_t)(f.data[0] & 0x0F) << 8) | f.data[1];
			got = f.can_dlc - 2 < 6 ? f.can_dlc - 2 : 6;
			memcpy(buf, f.data + 2, got);
			seq_expect = 1;
			cf_seen = 0;
			cf_in_block = 0;
			t_ff = now_ms();

			printf("[FF] 声明长度 %zu，首帧带 %zu 字节\n", need, got);

			if (mode == M_NOFC) {
				printf("     （故意不回 FC，看发送端多久放弃）\n");
				continue;
			}

			if (mode == M_OVFLW) {
				fc_raw(&c, 2, 0, 0);
				printf("     （已告诉对端溢出，它应该中止）\n");
				continue;
			}

			if (mode == M_WAIT) {
				/* 连回 3 个 WAIT，再给 CTS。发送端应耐心等待 */
				for (int i = 0; i < 3; i++) {
					fc_raw(&c, 1, 0, 0);
					wait_sent++;
				}
			}

			fc_raw(&c, 0, (uint8_t)bs, (uint8_t)stmin);
			continue;
		}

		/* ---------- 连续帧 ---------- */
		if (pci == 2) {
			int seq = f.data[0] & 0x0F;

			cf_seen++;

			if (seq != seq_expect) {
				printf("[CF %d] 序号 %d 与期望 %d 不符 —— "
				       "丢帧了，整条消息作废\n",
				       cf_seen, seq, seq_expect);
				break;
			}

			size_t take = need - got;
			size_t dlc = f.can_dlc;

			if (dlc < 1)
				continue;
			if (take > 7)
				take = 7;
			if (take > dlc - 1)
				take = dlc - 1;

			memcpy(buf + got, f.data + 1, take);
			got += take;
			seq_expect = (seq_expect % 15) + 1;

			printf("[CF %d] seq=%d 收到 %zu 字节，累计 %zu/%zu\n",
			       cf_seen, seq, take, got, need);

			/*
			 * BS 分组是双向约定：一个 block 收满，接收端必须
			 * 再回一个 FC，否则发送端会一直等下去。忘了这一句，
			 * 现象是"小于 BS 帧的消息正常，大一点就卡死"。
			 */
			if (bs > 0 && ++cf_in_block >= bs && got < need) {
				cf_in_block = 0;
				printf("     （收满 %d 帧，续发流控）\n", bs);
				fc_raw(&c, 0, (uint8_t)bs, (uint8_t)stmin);
			}

			if (mode == M_DROP && cf_seen == drop_at) {
				/* 故意把期望序号拨快一格，下一帧必然对不上 */
				seq_expect = (seq_expect % 15) + 1;
				printf("     （故意错序，等下一帧触发校验失败）\n");
			}

			if (got >= need) {
				printf("\n[完成] 收齐 %zu 字节，耗时 %lld ms\n",
				       got, now_ms() - t_ff);
				printf("  内容前 8 字节：");

				for (size_t i = 0; i < got && i < 8; i++)
					printf("%02X ", buf[i]);
				printf("\n");
				break;
			}
			continue;
		}

		/* FC 方向不该出现数据，打印一下便于排查 */
		if (pci == 3)
			printf("[FC] 收到流控帧（本端是接收方，忽略）\n");
	}

	if (mode == M_WAIT)
		printf("累计回了 %d 个 WAIT\n", wait_sent);

	isotp_close(&c);
	return 0;
}
