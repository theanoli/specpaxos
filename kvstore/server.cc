// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * kvstore/server.cc:
 *   Single KVStore server
 *
 **********************************************************************/
#include "kvstore/server.h"
#include "kvstore/request.pb.h"

namespace kvstore {

using namespace specpaxos;

void
Server::ReplicaUpcall(opnum_t opnum, const string &str1, string &str2)
{
    Request request;
    Reply reply;
    int status;

    request.ParseFromString(str1);

    switch (request.op()) {

    case Request::GET:
    {
        string val;
        status = store.get(request.arg0(), val);
        reply.set_value(val);            
        break;
    }

    case Request::PUT:
        status = store.put(request.arg0(), request.arg1());
		reply.set_value("");
        break;

    default:
        Panic("Unrecognized operation.");
    }
    reply.set_status(status);
    reply.SerializeToString(&str2);
}

void Server::LeaderUpcall(opnum_t opnum, const string &op, bool &replicate, string &res)
{
	Request request;
	request.ParseFromString(op);
	
	// We are doing a stealth read; do not replicate
	if (doReadValidation && request.op() == Request::GET) {
		replicate = false;
		res = op;
	} else {
		replicate = true;
		res = op; 
	}
}

void 
Server::SetReadValidation(bool do_read_validation)
{
	doReadValidation = do_read_validation; 
}
}

static void Usage(const char *progName)
{
    fprintf(stderr, "usage: %s -c conf-file -i replica-index\n",
            progName);
    exit(1);
}

int
main(int argc, char **argv)
{
    int index = -1;
    const char *configPath = NULL;
    enum {
        PROTO_UNKNOWN,
        PROTO_VR,
        PROTO_VRW,
        PROTO_SPEC,
        PROTO_FAST,
    } proto = PROTO_UNKNOWN;

	bool validate_reads = false;

  // Parse arguments
    int opt;
    while ((opt = getopt(argc, argv, "c:i:m:s")) != -1) {
        switch (opt) {
			case 's': 
				// Reads should not be logged but should be consistent
				Notice("Doing read validation!");
				validate_reads = true;
				break; 

            case 'c':
                configPath = optarg;
                break;

            case 'i':
            {
                char *strtolPtr;
                index = strtoul(optarg, &strtolPtr, 10);
                if ((*optarg == '\0') || (*strtolPtr != '\0') || (index < 0))
                {
                    fprintf(stderr,
                            "option -i requires a numeric arg\n");
                    Usage(argv[0]);
                }
                break;
            }

            case 'm':
            {
                if (strcasecmp(optarg, "vr") == 0) {
                    proto = PROTO_VR;
                } else if (strcasecmp(optarg, "spec") == 0) {
                    proto = PROTO_SPEC;
                } else if (strcasecmp(optarg, "vrw") == 0) {
                    proto = PROTO_VRW;
                } else if (strcasecmp(optarg, "fast") == 0) {
                    proto = PROTO_FAST;
                } else {
                    fprintf(stderr, "unknown mode '%s'\n", optarg);
                    Usage(argv[0]);
                }
                break;
            }

            default:
                fprintf(stderr, "Unknown argument %s\n", argv[optind]);
                break;
        }
    }

    if (!configPath) {
        fprintf(stderr, "option -c is required\n");
        Usage(argv[0]);
    }

    if (index == -1) {
        fprintf(stderr, "option -i is required\n");
        Usage(argv[0]);
    }

    if (proto == PROTO_UNKNOWN) {
        fprintf(stderr, "option -m is required\n");
        Usage(argv[0]);
    }

    // Load configuration
    std::ifstream configStream(configPath);
    if (configStream.fail()) {
        fprintf(stderr, "unable to read configuration file: %s\n",
                configPath);
        Usage(argv[0]);
    }
    specpaxos::Configuration config(configStream);

    if (index >= config.n) {
        fprintf(stderr, "replica index %d is out of bounds; "
                "only %d replicas defined\n", index, config.n);
        Usage(argv[0]);
    }

    DkTransport transport(0.0, 0.0, 0);

    specpaxos::Replica *replica;
    kvstore::Server server;

    switch (proto) {
        case PROTO_VR:
			server = kvstore::Server();
            replica = new specpaxos::vr::VRReplica(config, index, true,
                                                   &transport, 1, &server);
            break;
			
		case PROTO_VRW:
			// TODO witness
			server = kvstore::Server();
			if (specpaxos::IsWitness(index)) {
				replica = new specpaxos::vrw::VRWWitness(config, index, true,
													   &transport, 1, &server);
			} else {
				replica = new specpaxos::vrw::VRWReplica(config, index, true,
													   &transport, 1, &server);
			}
            break;

		case PROTO_SPEC:
			server = kvstore::Server();
            replica = new specpaxos::spec::SpecReplica(config, index, true, &transport, &server);
            break;

        case PROTO_FAST:
            server = kvstore::Server();
            replica = new specpaxos::fastpaxos::FastPaxosReplica(config, index, true,
                                                                 &transport, &server);
            break;

        default:
            NOT_REACHABLE();
    }
    
	server.SetReadValidation(validate_reads);

    (void)replica;              // silence warning
    transport.Run();

    return 0;
}

