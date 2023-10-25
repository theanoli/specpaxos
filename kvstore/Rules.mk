d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	client.cc server.cc kvstore.cc benchClient.cc)

PROTOS += $(addprefix $(d), \
	    request.proto)

OBJS-kv-store := $(o)server.o $(o)kvstore.o

OBJS-kv-client := $(o)request.o $(o)client.o \
  $(OBJS-spec-client) $(OBJS-vrw-client) $(OBJS-vr-client) $(OBJS-fastpaxos-client) $(LIB-dkudptransport) $(LIB-udptransport)

$(d)benchClient: $(OBJS-kv-client) $(o)benchClient.o

$(d)replica: $(o)request.o $(OBJS-kv-store) \
	$(OBJS-spec-replica) $(OBJS-vrw-replica) $(OBJS-vrw-witness) $(OBJS-vr-replica) $(OBJS-fastpaxos-replica) $(LIB-dkudptransport) $(LIB-udptransport)

BINS += $(d)benchClient $(d)replica
