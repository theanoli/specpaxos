
#ifndef __BEEHIVE_LIB_H__
#define __BEEHIVE_LIB_H__

#include <google/protobuf/message.h>


// FIXME: We don't handle recovery: a node reappearing as itself
enum class MsgTypeEnum {
    Request = 1,
    Reply = 2,
    UnloggedRequest = 3,
    UnloggedReply = 4,
    Prepare = 5,
    PrepareOK = 6,
    Commit = 7,
    RequestStateTransfer= 8,
    StateTransfer = 9,
    StartViewChange = 10,
    DoViewChange = 11,
    StartView = 12,
    ValidateReadRequest = 13,
    ValidateReadReply = 14,

    NONE=255
};

const uint64_t MsgTypeEnumBytes = 1; 

size_t SerializeMessageBeehive(const ::google::protobuf::Message &m, char **out);
void DecodePacketBeehive(const char *buf, size_t sz, std::string &type, std::string &msg);
bool CheckMessage(const google::protobuf::Message &orig_msg);

#endif
