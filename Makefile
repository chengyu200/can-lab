# 一次全编：make
# 单独编某个：make 05-isotp/isotp_test
#
# proto.o  是阶段 3/4 的协议层实现，被三个程序共用。
# isotp_user.o 是阶段 5 的用户态 ISO-TP，被测试驱动和故障注入共用。
# -Iinclude 让所有源文件都用 #include "xxx.h"。
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11 -Iinclude
LDLIBS  ?= -lm

# 阶段 9 是 C++（模拟你们真实的 tcp_server 技术栈）
CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -std=c++17 -I09-bridge
CXLIBS   ?= -pthread

BINS := 01-hello/can_hello \
        02-router/can_router \
        03-app/ecu_engine \
        03-app/ecu_dash \
        03-app/can_decode \
        04-pro/bcm_periodic \
        04-pro/link_watch \
        04-pro/fd_demo \
        05-isotp/isotp_test \
        05-isotp/isotp_kernel \
        05-isotp/isotp_fault \
        06-uds/ecu_server \
        06-uds/uds_tester \
        07-j1939/j1939_addr \
        07-j1939/j1939_node \
        08-gateway/gw

PROTO  := proto.o
ISOTP  := 05-isotp/isotp_user.o
UDS    := 06-uds/uds.o
BRIDGE := 09-bridge/can_bridge.o
# ISO-TP 后端：直接复用阶段 5 的 05-isotp/isotp_user.c（C 源码，一行没改），
# 所以链接时要把 $(ISOTP) 一起带上，编译时用 -Iinclude 找 isotp_user.h。
BRIDGE_ISOTP := 09-bridge/can_bridge_isotp.o

CXXBINS := 09-bridge/tcp_server \
           09-bridge/client

all: $(BINS) $(CXXBINS)

$(PROTO): proto.c include/can_proto.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(ISOTP): 05-isotp/isotp_user.c include/isotp_user.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(UDS): 06-uds/uds.c include/uds.h
	$(CC) $(CFLAGS) -c -o $@ $<

01-hello/can_hello: 01-hello/can_hello.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

02-router/can_router: 02-router/can_router.c include/can_proto.h
	$(CC) $(CFLAGS) -o $@ $<

03-app/ecu_engine: 03-app/ecu_engine.c $(PROTO) include/can_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(PROTO) $(LDLIBS)

03-app/ecu_dash: 03-app/ecu_dash.c $(PROTO) include/can_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(PROTO) $(LDLIBS)

03-app/can_decode: 03-app/can_decode.c $(PROTO) include/can_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(PROTO) $(LDLIBS)

04-pro/bcm_periodic: 04-pro/bcm_periodic.c $(PROTO) include/can_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(PROTO) $(LDLIBS)

04-pro/link_watch: 04-pro/link_watch.c $(PROTO) include/can_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(PROTO) $(LDLIBS)

04-pro/fd_demo: 04-pro/fd_demo.c $(PROTO) include/can_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(PROTO) $(LDLIBS)

05-isotp/isotp_test: 05-isotp/isotp_test.c $(ISOTP) include/isotp_user.h
	$(CC) $(CFLAGS) -o $@ $< $(ISOTP) $(LDLIBS)

05-isotp/isotp_kernel: 05-isotp/isotp_kernel.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

05-isotp/isotp_fault: 05-isotp/isotp_fault.c $(ISOTP) include/isotp_user.h
	$(CC) $(CFLAGS) -o $@ $< $(ISOTP) $(LDLIBS)

06-uds/ecu_server: 06-uds/ecu_server.c $(UDS) $(ISOTP) include/uds.h include/isotp_user.h
	$(CC) $(CFLAGS) -o $@ $< $(UDS) $(ISOTP) $(LDLIBS)

06-uds/uds_tester: 06-uds/uds_tester.c $(UDS) include/uds.h
	$(CC) $(CFLAGS) -o $@ $< $(UDS) $(LDLIBS)

07-j1939/j1939_addr: 07-j1939/j1939_addr.c include/j1939_util.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

07-j1939/j1939_node: 07-j1939/j1939_node.c include/j1939_util.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

08-gateway/gw: 08-gateway/gw.c $(PROTO) include/can_proto.h
	$(CC) $(CFLAGS) -o $@ $< $(PROTO) $(LDLIBS)

$(BRIDGE): 09-bridge/can_bridge.cpp 09-bridge/can_bridge.h 09-bridge/common.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BRIDGE_ISOTP): 09-bridge/can_bridge_isotp.cpp 09-bridge/can_bridge_isotp.h \
                 09-bridge/can_bridge.h 09-bridge/common.h include/isotp_user.h
	$(CXX) $(CXXFLAGS) -Iinclude -c -o $@ $<

09-bridge/tcp_server: 09-bridge/tcp_server.cpp $(BRIDGE) $(BRIDGE_ISOTP) $(ISOTP) \
                      09-bridge/can_bridge.h 09-bridge/common.h
	$(CXX) $(CXXFLAGS) -Iinclude -o $@ $< $(BRIDGE) $(BRIDGE_ISOTP) $(ISOTP) $(CXLIBS)

09-bridge/client: 09-bridge/client.cpp 09-bridge/common.h
	$(CXX) $(CXXFLAGS) -o $@ $< $(CXLIBS)

clean:
	rm -f $(BINS) $(CXXBINS) $(PROTO) $(ISOTP) $(UDS) $(BRIDGE) $(BRIDGE_ISOTP)

.PHONY: all clean
