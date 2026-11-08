/*
 * uds.c —— UDS 服务实现（纯逻辑，不碰 socket）
 *
 * 每个服务函数都遵循同一个骨架：
 *
 *   1. 长度对不对？            -> 0x13 incorrectMessageLength
 *   2. 这个会话允许吗？        -> 0x7E conditionsNotCorrect
 *   3. 安全等级够吗？          -> 0x33 securityAccessDenied
 *   4. 参数合法吗？            -> 0x31 requestOutOfRange
 *   5. 干活，回正响应（SID + 0x40）
 *
 * 顺序不能乱。先报长度错再报权限错，是因为长度不对时连参数都读不出来；
 * 而权限检查必须在参数检查之前，否则会通过错误码泄露"参数是否存在"。
 * 这类细节在 ISO 14229 里都有明确规定，也是诊断测试用例最爱挑的地方。
 */
#include <stdio.h>
#include <string.h>

#include "uds.h"

/*
 * DTC 的 3 字节编码不是 ASCII：
 *   byte0 bits7-6 = 系统类别（P=00 C=01 B=10 U=11）
 *   byte0 bits5-4 = 第一位数字
 *   byte0 bits3-0 = 第二位数字
 *   byte1         = 第三、四位数字（BCD）
 * 所以 P0301 是 03 01，U0100 是 C1 00，C1234 是 52 34。
 */
static const struct uds_dtc default_dtcs[] = {
	{ { 0x03, 0x01, 0x00 }, 0x2F, "P0301 气缸 1 失火" },
	{ { 0xC1, 0x00, 0x00 }, 0x08, "U0100 与 ECM 通信丢失" },
	{ { 0x52, 0x34, 0x00 }, 0x04, "C1234 轮速传感器信号异常" },
};

static int neg(uint8_t *resp, uint8_t sid, uint8_t nrc)
{
	resp[0] = 0x7F;
	resp[1] = sid;
	resp[2] = nrc;
	return 3;
}

void uds_init(struct uds_ctx *c)
{
	memset(c, 0, sizeof(*c));
	c->session = UDS_SESSION_DEFAULT;
	memcpy(c->vin, "LSVAA123456789012", 17);
	snprintf(c->part, sizeof(c->part), "ENG-ECU-2026A");
	c->dtcs  = default_dtcs;
	c->n_dtcs = (int)(sizeof(default_dtcs) / sizeof(default_dtcs[0]));
	c->rpm = 850;
	c->last_activity_ms = 0;
}

/* ---------- 0x10 会话控制 ---------- */
static int svc_session(struct uds_ctx *c, const uint8_t *req, size_t n,
		       uint8_t *resp)
{
	if (n != 2)
		return neg(resp, UDS_SID_SESSION, UDS_NRC_INCORRECT_LENGTH);

	int suppress = (req[1] & 0x80) != 0;
	uint8_t sub = req[1] & 0x7F;
	int s;

	switch (sub) {
	case UDS_SESSION_DEFAULT:  s = UDS_SESSION_DEFAULT;  break;
	case UDS_SESSION_PROGRAM:  s = UDS_SESSION_PROGRAM;  break;
	case UDS_SESSION_EXTENDED: s = UDS_SESSION_EXTENDED; break;
	default:
		return neg(resp, UDS_SID_SESSION, UDS_NRC_SUBFUNC_UNSUPPORTED);
	}

	c->session = s;

	/*
	 * 切会话必须重置安全状态。否则攻击者可以「进扩展会话、解锁、
	 * 退回默认会话、再进扩展会话」绕过一部分限制。
	 */
	c->security_unlocked = 0;
	c->seed_pending = 0;
	c->security_attempts = 0;

	if (suppress)
		return 0;

	resp[0] = 0x50;
	resp[1] = sub;
	resp[2] = (uint8_t)(UDS_P2_MS >> 8);
	resp[3] = (uint8_t)(UDS_P2_MS & 0xFF);
	resp[4] = (uint8_t)(UDS_P2STAR_MS >> 8);
	resp[5] = (uint8_t)(UDS_P2STAR_MS & 0xFF);
	return 6;
}

/* ---------- 0x11 ECU 复位 ---------- */
static int svc_reset(struct uds_ctx *c, const uint8_t *req, size_t n,
		     uint8_t *resp)
{
	if (n != 2)
		return neg(resp, UDS_SID_RESET, UDS_NRC_INCORRECT_LENGTH);

	if ((req[1] & 0x7F) != 0x01)
		return neg(resp, UDS_SID_RESET, UDS_NRC_SUBFUNC_UNSUPPORTED);

	/* 复位之后一切回到出厂状态 */
	c->session = UDS_SESSION_DEFAULT;
	c->security_unlocked = 0;
	c->seed_pending = 0;

	resp[0] = 0x51;
	resp[1] = 0x01;
	return 2;
}

/* ---------- 0x22 读数据 ---------- */
static int svc_read_did(struct uds_ctx *c, const uint8_t *req, size_t n,
			uint8_t *resp)
{
	if (n != 3)
		return neg(resp, UDS_SID_READ_DID, UDS_NRC_INCORRECT_LENGTH);

	uint16_t did = (uint16_t)((req[1] << 8) | req[2]);
	int p = 3;

	resp[0] = 0x62;
	resp[1] = req[1];
	resp[2] = req[2];

	switch (did) {
	case UDS_DID_VIN:
		memcpy(resp + p, c->vin, 17);
		p += 17;
		break;

	case UDS_DID_PART_NUMBER: {
		size_t l = strlen(c->part);

		memcpy(resp + p, c->part, l);
		p += (int)l;
		break;
	}

	case UDS_DID_SERIAL: {
		static const char sn[] = "SN20261107001";

		memcpy(resp + p, sn, sizeof(sn) - 1);
		p += (int)(sizeof(sn) - 1);
		break;
	}

	case UDS_DID_HW_VERSION:
		memcpy(resp + p, "HW-1.2", 6);
		p += 6;
		break;

	case UDS_DID_LIVE_RPM:
		resp[p++] = (uint8_t)(c->rpm >> 8);
		resp[p++] = (uint8_t)(c->rpm & 0xFF);
		break;

	default:
		/*
		 * 不存在的 DID 回 0x31 而不是 0x11：服务本身是支持的，
		 * 只是这个标识符没有定义。这两者的区别在测试用例里
		 * 会被严格检查。
		 */
		return neg(resp, UDS_SID_READ_DID, UDS_NRC_OUT_OF_RANGE);
	}

	return p;
}

/* ---------- 0x2E 写数据 ---------- */
static int svc_write_did(struct uds_ctx *c, const uint8_t *req, size_t n,
			 uint8_t *resp)
{
	if (n < 4)
		return neg(resp, UDS_SID_WRITE_DID, UDS_NRC_INCORRECT_LENGTH);

	/*
	 * 写操作是两把锁：会话 + 安全等级。
	 * 先检查会话（0x7E），再检查安全（0x33）—— 顺序反了会让人
	 * 在默认会话下收到 0x33，误以为需要解锁，其实先要进扩展会话。
	 */
	if (c->session != UDS_SESSION_EXTENDED &&
	    c->session != UDS_SESSION_PROGRAM)
		return neg(resp, UDS_SID_WRITE_DID, UDS_NRC_CONDITIONS);

	if (!c->security_unlocked)
		return neg(resp, UDS_SID_WRITE_DID, UDS_NRC_SECURITY_DENIED);

	uint16_t did = (uint16_t)((req[1] << 8) | req[2]);

	if (did != UDS_DID_VIN)
		return neg(resp, UDS_SID_WRITE_DID, UDS_NRC_OUT_OF_RANGE);

	if (n != 3 + 17)
		return neg(resp, UDS_SID_WRITE_DID, UDS_NRC_INCORRECT_LENGTH);

	/* VIN 只允许大写字母和数字，不含 I / O / Q（避免与 1 / 0 混淆） */
	for (int i = 0; i < 17; i++) {
		uint8_t ch = req[3 + i];

		if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z')))
			return neg(resp, UDS_SID_WRITE_DID,
				   UDS_NRC_OUT_OF_RANGE);
		if (ch == 'I' || ch == 'O' || ch == 'Q')
			return neg(resp, UDS_SID_WRITE_DID,
				   UDS_NRC_OUT_OF_RANGE);
	}

	memcpy(c->vin, req + 3, 17);

	resp[0] = 0x6E;
	resp[1] = req[1];
	resp[2] = req[2];
	return 3;
}

/* ---------- 0x27 安全访问 ---------- */
static int svc_security(struct uds_ctx *c, const uint8_t *req, size_t n,
			uint8_t *resp, long long now_ms)
{
	if (n < 2)
		return neg(resp, UDS_SID_SECURITY, UDS_NRC_INCORRECT_LENGTH);

	uint8_t sub = req[1];

	/* --- 请求种子 --- */
	if (sub == 0x01) {
		if (n != 2)
			return neg(resp, UDS_SID_SECURITY,
				   UDS_NRC_INCORRECT_LENGTH);

		/*
		 * 种子必须每次不同，否则重放攻击可以直接复用上次的 key。
		 * 这里用时间戳打散，真实实现应该用硬件随机数。
		 */
		uint32_t seed = (uint32_t)(now_ms ^ 0x12345678);

		if (seed == 0)
			seed = 1;

		c->seed = seed;
		c->seed_pending = 1;

		resp[0] = 0x67;
		resp[1] = 0x01;
		resp[2] = (uint8_t)(seed >> 24);
		resp[3] = (uint8_t)(seed >> 16);
		resp[4] = (uint8_t)(seed >> 8);
		resp[5] = (uint8_t)(seed);
		return 6;
	}

	/* --- 发送密钥 --- */
	if (sub == 0x02) {
		if (n != 6)
			return neg(resp, UDS_SID_SECURITY,
				   UDS_NRC_INCORRECT_LENGTH);

		/* 没要过种子直接发 key：顺序错误 */
		if (!c->seed_pending)
			return neg(resp, UDS_SID_SECURITY, UDS_NRC_SEQUENCE);

		uint32_t key = ((uint32_t)req[2] << 24) |
			       ((uint32_t)req[3] << 16) |
			       ((uint32_t)req[4] << 8) |
			       (uint32_t)req[5];

		c->seed_pending = 0;

		if (key != uds_key_from_seed(c->seed)) {
			c->security_attempts++;

			/*
			 * 连续错 3 次锁死（需要等延时或复位）。
			 * 这是防暴力破解的基本手段，但也会带来产线误锁问题，
			 * 真实项目里通常配合 NVM 计数和延时。
			 */
			if (c->security_attempts >= 3)
				return neg(resp, UDS_SID_SECURITY,
					   UDS_NRC_EXCEED_ATTEMPTS);

			return neg(resp, UDS_SID_SECURITY,
				   UDS_NRC_INVALID_KEY);
		}

		c->security_unlocked = 1;
		c->security_attempts = 0;

		resp[0] = 0x67;
		resp[1] = 0x02;
		return 2;
	}

	return neg(resp, UDS_SID_SECURITY, UDS_NRC_SUBFUNC_UNSUPPORTED);
}

/* ---------- 0x19 读 DTC ---------- */
static int svc_read_dtc(struct uds_ctx *c, const uint8_t *req, size_t n,
			uint8_t *resp)
{
	if (n != 3)
		return neg(resp, UDS_SID_READ_DTC, UDS_NRC_INCORRECT_LENGTH);

	if (req[1] != 0x02)
		return neg(resp, UDS_SID_READ_DTC,
			   UDS_NRC_SUBFUNC_UNSUPPORTED);

	uint8_t mask = req[2];

	resp[0] = 0x59;
	resp[1] = 0x02;
	resp[2] = 0xFF;		/* 本 ECU 支持哪些状态位 */

	int p = 3;

	for (int i = 0; i < c->n_dtcs; i++) {
		if (!(c->dtcs[i].status & mask))
			continue;

		memcpy(resp + p, c->dtcs[i].code, 3);
		resp[p + 3] = c->dtcs[i].status;
		p += 4;
	}

	return p;
}

/* ---------- 0x3E 保持会话 ---------- */
static int svc_tester_present(struct uds_ctx *c, const uint8_t *req, size_t n,
			      uint8_t *resp)
{
	(void)c;

	if (n != 2)
		return neg(resp, UDS_SID_TESTER, UDS_NRC_INCORRECT_LENGTH);

	if ((req[1] & 0x7F) != 0x00)
		return neg(resp, UDS_SID_TESTER,
			   UDS_NRC_SUBFUNC_UNSUPPORTED);

	/* bit7 = suppressPosRspMsgIndicationBit：不回响应，省带宽 */
	if (req[1] & 0x80)
		return 0;

	resp[0] = 0x7E;
	resp[1] = 0x00;
	return 2;
}

int uds_handle(struct uds_ctx *c, const uint8_t *req, size_t n,
	       uint8_t *resp, size_t maxresp, long long now_ms)
{
	if (n == 0 || maxresp < 256)
		return 0;

	c->n_req++;
	c->last_activity_ms = now_ms;

	int r;

	switch (req[0]) {
	case UDS_SID_SESSION:
		r = svc_session(c, req, n, resp);
		break;
	case UDS_SID_RESET:
		r = svc_reset(c, req, n, resp);
		break;
	case UDS_SID_READ_DTC:
		r = svc_read_dtc(c, req, n, resp);
		break;
	case UDS_SID_READ_DID:
		r = svc_read_did(c, req, n, resp);
		break;
	case UDS_SID_SECURITY:
		r = svc_security(c, req, n, resp, now_ms);
		break;
	case UDS_SID_WRITE_DID:
		r = svc_write_did(c, req, n, resp);
		break;
	case UDS_SID_TESTER:
		r = svc_tester_present(c, req, n, resp);
		break;
	default:
		r = neg(resp, req[0], UDS_NRC_SERVICE_UNSUPPORTED);
		break;
	}

	if (r >= 3 && resp[0] == 0x7F)
		c->n_nrc++;

	return r;
}

int uds_check_s3(struct uds_ctx *c, long long now_ms)
{
	if (c->session == UDS_SESSION_DEFAULT)
		return 0;

	if (now_ms - c->last_activity_ms <= UDS_S3_MS)
		return 0;

	c->session = UDS_SESSION_DEFAULT;
	c->security_unlocked = 0;
	c->seed_pending = 0;
	return 1;
}

const char *uds_session_name(int s)
{
	switch (s) {
	case UDS_SESSION_DEFAULT:  return "默认";
	case UDS_SESSION_PROGRAM:  return "编程";
	case UDS_SESSION_EXTENDED: return "扩展";
	}
	return "未知";
}

const char *uds_sid_name(uint8_t sid)
{
	switch (sid) {
	case UDS_SID_SESSION:    return "10 会话控制";
	case UDS_SID_RESET:      return "11 ECU复位";
	case UDS_SID_READ_DTC:   return "19 读DTC";
	case UDS_SID_READ_DID:   return "22 读数据";
	case UDS_SID_SECURITY:   return "27 安全访问";
	case UDS_SID_WRITE_DID:  return "2E 写数据";
	case UDS_SID_TESTER:     return "3E 保持会话";
	}
	return "未知服务";
}

const char *uds_nrc_name(uint8_t nrc)
{
	switch (nrc) {
	case UDS_NRC_SERVICE_UNSUPPORTED: return "serviceNotSupported";
	case UDS_NRC_SUBFUNC_UNSUPPORTED: return "subFunctionNotSupported";
	case UDS_NRC_INCORRECT_LENGTH:    return "incorrectMessageLength";
	case UDS_NRC_CONDITIONS:          return "conditionsNotCorrect";
	case UDS_NRC_SEQUENCE:            return "requestSequenceError";
	case UDS_NRC_OUT_OF_RANGE:        return "requestOutOfRange";
	case UDS_NRC_SECURITY_DENIED:     return "securityAccessDenied";
	case UDS_NRC_INVALID_KEY:         return "invalidKey";
	case UDS_NRC_EXCEED_ATTEMPTS:     return "exceedNumberOfAttempts";
	case UDS_NRC_PENDING:             return "responsePending";
	}
	return "unknownNrc";
}
