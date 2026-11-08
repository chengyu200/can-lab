/*
 * proto.c —— 协议表的定义与实现（唯一真相源）
 *
 * 这里只有一张表 + 三个通用函数。两个 ECU、离线解码工具都从这张表派生，
 * 所以「发送端和接收端对不上」这种最常见的联调问题在这套结构里不会发生。
 *
 * 编译： make proto.o
 */
#include <stdio.h>
#include <string.h>

#include "can_proto.h"

/* ---------------- 报文与信号定义 ----------------
 *
 * 小端位序（Intel），逐段排布：
 *
 *   0x180 ENGINE_FAST  8 字节 / 10 ms
 *     bit  0-15  rpm       0.25 rpm/bit   -> 0 .. 16383.75 rpm
 *     bit 16-23  throttle  0.4 %/bit      -> 0 .. 102 %
 *     bit 24-31  load      1 %/bit        -> 0 .. 100 %（>100% 表示瞬态超载）
 *     bit 32-39  alive     滚动计数器，每帧 +1，到 255 回绕
 *     bit 40-63  保留（必须为 0，方便以后扩展）
 *
 *   alive 这一字节是本实验台唯一一件「协议级技巧」：
 *     靠到达间隔判断丢帧是不可靠的 —— 帧晚到 20 ms 和丢了一帧，接收端
 *     看到的现象一模一样。放一个滚动计数器，接收端只要比较相邻两帧的
 *     差值就能精确知道丢了几帧（差 3 就是丢了 2 帧）。
 *     这就是 AUTOSAR E2E 保护（Profile 1/2）里 counter 字段的最小版本，
 *     也是为什么真实报文里几乎总能找到一个「每帧递增」的字节。
 *
 *   0x181 ENGINE_SLOW  8 字节 / 100 ms
 *     bit  0-7   coolant   1 degC/bit, offset -40 -> -40 .. 215 degC
 *     bit  8-15  oil_press 10 kPa/bit   -> 0 .. 2550 kPa
 *     bit 16-23  flags     位域
 *     bit 24-63  保留
 *
 *   0x200 DASH_CMD     2 字节 / 事件
 *     bit 0-7    cmd
 *     bit 8-15   arg
 *
 * 「保留位必须为 0」不是洁癖：接收端常常把保留位留作版本号或校验，
 * 如果发送端乱填，将来加信号时老接收端会误判。
 */
static const struct can_signal ef_signals[] = {
	{ "rpm",      0, 16, 0.25,  0.0, "rpm" },
	{ "throttle", 16, 8, 0.4,   0.0, "%"   },
	{ "load",     24, 8, 1.0,   0.0, "%"   },
	{ "alive",    32, 8, 1.0,   0.0, ""    },
};

static const struct can_signal es_signals[] = {
	{ "coolant",   0, 8, 1.0, -40.0, "degC" },
	{ "oil_press", 8, 8, 10.0,  0.0, "kPa"  },
	{ "flags",    16, 8, 1.0,   0.0, ""     },
};

static const struct can_signal dc_signals[] = {
	{ "cmd", 0, 8, 1.0, 0.0, "" },
	{ "arg", 8, 8, 1.0, 0.0, "" },
};

const struct can_msg_def proto_msgs[] = {
	{ CAN_ID_ENGINE_FAST, "ENGINE_FAST", 8, 4, ef_signals },
	{ CAN_ID_ENGINE_SLOW, "ENGINE_SLOW", 8, 3, es_signals },
	{ CAN_ID_DASH_CMD,    "DASH_CMD",    2, 2, dc_signals },
};

const unsigned proto_msg_count = sizeof(proto_msgs) / sizeof(proto_msgs[0]);

/* ---------------- 查表 ---------------- */

const struct can_msg_def *proto_lookup(canid_t id)
{
	if (id & CAN_EFF_FLAG)		/* 本协议只用标准帧 */
		return NULL;
	id &= CAN_SFF_MASK;

	for (unsigned i = 0; i < proto_msg_count; i++)
		if (proto_msgs[i].id == id)
			return &proto_msgs[i];
	return NULL;
}

/* ---------------- 编码 ---------------- */

void proto_encode(const struct can_msg_def *m, const double *phys,
		  uint8_t *data)
{
	memset(data, 0, m->dlc);

	for (unsigned i = 0; i < m->nsignals; i++) {
		const struct can_signal *s = &m->signals[i];

		double raw = (phys[i] - s->offset) / s->factor;
		double max = (s->len >= 32) ? 4294967295.0
					    : (double)((1u << s->len) - 1);

		/* 饱和而不是取模：宁可发一个顶上天的值，也不要悄悄变成小数 */
		if (raw < 0.0)
			raw = 0.0;
		if (raw > max)
			raw = max;

		can_bits_put(data, s->start, s->len, (uint32_t)(raw + 0.5));
	}
}

/* ---------------- 解码 ---------------- */

void proto_decode(const struct can_msg_def *m, const uint8_t *data,
		  double *phys)
{
	for (unsigned i = 0; i < m->nsignals; i++) {
		const struct can_signal *s = &m->signals[i];
		uint32_t raw = can_bits_get(data, s->start, s->len);

		phys[i] = (double)raw * s->factor + s->offset;
	}
}

/* ---------------- 一行文本 ---------------- */

int proto_format(const struct can_frame *f, char *buf, size_t n)
{
	canid_t id = f->can_id & CAN_SFF_MASK;
	const struct can_msg_def *m = proto_lookup(f->can_id);
	int w = 0;
	double phys[CAN_MAX_SIGNALS];

	if (!m)
		return snprintf(buf, n, "0x%03X %-12s [?] dlc=%u 未定义的 ID",
				id, can_id_name(id), f->can_dlc);

	/*
	 * 报文短于定义长度时，缺失的字节按 0 处理。
	 * 直接读 f->data 会在 dlc=0 的帧上读到栈上残留 —— 这类 bug 只在
	 * 「刚好来了一帧畸形报文」时炸，最难复现。
	 */
	uint8_t scratch[8] = { 0 };
	memcpy(scratch, f->data, f->can_dlc < 8 ? f->can_dlc : 8);
	proto_decode(m, scratch, phys);

	w += snprintf(buf + w, n - (size_t)w, "0x%03X %-12s", id, m->name);
	for (unsigned i = 0; i < m->nsignals && (size_t)w < n; i++) {
		const struct can_signal *s = &m->signals[i];

		if (!strcmp(s->name, "flags")) {
			/* 位域单独展开，比打印 0x02 有用得多 */
			w += snprintf(buf + w, n - (size_t)w, " flags=0x%02X[%s%s%s]",
				      (unsigned)phys[i],
				      ((unsigned)phys[i] & ES_FLAG_MIL) ? "MIL " : "",
				      ((unsigned)phys[i] & ES_FLAG_OVERHEAT) ? "OVERHEAT " : "",
				      ((unsigned)phys[i] & ES_FLAG_OIL_LOW) ? "OIL_LOW" : "");
		} else if (!strcmp(s->name, "alive")) {
			/* 计数器按整数打，带两位小数反而看不清连续性 */
			w += snprintf(buf + w, n - (size_t)w, " alive=%3u",
				      (unsigned)phys[i]);
		} else {
			w += snprintf(buf + w, n - (size_t)w, " %s=%.2f%s",
				      s->name, phys[i], s->unit);
		}
	}
	return w;
}

/* ---------------- 打印定义表 ---------------- */

void proto_print_defs(FILE *out)
{
	fprintf(out, "本实验台的报文定义（等价于一个 DBC 文件）\n");
	fprintf(out, "%-18s %-6s %-4s %s\n", "报文", "ID", "DLC", "信号");
	fprintf(out, "--------------------------------------------------------------\n");

	for (unsigned i = 0; i < proto_msg_count; i++) {
		const struct can_msg_def *m = &proto_msgs[i];

		fprintf(out, "%-18s 0x%03X  %u\n", m->name, m->id, m->dlc);
		for (unsigned k = 0; k < m->nsignals; k++) {
			const struct can_signal *s = &m->signals[k];

			fprintf(out, "    %-10s bit %2u|%-2u  @1+  (%g,%g)  %s\n",
				s->name, s->start, s->len, s->factor,
				s->offset, s->unit);
		}
	}
	fprintf(out, "--------------------------------------------------------------\n");
	fprintf(out, "格式与 DBC 的 SG_ 行对应：<名字> bit <起始位>|<位宽> @1+ (<factor>,<offset>) <单位>\n");
	fprintf(out, "@1 = 小端(Intel)，+ = 无符号\n");
}
