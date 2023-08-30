
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/util/message_differencer.h>
#include <vector>

#include "vrw/vrw-proto.pb.h"
#include "lib/beehive_lib.h"
#include "lib/message.h"


using namespace google::protobuf;

typedef struct __attribute__ ((packed)) {
    uint32_t frag;
    uint8_t  msg_type;
    uint64_t data_size;
} beehive_hdr_t;

const uint64_t NONFRAG_MAGIC = 0x20050318;
const uint64_t FRAG_MAGIC = 0x20101010;

static size_t ToBeehiveWire(const ::google::protobuf::Message &m, char *out);
static const char * FromBeehiveWire(google::protobuf::Message *msg, const char *buf);
static size_t get_beehive_wire_size(const ::google::protobuf::Message &m);
static void write_u64_be(char *ptr, uint64_t value);


static void hexdump_buf(const char *buf, size_t len) {
    if (Message_DebugEnabled(__FILE__)) {
        for (size_t i = 0; i < len; i++) {
            printf("%02x", buf[i]);
            printf("\n");
        }
    }
}

bool CheckMessage(const Message &orig_msg) {
    static specpaxos::vrw::proto::RequestMessage request;
    static specpaxos::vrw::proto::ReplyMessage reply;
    static specpaxos::vrw::proto::PrepareMessage prepare;
    static specpaxos::vrw::proto::PrepareOKMessage prepareOK;
    static specpaxos::vrw::proto::CommitMessage commit;
    char *serialized_buf;

    bool result = false; 
    size_t buf_len = SerializeMessageBeehive(orig_msg, &serialized_buf);
    Debug("Total buf len is %lu\n", buf_len);
    if (buf_len == 0) {
        Notice("Serialization not implemented, returning without checking\n");
        return true;
    }
    string msgType, msg;

    DecodePacketBeehive(serialized_buf+sizeof(uint32_t), buf_len-sizeof(uint32_t), msgType, msg);
    Debug("Got message type %s\n", msgType.c_str());
    if (msgType == request.GetTypeName()) {
        request.ParseFromString(msg);
        result = util::MessageDifferencer::Equals(request, orig_msg);
    }
    else if (msgType == reply.GetTypeName()) {
        reply.ParseFromString(msg);
        result = util::MessageDifferencer::Equals(reply, orig_msg);
    }
    else if (msgType == prepare.GetTypeName()) {
        prepare.ParseFromString(msg);
        result = util::MessageDifferencer::Equals(prepare, orig_msg);
    }
    else if (msgType == prepareOK.GetTypeName()) {
        prepareOK.ParseFromString(msg);
        result = util::MessageDifferencer::Equals(prepareOK, orig_msg);
    }
    else if (msgType == commit.GetTypeName()) {
        commit.ParseFromString(msg);
        result = util::MessageDifferencer::Equals(commit, orig_msg);
    }

    if (!result) {
        Notice("Serialization failed on message type %s\n", msgType.c_str());
    }
    else {
        Debug("Serialization successful!");
    }
    return result;
}

size_t SerializeMessageBeehive(const ::google::protobuf::Message &m, char **out) {
    const google::protobuf::Descriptor *desc = m.GetDescriptor();
    std::string type = m.GetTypeName();
    Debug("Serializing message with type %s\n", type.c_str());
    MsgTypeEnum type_enum;

    // TODO: Handle view change types
    static specpaxos::vrw::proto::RequestMessage request;
    static specpaxos::vrw::proto::ReplyMessage reply;
    static specpaxos::vrw::proto::PrepareMessage prepare;
    static specpaxos::vrw::proto::PrepareOKMessage prepareOK;
    static specpaxos::vrw::proto::CommitMessage commit;
    static specpaxos::vrw::proto::RequestStateTransferMessage requestStateTransfer;
    static specpaxos::vrw::proto::StateTransferMessage stateTransfer;
    static specpaxos::vrw::proto::StartViewChangeMessage startViewChange;
    static specpaxos::vrw::proto::DoViewChangeMessage doViewChange;
    static specpaxos::vrw::proto::StartViewMessage startView;

    const char* data;
    // fragmagic size, 1 for type enum, 8 for data_len
    size_t total_len = sizeof(uint32_t) + 1 + sizeof(uint64_t);
    size_t data_len = get_beehive_wire_size(m);
    char data_arr[data_len];
    size_t result_size;
    string serialized;
    Debug("Data length is %lu\n", data_len);

    if (type == prepare.GetTypeName()) {
        type_enum = MsgTypeEnum::Prepare;
        result_size = ToBeehiveWire(m, data_arr);
        data = data_arr;
    }
    else if (type == prepareOK.GetTypeName()) {
        type_enum = MsgTypeEnum::PrepareOK;
        result_size = ToBeehiveWire(m, data_arr);
        data = data_arr;
    } 
    else if (type == commit.GetTypeName()) {
        type_enum = MsgTypeEnum::Commit;
        result_size = ToBeehiveWire(m, data_arr);
        data = data_arr;
    }
    else if (type == reply.GetTypeName()) {
        type_enum = MsgTypeEnum::Reply;
        serialized = m.SerializeAsString();
        data = serialized.c_str();
        data_len = serialized.length();
        result_size = data_len;
    }
    else if (type == request.GetTypeName()) {
        type_enum = MsgTypeEnum::Request;
        serialized = m.SerializeAsString();
        data = serialized.c_str();
        data_len = serialized.length();
        result_size = data_len;
    }
    else if (type == requestStateTransfer.GetTypeName()) {
        type_enum = MsgTypeEnum::RequestStateTransfer;
        result_size = ToBeehiveWire(m, data_arr);
        data = data_arr;
    }
    else if (type == stateTransfer.GetTypeName()) {
        type_enum = MsgTypeEnum::StateTransfer;
        result_size = ToBeehiveWire(m, data_arr);
        data = data_arr;
    }
    else if (type == startViewChange.GetTypeName()) {
        type_enum = MsgTypeEnum::StartViewChange;
        result_size = ToBeehiveWire(m, data_arr);
        data = data_arr;
    }
    else if (type == doViewChange.GetTypeName()) {
        data_len = get_beehive_wire_size(m);
        type_enum = MsgTypeEnum::DoViewChange;
        result_size = ToBeehiveWire(m, data_arr);
        data = data_arr;
    }
    else if (type == startView.GetTypeName()) {
        type_enum = MsgTypeEnum::StartView;
        result_size = ToBeehiveWire(m, data_arr);
        data = data_arr;
    }
    else {
        Panic("Got message type %s which has no enum value", type.c_str());
        type_enum = MsgTypeEnum::NONE;
        return 0;
    }

    assert(result_size == data_len);

    total_len += data_len;
    char *out_buf = new char[total_len];
    beehive_hdr_t *hdr = (beehive_hdr_t *)out_buf;
    hdr->frag = NONFRAG_MAGIC;
    hdr->msg_type = (uint8_t)type_enum;
    write_u64_be((char *)(&(hdr->data_size)), data_len);

    char *wr_ptr = out_buf + sizeof(beehive_hdr_t);
    memcpy(wr_ptr, data, data_len);

    *out = out_buf;
    //Debug("Serialized buffer is ");
    //hexdump_buf(data, data_len);

    return total_len;
}

static void write_u64_be(char *ptr, uint64_t value) {
    // loop over backwards, because we're big endian
    char * val_ptr = (char *)(&value);
    for (size_t i = 0; i < sizeof(uint64_t); i++) {
        ptr[i] = val_ptr[sizeof(uint64_t) - 1 - i];
    }
}

static uint64_t read_u64_be(const char *ptr) {
    char reversed[sizeof(uint64_t)];
    for (size_t i = 0; i < sizeof(uint64_t); i++) {
        reversed[i] = ptr[sizeof(uint64_t) - 1 - i];
    }
    uint64_t * num_ptr = (uint64_t *)(reversed);
    return *num_ptr;
}

static size_t ToBeehiveWire(const ::google::protobuf::Message &m, char *out) {
    const Descriptor *desc = m.GetDescriptor();
    const Reflection *refl = m.GetReflection();
    char * curr_ptr = out;
    size_t curr_size = 0;

    int num_fields = desc->field_count();
    for (int i = 0; i < num_fields; i++) {
        // get the field
        const FieldDescriptor * field_desc = desc->field(i);
        FieldDescriptor::Type field_type = field_desc->type();
        Debug("Field %d has name %s and has type %d\n", i, field_desc->name().c_str(), field_desc->type());
        // is the field repeated?
        if (field_desc->is_repeated()) {
            // write the field count
            int field_count = refl->FieldSize(m, field_desc);
            write_u64_be(curr_ptr, (uint64_t)field_count);
            curr_ptr += sizeof(uint64_t);

            Debug("Field is repeated with a count of %d\n", field_count);
            for (int rep_index = 0; rep_index < field_count; rep_index++) {
                // figure out what type we're getting
                switch (field_type) {
                    case FieldDescriptor::TYPE_MESSAGE: {
                        const Message &inner_msg = refl->GetRepeatedMessage(m, field_desc, rep_index);
                        curr_ptr += ToBeehiveWire(inner_msg, curr_ptr);
                        break;
                    }
                    case FieldDescriptor::TYPE_UINT64: {
                        uint64_t field_value = refl->GetRepeatedUInt64(m, field_desc, rep_index);
                        Debug("Field value is %lu\n", field_value);
                        write_u64_be(curr_ptr, field_value);
                        curr_ptr += sizeof(uint64_t);
                        break;
                    }   
                    case FieldDescriptor::TYPE_UINT32: {
                        uint32_t field_value = refl->GetRepeatedUInt32(m, field_desc, rep_index);
                        Debug("Field value is %u\n", field_value);
                        write_u64_be(curr_ptr, (uint64_t)field_value);
                        curr_ptr += sizeof(uint64_t);
                        break;
                    }
                    case FieldDescriptor::TYPE_BYTES: {
                        std::string bytes = refl->GetRepeatedString(m, field_desc, rep_index);
                        Debug("Field value is %s with length %lu\n", bytes.c_str(), bytes.length());
                        write_u64_be(curr_ptr, bytes.length());
                        curr_ptr += sizeof(uint64_t);
                        memcpy(curr_ptr, bytes.c_str(), bytes.length());
                        curr_ptr += bytes.length();
                        break;
                    }
                    default:
                        Notice("Unimplemented for repeated field type %d", field_type);

                }
            }
        }
        else {
            // check the type
            switch (field_type) {
                case FieldDescriptor::TYPE_MESSAGE: {
                    const Message &inner_msg = refl->GetMessage(m, field_desc);
                    curr_ptr += ToBeehiveWire(inner_msg, curr_ptr);
                    break;
                }
                case FieldDescriptor::TYPE_UINT64: {
                    uint64_t field_value = refl->GetUInt64(m, field_desc);
                    Debug("Field value is %lu\n", field_value);
                    write_u64_be(curr_ptr, field_value);
                    curr_ptr += sizeof(uint64_t);
                    break;
                }
                case FieldDescriptor::TYPE_UINT32: {
                    uint32_t field_value = refl->GetUInt32(m, field_desc);
                    Debug("Field value is %u\n", field_value);
                    write_u64_be(curr_ptr, (uint64_t)field_value);
                    curr_ptr += sizeof(uint64_t);
                    break;
                }
                case FieldDescriptor::TYPE_BYTES: {
                    std::string bytes = refl->GetString(m, field_desc);
                    Debug("Field value is %s with length %lu\n", bytes.c_str(), bytes.length());
                    write_u64_be(curr_ptr, bytes.length());
                    curr_ptr += sizeof(uint64_t);
                    memcpy(curr_ptr, bytes.c_str(), bytes.length());
                    curr_ptr += bytes.length();
                    break;
                }
                default:
                    Notice("Unimplemented for field type %d", field_type);
            }
        }
    }
    return (curr_ptr - out);
}


static size_t get_beehive_wire_size(const Message &m) {
    const Descriptor *desc = m.GetDescriptor();
    const Reflection *refl = m.GetReflection();

    std::vector<const FieldDescriptor *> fields;

    refl->ListFields(m, &fields);
    int num_fields = desc->field_count();
    size_t msg_size = 0;

    for (int i = 0; i < num_fields; i++) {
        const FieldDescriptor * field = desc->field(i);
        FieldDescriptor::Type field_type = field->type();
        Debug("Serializing field %s\n", field->name().c_str());

        // is the field repeated?
        if (field->is_repeated()) {
            int field_count = refl->FieldSize(m, field);
            msg_size += sizeof(uint64_t);
            switch (field_type) {
                case FieldDescriptor::TYPE_MESSAGE: {
                    for (int rep_index = 0; rep_index < field_count; rep_index++) {
                        const Message &inner_msg = refl->GetRepeatedMessage(m, field, rep_index);
                        msg_size += get_beehive_wire_size(inner_msg);
                    }
                    break;
                }
                case FieldDescriptor::TYPE_UINT64: {
                    msg_size += (sizeof(uint64_t) * field_count);
                    break;
                }   
                case FieldDescriptor::TYPE_UINT32: {
                    msg_size += (sizeof(uint64_t) * field_count);
                    break;
                }
                case FieldDescriptor::TYPE_BYTES: {
                    for (int rep_index = 0; rep_index < field_count; rep_index++) {
                        std::string bytes = refl->GetRepeatedString(m, field, rep_index);
                        // add 8 for len field
                        msg_size += sizeof(uint64_t);
                        msg_size += bytes.length();
                    }
                    break;
                }
                default:
                    Notice("Unimplemented for repeated field type %d", field_type);

            }
        }
        else {
            // check the type
            switch (field_type) {
                case FieldDescriptor::TYPE_MESSAGE: {
                    const Message &inner_msg = refl->GetMessage(m, field);
                    msg_size += get_beehive_wire_size(inner_msg);
                    break;
                }
                case FieldDescriptor::TYPE_UINT64: {
                    msg_size += sizeof(uint64_t);
                    break;
                }
                case FieldDescriptor::TYPE_UINT32: {
                    msg_size += sizeof(uint64_t);
                    break;
                }
                case FieldDescriptor::TYPE_BYTES: {
                    std::string bytes = refl->GetString(m, field);
                    msg_size += sizeof(uint64_t);
                    msg_size += bytes.size();
                    break;
                }
                default:
                    Notice("Unimplemented for field type %d", field_type);
            }
        }
    }
    return msg_size;
}

void DecodePacketBeehive(const char *buf, size_t sz, string &type, string &msg) {
    const char *rd_ptr = buf;

    //TODO: Add view change types

    // grab the type enum
    static specpaxos::vrw::proto::RequestMessage request;
    static specpaxos::vrw::proto::ReplyMessage reply;
    static specpaxos::vrw::proto::PrepareMessage prepare;
    static specpaxos::vrw::proto::PrepareOKMessage prepareOK;
    static specpaxos::vrw::proto::CommitMessage commit;
    static specpaxos::vrw::proto::RequestStateTransferMessage requestStateTransfer;
    static specpaxos::vrw::proto::StateTransferMessage stateTransfer;
    static specpaxos::vrw::proto::StartViewChangeMessage startViewChange;
    static specpaxos::vrw::proto::DoViewChangeMessage doViewChange;
    static specpaxos::vrw::proto::StartViewMessage startView;

    Message *msg_used;
    MsgTypeEnum type_enum = (MsgTypeEnum)(buf[0]);
    Debug("message type is %hhu\n", (uint8_t)type_enum);

    rd_ptr += 1;
    uint64_t data_len = read_u64_be(rd_ptr);
    Debug("Decode data len is %lu\n", data_len);
    rd_ptr += sizeof(uint64_t);

    switch (type_enum) {
        case (MsgTypeEnum::Request): {
            type = request.GetTypeName();
            msg = string(rd_ptr, data_len);
            rd_ptr += data_len;
            msg_used = &request;
            break;
        }
        case (MsgTypeEnum::Reply): {
            type = reply.GetTypeName(); 
            msg = string(rd_ptr, data_len);
            rd_ptr += data_len;
            msg_used = &reply;
            break;
        }
        case (MsgTypeEnum::Prepare): {
            type = prepare.GetTypeName();
            rd_ptr = FromBeehiveWire(&prepare, rd_ptr);
            Debug("Prepare is %s", prepare.ShortDebugString().c_str());
            msg = prepare.SerializeAsString();
            msg_used = &prepare;
            break;
        }
        case (MsgTypeEnum::PrepareOK): {
            type = prepareOK.GetTypeName();
            rd_ptr = FromBeehiveWire(&prepareOK, rd_ptr);
            msg = prepareOK.SerializeAsString();
            msg_used = &prepareOK;
            break;
        }
        case (MsgTypeEnum::Commit): {
            type = commit.GetTypeName();
            rd_ptr = FromBeehiveWire(&commit, rd_ptr);
            msg = commit.SerializeAsString();
            msg_used = &commit;
            break;
        }
        case (MsgTypeEnum::RequestStateTransfer): {
            type = requestStateTransfer.GetTypeName();
            rd_ptr = FromBeehiveWire(&requestStateTransfer, rd_ptr);
            msg = requestStateTransfer.SerializeAsString();
            msg_used = &requestStateTransfer;
            break;
        }
        case(MsgTypeEnum::StateTransfer): {
            type = stateTransfer.GetTypeName();
            rd_ptr = FromBeehiveWire(&stateTransfer, rd_ptr);
            msg = stateTransfer.SerializeAsString();
            msg_used = &stateTransfer;
            break;
        }
        case(MsgTypeEnum::StartViewChange): {
            type = startViewChange.GetTypeName();
            rd_ptr = FromBeehiveWire(&startViewChange, rd_ptr);
            msg = startViewChange.SerializeAsString();
            msg_used = &startViewChange;
            break;
        }
        case (MsgTypeEnum::DoViewChange): {
            type = doViewChange.GetTypeName();
            rd_ptr = FromBeehiveWire(&doViewChange, rd_ptr);
            msg = doViewChange.SerializeAsString();
            msg_used = &doViewChange;
            break;
        }
        case (MsgTypeEnum::StartView): {
            type = startView.GetTypeName();
            rd_ptr = FromBeehiveWire(&startView, rd_ptr);
            msg = startView.SerializeAsString();
            msg_used = &startView;
            break;
        }
        default: {
            Panic("Got unimplemented enum value %hhu\n", (uint8_t)type_enum);
        }
    }
    Assert(rd_ptr == buf + data_len);
    msg_used->Clear();
}

static const char * FromBeehiveWire(Message *msg, const char *buf) {
    const Descriptor *desc = msg->GetDescriptor();
    const Reflection *refl = msg->GetReflection();
    const char *rd_ptr = buf;

    int num_fields = desc->field_count();
    for (int i = 0; i < num_fields; i++) {
        // get the field
        const FieldDescriptor * field_desc = desc->field(i);
        FieldDescriptor::Type field_type = field_desc->type();

        Debug("Field %d has name %s and has type %d\n", i, field_desc->name().c_str(), field_desc->type());
        // is the field repeated?
        if (field_desc->is_repeated()) {
            // consume the field count
            uint64_t field_count = read_u64_be(rd_ptr);
            rd_ptr += sizeof(uint64_t);
            Debug("Field is repeated with a count of%lu\n", field_count);

            for (uint64_t rep_index = 0; rep_index < field_count; rep_index++) {
                switch (field_type) {
                    case FieldDescriptor::TYPE_MESSAGE: {
                        Message *inner_msg = refl->AddMessage(msg, field_desc);
                        rd_ptr = FromBeehiveWire(inner_msg, rd_ptr);
                        break;
                    }
                    case FieldDescriptor::TYPE_UINT64: {
                        uint64_t field_val = read_u64_be(rd_ptr);
                        refl->AddUInt64(msg, field_desc, field_val);
                        rd_ptr += sizeof(uint64_t);
                        Debug("Field value is %lu\n", field_val);
                        break;
                    }   
                    case FieldDescriptor::TYPE_UINT32: {
                        uint64_t field_val = read_u64_be(rd_ptr);
                        refl->AddUInt32(msg, field_desc, (uint32_t)(field_val));
                        rd_ptr += sizeof(uint64_t);
                        Debug("Field value is %lu\n", field_val);
                        break;
                    }
                    case FieldDescriptor::TYPE_BYTES: {
                        // read length
                        uint64_t field_len = read_u64_be(rd_ptr);
                        rd_ptr += sizeof(uint64_t);
                        Debug("Length is %lu\n", field_len);
                        string bytes = string(rd_ptr, field_len);
                        refl->AddString(msg, field_desc, bytes);
                        rd_ptr += field_len;
                        Debug("Field value is %s\n", bytes.c_str());
                        break;
                    }
                    default:
                        Notice("Unimplemented for repeated field type %d", field_type);

                }
            } 
        }
        else {
            switch (field_type) {
                case FieldDescriptor::TYPE_MESSAGE: {
                    Message *inner_msg = refl->MutableMessage(msg, field_desc);
                    rd_ptr = FromBeehiveWire(inner_msg, rd_ptr);
                    break;
                }
                case FieldDescriptor::TYPE_UINT64: {
                    uint64_t field_val = read_u64_be(rd_ptr);
                    refl->SetUInt64(msg, field_desc, field_val);
                    rd_ptr += sizeof(uint64_t);
                    Debug("Field value is %lu\n", field_val);
                    break;
                }
                case FieldDescriptor::TYPE_UINT32: {
                    uint64_t field_val = read_u64_be(rd_ptr);
                    refl->SetUInt32(msg, field_desc, (uint32_t)field_val);
                    rd_ptr += sizeof(uint64_t);
                    Debug("Field value is %lu\n", field_val);
                    break;
                }
                case FieldDescriptor::TYPE_BYTES: {
                    uint64_t field_len = read_u64_be(rd_ptr);
                    rd_ptr += sizeof(uint64_t);
                    Debug("Length is %lu\n", field_len);
                    string bytes = string(rd_ptr, field_len);
                    refl->SetString(msg, field_desc, bytes);
                    rd_ptr += field_len;
                    Debug("Field value is %s\n", bytes.c_str());
                    break;
                }
                default: 
                    Notice("Unimplemented for field type %d", field_type);
            }

        }
    }
    return rd_ptr;
}
