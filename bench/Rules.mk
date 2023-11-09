d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	client.cc benchmark.cc replica.cc)

OBJS-benchmark := $(o)benchmark.o \
                  $(LIB-message) $(LIB-latency)

$(d)client: $(o)client.o $(OBJS-vrw-client) $(OBJS-benchmark) $(LIB-dkudptransport)

$(d)replica: $(o)replica.o $(OBJS-vrw-replica) $(OBJS-vrw-witness) $(LIB-dkudptransport)

BINS += $(d)client $(d)replica
