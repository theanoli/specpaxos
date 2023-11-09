d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	client.cc server.cc kvstore.cc benchClient.cc)

PROTOS += $(addprefix $(d), \
	    request.proto)

OBJS-kv-store := $(o)server.o $(o)kvstore.o

OBJS-kv-client := $(o)request.o $(o)client.o \
  $(OBJS-vrw-client) $(LIB-dkudptransport)

$(d)benchClient: $(OBJS-kv-client) $(o)benchClient.o

$(d)replica: $(o)request.o $(OBJS-kv-store) \
	$(OBJS-vrw-replica) $(OBJS-vrw-witness) $(LIB-dkudptransport)

BINS += $(d)benchClient $(d)replica
