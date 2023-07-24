d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), \
	replica.cc witness.cc client.cc)

PROTOS += $(addprefix $(d), \
	    vrw-proto.proto)

OBJS-vrw-client := $(o)client.o $(o)vrw-proto.o \
                   $(OBJS-client) $(LIB-message) \
                   $(LIB-configuration)

OBJS-vrw-replica := $(o)replica.o $(o)vrw-proto.o \
                   $(OBJS-replica) $(LIB-message) \
                   $(LIB-configuration) $(LIB-latency)

OBJS-vrw-witness := $(o)witness.o $(o)vrw-proto.o \
                   $(OBJS-witness) $(LIB-message) \
                   $(LIB-configuration) $(LIB-latency)

include $(d)tests/Rules.mk

