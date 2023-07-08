d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	client.cc benchmark.cc replica.cc)

OBJS-benchmark := $(o)benchmark.o \
                  $(LIB-message) $(LIB-latency)

$(d)client: $(o)client.o $(OBJS-spec-client) $(OBJS-vrw-client) $(OBJS-vr-client) $(OBJS-fastpaxos-client) $(OBJS-unreplicated-client) $(OBJS-benchmark) $(LIB-udptransport)

$(d)replica: $(o)replica.o $(OBJS-spec-replica) $(OBJS-vrw-replica) $(OBJS-vr-replica) $(OBJS-fastpaxos-replica) $(OBJS-unreplicated-replica) $(LIB-udptransport)

BINS += $(d)client $(d)replica
