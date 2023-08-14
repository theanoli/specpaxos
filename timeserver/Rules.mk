d := $(dir $(lastword $(MAKEFILE_LIST)))

SRCS += $(addprefix $(d), timeserver.cc)

$(d)replica: $(o)timeserver.o $(OBJS-fastpaxos-replica) \
  $(OBJS-spec-replica) $(OBJS-vr-replica) $(OBJS-vrw-witness) $(OBJS-vrw-replica) $(LIB-udptransport)

BINS += $(d)replica
