/*
 * isotp_user.h —— 用户态 ISO-TP（ISO 15765-2）实现
 *
 * 为什么要有这个文件：内核已经有 net/can/isotp.c（1737 行），
 * 但它把「分片、流控、超时、序号」全部藏在里层。写一遍用户态版本，
 * 你才知道每一帧到底是谁发的、为什么要这么发。
 *
 * 本实现只支持经典 CAN（8 字节帧），不支持扩展寻址、FD。
 * 这些在真实项目里都有，但会把核心逻辑淹没；阶段 5 的笔记里
 * 会说明怎么扩展。
 *
 * 帧格式（经典 CAN，正常寻址）：
 *
 *   单帧 SF:   [0x0L][data...]                  L = 长度 1..7
 *   首帧 FF:   [0x1H][0xLL][6 字节数据]         长度 = (H<<8)|LL，> 7
 *   连续帧 CF: [0x2N][7 字节数据]               N = 序号 1..15 循环
 *   流控帧 FC: [0x3S][BS][STmin]                S = 0 继续 / 1 等待 / 2 溢出
 *
 * 序号从 1 开始，到 15 之后回到 1 —— 不是回到 0。这个细节错一次就
 * 会得到「大包必挂、小包正常」的诡异现象。
 *
 * 半双工提醒（阶段 9 实测踩到的）：
 * 一个 chan 的收发共用同一个 socket 和同一个 rx_id，所以 isotp_send() 在
 * wait_fc() 里等流控时，会把同一个 fd 上收到的**对端数据帧当作垃圾丢掉**。
 * 单向请求-响应（UDS 诊断）完全没问题，但双向同时发的桥接会丢消息。
 * 桥接的解法是在包装层开两个 socket、用同一个过滤器，靠 CAN_RAW 的多播
 * 各拿一份副本（见 09-bridge/can_bridge_isotp.h），本文件一行不用改。
 */
#ifndef ISOTP_USER_H
#define ISOTP_USER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <linux/can.h>

/* 定时参数，单位毫秒。与内核 isotp.c 的默认值一致 */
#define ISOTP_N_BS_MS		1000	/* 发送端等 FC 的超时 */
#define ISOTP_N_CR_MS		1000	/* 接收端等下一个 CF 的超时 */

/* 单条消息上限。真实 ISO-TP 可到 4095（12 位长度），这里放宽到 4 KB */
#define ISOTP_MAX_MSG		4096

struct isotp_chan {
	int	fd;
	canid_t	tx_id;		/* 本端发出的帧用的 ID */
	canid_t	rx_id;		/* 本端接收的帧的 ID */

	/* ---- 接收状态机 ---- */
	uint8_t	rxbuf[ISOTP_MAX_MSG];
	size_t	rxlen;		/* 已收到多少字节 */
	size_t	rxexpect;	/* FF 声明的总长度 */
	uint8_t	rxseq;		/* 期望的下一个 CF 序号 */
	int	rx_active;	/* 1 = 正在收多帧 */

	/* ---- 本端作为接收方时通告的流控参数 ---- */
	uint8_t	fc_bs;		/* 0 = 一次收完 */
	uint8_t	fc_stmin;	/* 0 = 不限间隔 */

	/* ---- 统计 ---- */
	unsigned long n_tx_frames;
	unsigned long n_rx_frames;
	unsigned long n_timeout;
	unsigned long n_badseq;

	int	verbose;
};

/*
 * 打开一个 ISO-TP 端点。
 *   ifname  : vcan0 / vxcan0 / can0 ...
 *   tx_id   : 本端发送用的 CAN ID
 *   rx_id   : 本端接收的 CAN ID
 * 返回 0 成功，-1 失败（errno 已设置）。
 */
int isotp_open(struct isotp_chan *c, const char *ifname,
	       canid_t tx_id, canid_t rx_id);

void isotp_close(struct isotp_chan *c);

/*
 * 发送一条完整消息。内部完成分片、等 FC、遵守 STmin、按 BS 分组。
 * 阻塞直到发完或失败。返回 0 成功，-1 失败（超时/溢出/IO 错误）。
 */
int isotp_send(struct isotp_chan *c, const void *data, size_t len);

/*
 * 接收一条完整消息。内部完成组包、发 FC、序号校验。
 * timeout_ms 是整个调用的总超时（不是帧间超时）。
 * 返回消息长度，-1 失败/超时。返回 0 表示收到了零长度单帧。
 */
ssize_t isotp_recv(struct isotp_chan *c, void *buf, size_t maxlen,
		   int timeout_ms);

/* 设置本端作为接收方时回给对端的流控参数 */
void isotp_set_fc(struct isotp_chan *c, uint8_t bs, uint8_t stmin);

#endif /* ISOTP_USER_H */
