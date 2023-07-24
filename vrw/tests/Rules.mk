d := $(dir $(lastword $(MAKEFILE_LIST)))

GTEST_SRCS += $(d)vrw-test.cc

$(d)vrw-test: $(o)vrw-test.o \
	$(OBJS-vrw-replica) $(OBJS-vrw-witness) $(OBJS-vrw-client) \
	$(LIB-simtransport) \
	$(GTEST_MAIN)

TEST_BINS += $(d)vrw-test
