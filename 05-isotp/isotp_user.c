/*
 * isotp_user.c —— 用户态 ISO-TP 实现
 *
 * 设计取舍：
 *
 * 1) 阻塞式，单线程。真实 ECU 里 ISO-TP 往往是中断驱动的状态机，
 *    但那是为了不阻塞主循环。这里的目的是看清协议，阻塞式最直观。
 *
 * 2) 一个 chan 同时承担收发。ISO-TP 的每个方向各有自己的 CAN ID，
 *    tx_id 发、rx_id 收，两个方向互不干扰。
 *
 * 3) 不做重传。ISO-TP 本身规定的是「超时就报错、由上层重试」，
 *    UDS 的 P2 超时机制才是业务层的重试策略。内核版要多一步
 *    CAN_ISOTP_WAIT_TX_DONE 才能拿到准确结果，这里不涉及。
 */
#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can/raw.h>

#include "isotp_user.h"

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 发一帧原始 CAN 帧。dlc 必须一次写完整，内核不收半帧。 */
static int send_frame(struct isotp_chan *c, const uint8_t *d, int dlc)
{
	struct can_frame f;

	memset(&f, 0, sizeof(f));
	f.can_id  = c->tx_id;
	f.can_dlc = (uint8_t)dlc;
	memcpy(f.data, d, (size_t)dlc);

	if (write(c->fd, &f, sizeof(f)) != (ssize_t)sizeof(f))
		return -1;

	c->n_tx_frames++;

	if (c->verbose) {
		printf("  TX %03X [%d] ", c->tx_id, dlc);
		for (int i = 0; i < dlc; i++)
			printf("%02X ", d[i]);
		printf("\n");
	}
	return 0;
}

/*
 * STmin 编码（ISO 15765-2 表 5）：
 *   0x00 - 0x7F : 毫秒
 *   0xF1 - 0xF9 : 100 - 900 微秒
 *   其它        : 保留，实现应按摩最大等待处理
 *
 * 注意 0xF1 是 100us 而不是 1ms —— 把 0xF1 当 1 处理是常见 bug，
 * 会让发送端慢 10 倍。
 */
static int stmin_us(uint8_t v)
{
	if (v <= 0x7F)
		return v * 1000;
	if (v >= 0xF1 && v <= 0xF9)
		return (v - 0xF0) * 100;
	return 127000;
}

/* 回一帧流控。fs: 0=继续 1=等待 2=溢出 */
static void send_fc(struct isotp_chan *c, uint8_t fs)
{
	uint8_t b[3];

	b[0] = (uint8_t)(0x30 | fs);
	b[1] = c->fc_bs;
	b[2] = c->fc_stmin;
	send_frame(c, b, 3);
}

/*
 * 等流控帧。返回 0 成功，-1 超时/IO 错误，-2 对端溢出。
 * 期间收到的非 FC 帧直接丢弃 —— 真实的 ISO-TP 会话是串行的，
 * 同一时刻只会有一个方向在传输，所以这里可以粗暴丢弃。
 */
static int wait_fc(struct isotp_chan *c, int timeout_ms,
		   uint8_t *bs, uint8_t *stmin)
{
	long long deadline = now_ms() + timeout_ms;
	struct can_frame f;

	for (;;) {
		long long remain = deadline - now_ms();

		if (remain <= 0) {
			c->n_timeout++;
			errno = ETIMEDOUT;
			return -1;
		}

		struct pollfd p = { .fd = c->fd, .events = POLLIN };
		int r = poll(&p, 1, (int)remain);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0) {
			c->n_timeout++;
			errno = ETIMEDOUT;
			return -1;
		}

		ssize_t n = read(c->fd, &f, sizeof(f));

		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (n != (ssize_t)sizeof(f))
			continue;			/* FD 帧，跳过 */
		if ((f.can_id & CAN_SFF_MASK) != c->rx_id)
			continue;
		if (f.can_dlc < 3)
			continue;
		if ((f.data[0] >> 4) != 3)
			continue;			/* 不是 FC */

		c->n_rx_frames++;

		uint8_t fs = f.data[0] & 0x0F;

		if (c->verbose)
			printf("  RX %03X [%d] FC fs=%u bs=%u stmin=%u\n",
			       c->rx_id, f.can_dlc, fs, f.data[1], f.data[2]);

		if (fs == 1)			/* WAIT：继续等，不消耗剩余时间 */
			continue;
		if (fs == 2) {			/* OVFLW：对端缓冲区不够 */
			errno = ENOBUFS;
			return -2;
		}

		*bs    = f.data[1];
		*stmin = f.data[2];
		return 0;
	}
}

int isotp_open(struct isotp_chan *c, const char *ifname,
	       canid_t tx_id, canid_t rx_id)
{
	memset(c, 0, sizeof(*c));

	if (tx_id == rx_id) {
		fprintf(stderr, "[-] tx_id 与 rx_id 不能相同，否则会收到自己的帧\n");
		errno = EINVAL;
		return -1;
	}

	c->fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	if (c->fd < 0)
		return -1;

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = (int)if_nametoindex(ifname);

	if (!addr.can_ifindex) {
		fprintf(stderr, "[-] 接口 %s 不存在\n", ifname);
		close(c->fd);
		errno = ENODEV;
		return -1;
	}

	if (bind(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(c->fd);
		return -1;
	}

	/*
	 * 只订阅 rx_id。这里不要动 CAN_RAW_LOOPBACK：
	 * 本端发出的 tx_id 帧会被这个过滤器挡掉，所以不需要关回环。
	 * 而在 vcan 上关回环会直接丢帧（阶段 0 实测过），保持默认最安全。
	 */
	struct can_filter flt = {
		.can_id   = rx_id,
		.can_mask = CAN_SFF_MASK,
	};

	if (setsockopt(c->fd, SOL_CAN_RAW, CAN_RAW_FILTER,
		       &flt, sizeof(flt)) < 0) {
		close(c->fd);
		return -1;
	}

	c->tx_id = tx_id;
	c->rx_id = rx_id;
	return 0;
}

void isotp_close(struct isotp_chan *c)
{
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

void isotp_set_fc(struct isotp_chan *c, uint8_t bs, uint8_t stmin)
{
	c->fc_bs    = bs;
	c->fc_stmin = stmin;
}

int isotp_send(struct isotp_chan *c, const void *data, size_t len)
{
	const uint8_t *p = data;
	uint8_t b[8];

	if (len == 0 || len > ISOTP_MAX_MSG) {
		errno = EINVAL;
		return -1;
	}

	/* ---------- 单帧：<= 7 字节 ---------- */
	if (len <= 7) {
		memset(b, 0, sizeof(b));
		b[0] = (uint8_t)len;
		memcpy(b + 1, p, len);
		return send_frame(c, b, (int)len + 1);
	}

	/* ---------- 首帧：低 4 位放长度高 4 位，下一字节放低 8 位 ---------- */
	memset(b, 0, sizeof(b));
	b[0] = (uint8_t)(0x10 | ((len >> 8) & 0x0F));
	b[1] = (uint8_t)(len & 0xFF);
	memcpy(b + 2, p, 6);

	if (send_frame(c, b, 8) < 0)
		return -1;

	size_t sent = 6;
	uint8_t bs = 0, stmin = 0;

	/*
	 * 首帧发完必须停下等 FC。这是 ISO-TP 与「闷头发完」的关键差别：
	 * 接收端可能缓冲区很小，或者希望限速，流控帧就是它表达意愿的方式。
	 */
	int rc = wait_fc(c, ISOTP_N_BS_MS, &bs, &stmin);

	if (rc < 0)
		return -1;

	int gap_us = stmin_us(stmin);
	uint8_t seq = 1;			/* 序号从 1 开始，不是 0 */
	unsigned block = 0;

	while (sent < len) {
		/* 每个 block 发完，停下来等下一次流控 */
		if (bs && block == bs) {
			rc = wait_fc(c, ISOTP_N_BS_MS, &bs, &stmin);
			if (rc < 0)
				return -1;
			gap_us = stmin_us(stmin);
			block = 0;
		}

		size_t n = len - sent;

		if (n > 7)
			n = 7;

		memset(b, 0, sizeof(b));
		b[0] = (uint8_t)(0x20 | seq);
		memcpy(b + 1, p + sent, n);

		if (send_frame(c, b, 8) < 0)
			return -1;

		sent += n;
		block++;
		seq = (uint8_t)((seq % 15) + 1);	/* 15 之后回到 1 */

		/*
		 * STmin 是「两个连续帧之间的最小间隔」。发送端只能更慢，
		 * 不能更快。这里用 usleep 近似，精度受调度器影响 ——
		 * 这正是阶段 4 讲过的用户态抖动问题。
		 */
		if (gap_us > 0 && sent < len)
			usleep((useconds_t)gap_us);
	}

	return 0;
}

ssize_t isotp_recv(struct isotp_chan *c, void *buf, size_t maxlen,
		   int timeout_ms)
{
	long long deadline = now_ms() + timeout_ms;
	struct can_frame f;

	c->rx_active = 0;
	c->rxlen = 0;

	for (;;) {
		long long remain = deadline - now_ms();

		if (remain <= 0) {
			c->n_timeout++;
			errno = ETIMEDOUT;
			return -1;
		}

		/*
		 * 收多帧时，两个 CF 之间还有一层 N_Cr 超时。
		 * 总超时可能很长（比如 5 秒），但帧间超时只有 1 秒：
		 * 对端发到一半死掉，不该让我们干等到总超时。
		 */
		if (c->rx_active && remain > ISOTP_N_CR_MS)
			remain = ISOTP_N_CR_MS;

		struct pollfd p = { .fd = c->fd, .events = POLLIN };
		int r = poll(&p, 1, (int)remain);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0) {
			c->n_timeout++;
			errno = ETIMEDOUT;
			return -1;
		}

		ssize_t n = read(c->fd, &f, sizeof(f));

		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -1;
		}
		if (n != (ssize_t)sizeof(f))
			continue;
		if ((f.can_id & CAN_SFF_MASK) != c->rx_id)
			continue;

		size_t dlc = f.can_dlc;

		if (dlc > 8)
			dlc = 8;
		if (dlc < 1)
			continue;

		c->n_rx_frames++;

		unsigned pci = f.data[0] >> 4;

		if (c->verbose) {
			printf("  RX %03X [%zu] ", c->rx_id, dlc);
			for (size_t i = 0; i < dlc; i++)
				printf("%02X ", f.data[i]);
			printf("\n");
		}

		/* ---------- 单帧 ---------- */
		if (pci == 0) {
			size_t len = f.data[0] & 0x0F;

			if (len > dlc - 1)
				len = dlc - 1;	/* 容错：长度超出实际字节 */
			if (len > maxlen)
				len = maxlen;

			memcpy(buf, f.data + 1, len);
			return (ssize_t)len;
		}

		/* ---------- 首帧 ---------- */
		if (pci == 1) {
			size_t total = ((size_t)(f.data[0] & 0x0F) << 8) |
				       f.data[1];

			if (total <= 7 || total > ISOTP_MAX_MSG) {
				/* 长度不合法，告诉对端溢出，别让它白发 */
				send_fc(c, 2);
				errno = EMSGSIZE;
				return -1;
			}

			size_t n6 = total < 6 ? total : 6;

			memcpy(c->rxbuf, f.data + 2, n6);
			c->rxlen    = n6;
			c->rxexpect = total;
			c->rxseq    = 1;
			c->rx_active = 1;

			send_fc(c, 0);		/* 请求继续 */

			if (c->rxlen >= c->rxexpect)
				goto done;

			continue;
		}

		/* ---------- 连续帧 ---------- */
		if (pci == 2) {
			if (!c->rx_active)
				continue;	/* 孤立的 CF，丢弃 */

			uint8_t seq = f.data[0] & 0x0F;

			if (seq != c->rxseq) {
				/*
				 * 序号不符说明中间丢帧了，已收到的数据不可信。
				 * 只能整条丢弃 —— 这就是为什么上层（UDS）
				 * 还需要自己的超时和重试。
				 */
				c->n_badseq++;
				c->rx_active = 0;
				c->rxlen = 0;
				errno = EPROTO;
				return -1;
			}

			size_t want = c->rxexpect - c->rxlen;
			size_t n7 = want < 7 ? want : 7;

			if (n7 > dlc - 1)
				n7 = dlc - 1;

			memcpy(c->rxbuf + c->rxlen, f.data + 1, n7);
			c->rxlen += n7;
			c->rxseq = (uint8_t)((c->rxseq % 15) + 1);

			if (c->rxlen >= c->rxexpect)
				goto done;

			continue;
		}

		/* pci == 3：流控帧，接收方向忽略 */
	}

done:
	{
		size_t len = c->rxlen;

		if (len > maxlen)
			len = maxlen;

		memcpy(buf, c->rxbuf, len);
		c->rx_active = 0;
		return (ssize_t)len;
	}
}
