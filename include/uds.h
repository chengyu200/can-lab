/*
 * uds.h —— UDS（ISO 14229）诊断服务
 *
 * UDS 不是「一个协议」，而是「一套远程过程调用约定」。它规定了：
 *   - 请求和响应长什么样（SID + 参数 + 数据）
 *   - 什么条件下允许调用（会话、安全等级）
 *   - 出错怎么回答（7F + SID + NRC）
 *   - 多长算超时（P2 / P2* / S3）
 *
 * 它本身不关心传输层。跑在 ISO-TP 上是 CAN，跑在 DoIP 上是以太网。
 * 所以我们把这一层写成纯逻辑：进一个字节数组，出一个字节数组，
 * 不碰 socket。这一点在 ecu_server.c 里体现得很明显。
 */
#ifndef UDS_H
#define UDS_H

#include <stddef.h>
#include <stdint.h>

/* ---------- 服务标识 ---------- */
#define UDS_SID_SESSION		0x10	/* DiagnosticSessionControl */
#define UDS_SID_RESET		0x11	/* ECUReset */
#define UDS_SID_READ_DTC	0x19	/* ReadDTCInformation */
#define UDS_SID_READ_DID	0x22	/* ReadDataByIdentifier */
#define UDS_SID_SECURITY	0x27	/* SecurityAccess */
#define UDS_SID_WRITE_DID	0x2E	/* WriteDataByIdentifier */
#define UDS_SID_TESTER		0x3E	/* TesterPresent */

/* ---------- 负响应码（NRC） ---------- */
#define UDS_NRC_SERVICE_UNSUPPORTED	0x11
#define UDS_NRC_SUBFUNC_UNSUPPORTED	0x12
#define UDS_NRC_INCORRECT_LENGTH	0x13
#define UDS_NRC_CONDITIONS		0x22
#define UDS_NRC_SEQUENCE		0x24	/* 顺序错，比如没请求种子就发 key */
#define UDS_NRC_OUT_OF_RANGE		0x31
#define UDS_NRC_SECURITY_DENIED		0x33
#define UDS_NRC_INVALID_KEY		0x35
#define UDS_NRC_EXCEED_ATTEMPTS		0x36
#define UDS_NRC_PENDING			0x78	/* 我在处理，别超时 */

/* ---------- 会话 ---------- */
#define UDS_SESSION_DEFAULT	0x01
#define UDS_SESSION_PROGRAM	0x02
#define UDS_SESSION_EXTENDED	0x03

/*
 * 时间参数，单位毫秒。
 *   P2     : 服务器收到请求后必须在这个时间内响应，否则客户端认为超时
 *   P2*    : 服务器回了 NRC 0x78 之后，把等待时间延长到这个值
 *   S3     : 非默认会话下，客户端必须至少每 S3 发一次 3E，否则服务器
 *            自动退回默认会话
 * 50ms 是 ISO 14229 给的默认值，实际项目里 P2 常常放宽到 100ms。
 */
#define UDS_P2_MS		50
#define UDS_P2STAR_MS		5000
#define UDS_S3_MS		5000

/* ---------- DID ---------- */
#define UDS_DID_VIN		0xF190
#define UDS_DID_PART_NUMBER	0xF187
#define UDS_DID_SERIAL		0xF18C
#define UDS_DID_HW_VERSION	0xF191
#define UDS_DID_LIVE_RPM	0x0100	/* 自定义：实时转速 */

/* ---------- 安全访问的密钥算法 ----------
 *
 * 真实项目里这是保密算法（每个车型不同），常见做法是查表或
 * 用种子做变换。这里用一个能一眼看懂的异或，方便你算 key。
 */
static inline uint32_t uds_key_from_seed(uint32_t seed)
{
	return seed ^ 0xA5A5A5A5u;
}

struct uds_dtc {
	uint8_t	code[3];
	uint8_t	status;
	const char *desc;
};

struct uds_ctx {
	int	session;
	int	security_unlocked;
	int	seed_pending;
	uint32_t seed;
	int	security_attempts;

	uint8_t	vin[17];
	char	part[16];

	uint32_t rpm;			/* 给 0x22 0x0100 用的活数据 */

	long long last_activity_ms;

	const struct uds_dtc *dtcs;
	int	n_dtcs;

	unsigned long n_req;
	unsigned long n_nrc;
};

void uds_init(struct uds_ctx *c);

/*
 * 处理一条诊断请求。返回响应字节数；返回 0 表示「不响应」
 * （用于 3E 带 suppressPosRspMsgIndicationBit 的情况）。
 * resp 缓冲区至少要 256 字节。
 */
int uds_handle(struct uds_ctx *c, const uint8_t *req, size_t n,
	       uint8_t *resp, size_t maxresp, long long now_ms);

/* 检查 S3 超时。返回 1 表示刚刚退回默认会话（可以打日志）。 */
int uds_check_s3(struct uds_ctx *c, long long now_ms);

const char *uds_nrc_name(uint8_t nrc);
const char *uds_sid_name(uint8_t sid);
const char *uds_session_name(int s);

#endif /* UDS_H */
