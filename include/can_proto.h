/*
 * can_proto.h —— 本实验台的「总线协议定义」，相当于一个手写的 DBC 文件
 *
 * 阶段 2 这里只有 ID 表；阶段 3 补上信号定义与打包/解包。
 *
 * 设计约定：
 *   - 全部用 11-bit 标准帧（SFF），按用途分三段
 *       0x180 ~ 0x1FF  周期实时信号（优先级最高，ID 越小越优先）
 *       0x200 ~ 0x2FF  事件 / 命令
 *       0x300 ~ 0x3FF  诊断
 *     这套「ID 分段 = 优先级分层」是车载网络里的通用做法：
 *     标准帧仲裁是按 ID 数值逐位比较的，数值小的天然赢得仲裁，
 *     所以越是不能延迟的信号，ID 要定得越小。
 *
 *   - 信号一律按**小端（Intel）**位序排布，这与 DBC 里 `@1` 的语义一致，
 *     也是 ARM/x86 上的自然顺序。打包规则见下面 can_bits_put() 的注释。
 *
 * 为什么要把「信号定义」做成数据结构，而不是写死在一堆移位语句里？
 *   因为同一份布局要被三种东西共用：发送端编码、接收端解码、离线分析工具。
 *   写死三遍必然漂移。做成表之后，加一个信号只改一处，
 *   而且这张表本身就能打印出来给人看 —— 这就是 DBC 文件存在的原因。
 */
#ifndef CAN_PROTO_H
#define CAN_PROTO_H

#include <stdio.h>
#include <stdint.h>
#include <linux/can.h>

/* ---------------- ID 分配表 ---------------- */
enum {
	CAN_ID_ENGINE_FAST = 0x180,	/* engine -> bus, 10 ms  */
	CAN_ID_ENGINE_SLOW = 0x181,	/* engine -> bus, 100 ms */
	CAN_ID_DASH_CMD    = 0x200,	/* dash   -> bus, 事件   */
	CAN_ID_DIAG_REQ    = 0x300,	/* tester -> ecu         */
	CAN_ID_DIAG_RESP   = 0x308,	/* ecu    -> tester      */
};

/* ID -> 可读名字。注意先剥掉 EFF/RTR/ERR 这些标志位再看数值。 */
static inline const char *can_id_name(canid_t id)
{
	switch (id & CAN_SFF_MASK) {
	case CAN_ID_ENGINE_FAST: return "ENGINE_FAST";
	case CAN_ID_ENGINE_SLOW: return "ENGINE_SLOW";
	case CAN_ID_DASH_CMD:    return "DASH_CMD";
	case CAN_ID_DIAG_REQ:    return "DIAG_REQ";
	case CAN_ID_DIAG_RESP:   return "DIAG_RESP";
	default:                 return "UNKNOWN";
	}
}

/* ---------------- 每个报文的信号下标 ----------------
 *
 * 编码/解码统一用「物理值数组」，下标就是这里的枚举。
 * 这样调用方写 phys[EF_RPM] 而不是 phys[0]，加信号时不用改调用点。
 */
enum engine_fast_sig { EF_RPM, EF_THROTTLE, EF_LOAD, EF_ALIVE, EF_NSIG };
enum engine_slow_sig { ES_COOLANT, ES_OIL_PRESS, ES_FLAGS, ES_NSIG };
enum dash_cmd_sig    { DC_CMD, DC_ARG, DC_NSIG };

/* 0x181 第三字节是位域，一个字节里塞多个布尔量，这是 CAN 的常规操作 */
#define ES_FLAG_MIL       0x01	/* 故障灯（Malfunction Indicator Lamp） */
#define ES_FLAG_OVERHEAT  0x02	/* 过温 */
#define ES_FLAG_OIL_LOW   0x04	/* 油压过低 */

/* 0x200 命令字 */
#define CMD_DERATE_REQ    0x01	/* 请求降扭，arg = 降载百分比 */
#define CMD_DERATE_CLR    0x02	/* 解除降扭 */

/* ---------------- 信号 / 报文描述 ----------------
 *
 * start/len/factor/offset 的语义与 DBC 完全一致：
 *
 *   物理值 = 原始值 * factor + offset
 *   原始值 = (物理值 - offset) / factor
 *
 * 例：rpm 的 factor=0.25，物理 1000 rpm -> 原始 4000 -> 0x0FA0。
 * 用「因子」而不是直接传原始值，是为了让上层代码只跟物理量打交道：
 * 换一款分辨率更高的发动机控制器时，只改这张表，上层不用动。
 */
struct can_signal {
	const char *name;
	uint8_t     start;	/* 起始位（小端位序，bit0 = data[0] 的 bit0） */
	uint8_t     len;	/* 位宽 1..32 */
	double      factor;
	double      offset;
	const char *unit;
};

struct can_msg_def {
	canid_t     id;
	const char *name;
	uint8_t     dlc;	/* 该报文固定长度；0x200 只用了 2 字节 */
	uint8_t     nsignals;
	const struct can_signal *signals;
};

#define CAN_MAX_SIGNALS 8

extern const struct can_msg_def proto_msgs[];
extern const unsigned proto_msg_count;

/* ---------------- 小端位域读写 ----------------
 *
 * CAN 报文是 8 个字节的裸缓冲区，信号可以跨字节、可以不对齐。
 * 这里用「位偏移」寻址：bit i 落在 data[i/8] 的第 (i%8) 位（LSB 为第 0 位）。
 *
 *   data[]:   B0        B1
 *           76543210  76543210
 *   bit:     01234567  89...
 *
 * 于是 rpm（start=0, len=16）就是 B0 的低 8 位 + B1 的 8 位，小端拼起来。
 * 这正是 DBC 里 `BO_ 384 ... SG_ rpm : 0|16@1+ (0.25,0)` 的含义。
 *
 * 一段一段循环写位看起来很笨，但它对任意 start/len 都成立、
 * 不依赖字节序和编译器位域布局，而且能直接对照 DBC 检查 ——
 * 生产代码里同样是这么生成的（cantools 生成的 C 代码也是移位拼装）。
 */
static inline void can_bits_put(uint8_t *data, unsigned start, unsigned len,
				uint32_t raw)
{
	for (unsigned i = 0; i < len; i++) {
		unsigned bit = start + i;
		uint8_t mask = (uint8_t)(1u << (bit & 7));

		if ((raw >> i) & 1u)
			data[bit >> 3] |= mask;
		else
			data[bit >> 3] &= (uint8_t)~mask;
	}
}

static inline uint32_t can_bits_get(const uint8_t *data, unsigned start,
				    unsigned len)
{
	uint32_t v = 0;

	for (unsigned i = 0; i < len; i++) {
		unsigned bit = start + i;

		if (data[bit >> 3] & (1u << (bit & 7)))
			v |= 1u << i;
	}
	return v;
}

/* ---------------- 编解码接口 ---------------- */

/* 按 ID 查表，找不到返回 NULL */
const struct can_msg_def *proto_lookup(canid_t id);

/*
 * 物理值 -> 报文。data 会被整段清零（保证未用位为 0），
 * 超出位宽的值做饱和处理而不是回绕 —— 回绕会让 7000 rpm 变成 4464 rpm，
 * 这种静默错误在车上极难查。
 */
void proto_encode(const struct can_msg_def *m, const double *phys,
		  uint8_t *data);

/* 报文 -> 物理值 */
void proto_decode(const struct can_msg_def *m, const uint8_t *data,
		  double *phys);

/* 一行文本：0x180 ENGINE_FAST  rpm=1200.00rpm throttle=30.00% load=45.00% */
int proto_format(const struct can_frame *f, char *buf, size_t n);

/* 打印完整信号定义（看 DBC 长什么样） */
void proto_print_defs(FILE *out);

#endif /* CAN_PROTO_H */
