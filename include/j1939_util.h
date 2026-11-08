/*
 * j1939_util.h —— J1939 的 29 位 ID 与 64 位 NAME 的解析工具
 *
 * 阶段 7 的核心认知：J1939 的 ID 不是「一个数值」，而是一个结构体。
 *
 *   28      26 25 24 23      16 15       8 7        0
 *   +---------+--+--+----------+-----------+----------+
 *   | Priority|R |DP|    PF    |    PS     |    SA    |
 *   +---------+--+--+----------+-----------+----------+
 *        3bit  1  1     8           8           8
 *
 * 关键在 PF（PDU Format）：
 *
 *   PF < 0xF0  ->  PDU1，点对点。此时 PS 是「目标地址 DA」，
 *                  PGN 里不含 PS（按规范 PS 记 0）。
 *   PF >= 0xF0 ->  PDU2，广播。此时 PS 是「组扩展 GE」，属于 PGN 的一部分。
 *
 * 这条规则解释了两件容易困惑的事：
 *   1. 为什么同一个 PGN 有时是广播、有时是点对点 —— 看 PF 落在哪一段。
 *   2. 为什么 PGN 要单独算，而不能直接截 ID 的低 18 位 —— PDU1 的 PS 是地址，
 *      不同 DA 的两个报文其实属于同一个 PGN。
 *
 * NAME 则是 64 位，按位切成一堆字段，其中「Identity Number」等字段共同
 * 决定地址仲裁时的优先级：**NAME 数值小的赢**。内核里就是
 *   if (ecu->name > prev->name) 丢掉新来的
 * 这一行判断（net/can/j1939/address-claim.c）。
 */
#ifndef J1939_UTIL_H
#define J1939_UTIL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <linux/can.h>
#include <linux/can/j1939.h>

/* ---------------- 29 位 ID 拆解 ---------------- */

struct j1939_id {
	unsigned prio;	/* 0..7，越小越优先 */
	unsigned dp;	/* Data Page，扩展 PGN 用 */
	unsigned pf;	/* PDU Format，决定 PDU1/PDU2 */
	unsigned ps;	/* PDU1 时是 DA，PDU2 时是组扩展 GE */
	unsigned sa;	/* 源地址 */
};

static inline void j1939_id_decode(uint32_t id, struct j1939_id *o)
{
	o->prio = (id >> 26) & 0x7;
	o->dp   = (id >> 24) & 0x1;
	o->pf   = (id >> 16) & 0xFF;
	o->ps   = (id >> 8) & 0xFF;
	o->sa   = id & 0xFF;
}

/* PF < 0xF0 即 PDU1（点对点）。这是整个 J1939 编址的分水岭。 */
static inline int j1939_is_pdu1(unsigned pf)
{
	return pf < 0xF0;
}

/*
 * 从 29 位 ID 反算 PGN。
 * PDU1 时 PS 是目标地址，不属于 PGN，必须置 0；
 * PDU2 时 PS 是组扩展，属于 PGN。写错这一行，会得到一个「看起来对但永远匹配不上」的 PGN。
 */
static inline uint32_t j1939_id_to_pgn(uint32_t id)
{
	struct j1939_id o;

	j1939_id_decode(id, &o);
	return (uint32_t)((o.dp << 16) | (o.pf << 8) |
			  (j1939_is_pdu1(o.pf) ? 0 : o.ps));
}

/* 拼 PGN：pdu1 时传 ps=0 */
static inline uint32_t j1939_pgn(unsigned dp, unsigned pf, unsigned ps)
{
	return (uint32_t)((dp << 16) | (pf << 8) | ps);
}

/* 一组常用 PGN。名字里带 TP 的两个是传输协议自己的容器报文。 */
#define PGN_REQUEST        0x0EA00u	/* 请求，PDU1 */
#define PGN_TP_CM          0x0EC00u	/* 连接管理：RTS/CTS/EOMA/BAM/ABORT */
#define PGN_TP_DT          0x0EB00u	/* 数据传送，序号在首字节 */
#define PGN_ADDRESS_CLAIM  0x0EE00u	/* 地址声明，PDU1，DA 固定 0xFF 广播 */
#define PGN_EEC1           0x0F004u	/* 发动机转速等，PDU2 广播 */

static inline const char *j1939_pgn_name(uint32_t pgn)
{
	switch (pgn) {
	case PGN_REQUEST:       return "Request";
	case PGN_TP_CM:         return "TP.CM";
	case PGN_TP_DT:         return "TP.DT";
	case PGN_ADDRESS_CLAIM: return "AddressClaimed";
	case PGN_EEC1:          return "EEC1";
	case 0x0FECA:           return "DM1";
	case 0x0FEF1:           return "CCVS";
	case 0x0FEEE:           return "IdleOp";
	default:                return "PGN?";
	}
}

/* ---------------- 64 位 NAME 拆解 ---------------- */

struct j1939_name {
	unsigned identity;		/* 0..20  身份号（同一厂家内唯一） */
	unsigned manufacturer;		/* 21..31 厂家代码 */
	unsigned ecu_instance;		/* 32..34 ECU 实例 */
	unsigned function_instance;	/* 35..39 功能实例 */
	unsigned function;		/* 40..47 功能 */
	unsigned vehicle_system;	/* 49..55 车辆系统 */
	unsigned vsi;			/* 56..59 车辆系统实例 */
	unsigned industry_group;	/* 60..62 行业组 */
	unsigned arbitrary_addr;	/* 63     是否支持动态地址 */
};

static inline void j1939_name_decode(uint64_t n, struct j1939_name *o)
{
	o->identity          = (unsigned)(n & 0x1FFFFF);
	o->manufacturer      = (unsigned)((n >> 21) & 0x7FF);
	o->ecu_instance      = (unsigned)((n >> 32) & 0x7);
	o->function_instance = (unsigned)((n >> 35) & 0x1F);
	o->function          = (unsigned)((n >> 40) & 0xFF);
	o->vehicle_system    = (unsigned)((n >> 49) & 0x7F);
	o->vsi               = (unsigned)((n >> 56) & 0xF);
	o->industry_group    = (unsigned)((n >> 60) & 0x7);
	o->arbitrary_addr    = (unsigned)((n >> 63) & 0x1);
}

/*
 * 一个字节接一个字节拼，而不是直接 %016llx。
 * 因为在总线上 NAME 是小端传输的（内核 j1939_skb_to_name 就是 le64_to_cpup），
 * 用 hex 打印时高低位是反的，初学者极易看错 —— 这里刻意打成「总线顺序」，
 * 也就是 candump 里看到的那 8 个字节。
 */
static inline void j1939_name_wire_str(uint64_t name, char *buf, size_t len)
{
	unsigned char b[8];
	size_t off = 0;

	memcpy(b, &name, sizeof(b));
	for (int i = 0; i < 8 && off + 3 < len; i++)
		off += (size_t)snprintf(buf + off, len - off, "%02X ", b[i]);
	if (off && off < len)
		buf[off - 1] = '\0';
}

/* 仲裁比较：数值小的赢。返回值 <0 表示 a 赢，>0 表示 b 赢，0 表示相同 */
static inline int j1939_name_cmp(uint64_t a, uint64_t b)
{
	if (a < b)
		return -1;
	if (a > b)
		return 1;
	return 0;
}

/* 地址的语义分类，打印时经常要用 */
static inline const char *j1939_addr_str(unsigned a)
{
	if (a == J1939_NO_ADDR)
		return "broadcast";
	if (a == J1939_IDLE_ADDR)
		return "null(0xFE)";
	return "unicast";
}

#endif /* J1939_UTIL_H */
