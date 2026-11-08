/*
 * 阶段 4 —— 链路监控与自动恢复 link_watch
 *
 * vcan 造不出 Bus-Off，但它能造出一件在真车上同样常见的事：**接口消失**。
 * USB-CAN 适配器被拔掉、内核模块被卸载、网卡被 down 掉，
 * 用户态看到的现象都是一样的：套接字还在，但 write 返回 ENETDOWN。
 *
 * 这个程序演示一个能长期在车上跑下去的节点应该怎么写：
 *
 *   1) 不靠「write 失败」去发现链路问题
 *      write 失败是被动的，一秒钟才试一次，就有一秒的盲区；
 *      而且接口恢复时它不会告诉你。
 *
 *   2) 用 netlink 订阅链路事件（RTMGRP_LINK）
 *      接口 up/down/新增/删除，内核会主动推事件过来，毫秒级。
 *      这是 ip 命令背后的机制，也是所有「网络守护」程序的标准做法。
 *
 *   3) 恢复时靠**接口名**重新查 ifindex，而不是记住旧的
 *      这是最容易踩的一脚：vcan0 删掉再建，名字还叫 vcan0，
 *      但 ifindex 变了（22 -> 23）。记住旧 ifindex 去 bind，
 *      会得到一个 EINVAL，现象是「明明设备在，就是绑不上」。
 *      USB 适配器重插、docker 里的 veth 重建，都是这个坑。
 *
 * 运行： ./04-pro/link_watch vcan0
 * 实验： 另开终端 ip link set vcan0 down / up，观察它自动恢复
 *        更狠一点：ip link del vcan0; ip link add dev vcan0 type vcan; ip link set vcan0 up
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#include "can_proto.h"

#define TX_PERIOD_MS 500

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void die(const char *what)
{
	fprintf(stderr, "[-] %s 失败: %s\n", what, strerror(errno));
	exit(EXIT_FAILURE);
}

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- 节点状态 ---------------- */

struct node {
	const char *ifname;
	int fd;			/* -1 表示当前没有可用套接字 */
	int ifindex;
	unsigned long tx_ok, tx_err, reopen;

	long long t0;
	int link_up;
};

static void node_close(struct node *n)
{
	if (n->fd >= 0) {
		close(n->fd);
		n->fd = -1;
	}
}

/*
 * 打开并绑定。注意这里每次都用 if_nametoindex 重新查 ——
 * 不要缓存上一次的 ifindex。
 */
static int node_open(struct node *n)
{
	int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (s < 0) {
		fprintf(stderr, "[-] socket: %s\n", strerror(errno));
		return -1;
	}

	unsigned int idx = if_nametoindex(n->ifname);

	if (idx == 0) {
		fprintf(stderr, "[-] 接口 %s 不存在，等它回来\n", n->ifname);
		close(s);
		return -1;
	}

	struct sockaddr_can addr;

	memset(&addr, 0, sizeof(addr));
	addr.can_family  = AF_CAN;
	addr.can_ifindex = (int)idx;
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "[-] bind(ifindex=%u): %s\n", idx, strerror(errno));
		close(s);
		return -1;
	}

	n->fd = s;
	n->ifindex = (int)idx;
	n->reopen++;

	printf("[%6.1fs] 套接字就绪  %s ifindex=%d fd=%d\n",
	       (double)(now_ms() - n->t0) / 1000.0, n->ifname, n->ifindex, s);
	fflush(stdout);
	return 0;
}

static void node_send(struct node *n)
{
	const struct can_msg_def *m = proto_lookup(CAN_ID_ENGINE_SLOW);
	struct can_frame f;
	double phys[ES_NSIG] = { 88.0, 280.0, 0.0 };

	memset(&f, 0, sizeof(f));
	f.can_id  = m->id;
	f.can_dlc = m->dlc;
	proto_encode(m, phys, f.data);

	if (write(n->fd, &f, sizeof(f)) == (ssize_t)sizeof(f)) {
		n->tx_ok++;
		return;
	}

	/*
	 * 兜底：即使净链路事件丢了，write 的 errno 也能告诉我们链路状态。
	 * ENETDOWN = 接口 down；ENODEV/ENXIO = 设备已被删除。
	 * 这两种情况都要主动关掉套接字，然后等 netlink 通知重建。
	 */
	n->tx_err++;
	if (errno == ENETDOWN || errno == ENODEV || errno == ENXIO) {
		printf("[%6.1fs] write 失败: %s —— 主动关闭套接字，等链路恢复\n",
		       (double)(now_ms() - n->t0) / 1000.0, strerror(errno));
		n->link_up = 0;
		node_close(n);
	} else {
		printf("[%6.1fs] write 失败: %s\n",
		       (double)(now_ms() - n->t0) / 1000.0, strerror(errno));
	}
	fflush(stdout);
}

/* ---------------- netlink ---------------- */

static int nl_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);

	if (fd < 0)
		die("socket(AF_NETLINK)");

	struct sockaddr_nl sa;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	sa.nl_groups = RTMGRP_LINK;	/* 只要链路层事件 */

	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
		die("bind(AF_NETLINK)");

	return fd;
}

/* 从 ifinfomsg 后面取 IFLA_IFNAME */
static const char *attr_ifname(struct nlmsghdr *nh, struct ifinfomsg *ifi)
{
	int len = (int)nh->nlmsg_len - (int)NLMSG_LENGTH(sizeof(*ifi));
	struct rtattr *rta = (struct rtattr *)((char *)ifi +
					       NLMSG_ALIGN(sizeof(*ifi)));

	for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
		if (rta->rta_type == IFLA_IFNAME)
			return (const char *)RTA_DATA(rta);
	return NULL;
}

static void nl_handle(struct node *n, int fd)
{
	char buf[4096];
	ssize_t len = recv(fd, buf, sizeof(buf), 0);

	if (len < 0) {
		if (errno == EINTR || errno == EAGAIN)
			return;
		die("recv(netlink)");
	}

	for (struct nlmsghdr *nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, len);
	     nh = NLMSG_NEXT(nh, len)) {
		if (nh->nlmsg_type == NLMSG_DONE)
			return;
		if (nh->nlmsg_type != RTM_NEWLINK && nh->nlmsg_type != RTM_DELLINK)
			continue;

		struct ifinfomsg *ifi = NLMSG_DATA(nh);
		const char *name = attr_ifname(nh, ifi);

		if (!name || strcmp(name, n->ifname))
			continue;

		double t = (double)(now_ms() - n->t0) / 1000.0;

		if (nh->nlmsg_type == RTM_DELLINK) {
			printf("[%6.1fs] <netlink> %s 被删除\n", t, name);
			n->link_up = 0;
			node_close(n);
			continue;
		}

		int up = (ifi->ifi_flags & IFF_UP) != 0;

		printf("[%6.1fs] <netlink> %s %s  ifindex=%d\n", t, name,
		       up ? "UP" : "DOWN", ifi->ifi_index);

		if (!up) {
			n->link_up = 0;
			node_close(n);
		} else if (n->fd < 0) {
			n->link_up = 1;
			node_open(n);	/* 重新查 ifindex，可能已经变了 */
		}
		fflush(stdout);
	}
}

/* ---------------- main ---------------- */

int main(int argc, char *argv[])
{
	struct node n;

	memset(&n, 0, sizeof(n));
	n.ifname = (argc > 1) ? argv[1] : "vcan0";
	n.fd = -1;
	n.t0 = now_ms();

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	printf("=== 链路监控 ===\n");
	printf("  接口 %s，每 %d ms 发一帧 0x181\n", n.ifname, TX_PERIOD_MS);
	printf("  试试: ip link set %s down   然后   ip link set %s up\n\n",
	       n.ifname, n.ifname);

	int nl = nl_open();

	node_open(&n);

	struct pollfd pfd[2] = {
		{ .fd = nl,    .events = POLLIN },
		{ .fd = n.fd,  .events = 0 },	/* 只写不读，events=0 */
	};

	long long next_tx = now_ms() + TX_PERIOD_MS;
	long long next_stat = now_ms() + 5000;

	while (!g_stop) {
		pfd[0].fd = nl;
		pfd[1].fd = n.fd;

		int r = poll(pfd, 2, 100);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			die("poll");
		}

		if (r > 0 && (pfd[0].revents & POLLIN))
			nl_handle(&n, nl);

		long long now = now_ms();

		/*
		 * 发送用「到期就补一次」的策略：链路断的时候不累积欠账，
		 * 否则恢复瞬间会突刺一串帧出去。
		 */
		if (now >= next_tx) {
			next_tx = now + TX_PERIOD_MS;
			if (n.fd >= 0)
				node_send(&n);
		}

		if (now >= next_stat) {
			printf("[%6.1fs] 状态 %s  tx_ok=%lu tx_err=%lu 重开 %lu 次\n",
			       (double)(now - n.t0) / 1000.0,
			       n.fd >= 0 ? "在线" : "离线",
			       n.tx_ok, n.tx_err, n.reopen);
			fflush(stdout);
			next_stat = now + 5000;
		}
	}

	node_close(&n);
	close(nl);
	printf("\n退出：共发送 %lu 帧，失败 %lu 次，重开套接字 %lu 次\n",
	       n.tx_ok, n.tx_err, n.reopen);
	return 0;
}
