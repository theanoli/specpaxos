// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * kvstore/benchClient.cc:
 *   Benchmarking client for KVStore.
 *
 **********************************************************************/

#include "kvstore/client.h"

using namespace std;

int rand_key();

int nKeys = 100;
double alpha = -1;
double *zipf; 

bool ready = false;

int
main(int argc, char **argv)
{
    const char *configPath = NULL;
    const char *keysPath = NULL;
    int duration = 10;
    int nShards = 1;
    int wPer = 50; // Out of 100
    int skew = 0; // difference between real clock and TrueTime
    int error = 0; // error bars
	bool populate = false;

    vector<string> keys;
    string key, value;

    kvstore::Proto mode = kvstore::PROTO_UNKNOWN;

    int opt;
    while ((opt = getopt(argc, argv, "c:d:N:l:w:k:f:m:e:s:p")) != -1) {
        switch (opt) {
        case 'c': // Configuration path
        { 
            configPath = optarg;
            break;
        }

		case 'p': 
		{
			fprintf(stdout, "I am populating the kvstore!\n");
			populate = true;
			break;
		}

        case 'f': // Generated keys path
        { 
            keysPath = optarg;
            break;
        }

        case 'N': // Number of shards.
        { 
            char *strtolPtr;
            nShards = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') ||
                (nShards <= 0)) {
                fprintf(stderr, "option -n requires a numeric arg\n");
            }
            break;
        }

        case 'd': // Duration in seconds to run.
        { 
            char *strtolPtr;
            duration = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') ||
                (duration <= 0)) {
                fprintf(stderr, "option -n requires a numeric arg\n");
            }
            break;
        }

        case 'w': // Percentage of writes (out of 100)
        {
            char *strtolPtr;
            wPer = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') ||
                (wPer < 0 || wPer > 100)) {
                fprintf(stderr, "option -w requires a arg b/w 0-100\n");
            }
            break;
        }

        case 'k': // Number of keys to operate on.
        {
            char *strtolPtr;
            nKeys = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') ||
                (nKeys <= 0)) {
                fprintf(stderr, "option -k requires a numeric arg\n");
            }
            break;
        }
        case 's':
        {
            char *strtolPtr;
            skew = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || (skew < 0))
            {
                fprintf(stderr,
                        "option -s requires a numeric arg\n");
            }
            break;
        }
        case 'e':
        {
            char *strtolPtr;
            error = strtoul(optarg, &strtolPtr, 10);
            if ((*optarg == '\0') || (*strtolPtr != '\0') || (error < 0))
            {
                fprintf(stderr,
                        "option -e requires a numeric arg\n");
            }
            break;
        }

        case 'z': // Zipf coefficient for key selection.
        {
            char *strtolPtr;
            alpha = strtod(optarg, &strtolPtr);
            if ((*optarg == '\0') || (*strtolPtr != '\0'))
            {
                fprintf(stderr,
                        "option -z requires a numeric arg\n");
            }
            break;
        }

        case 'm': // Mode to run in [spec/vr/...]
        {
            if (strcasecmp(optarg, "spec") == 0) {
                mode = kvstore::PROTO_SPEC;
            } else if (strcasecmp(optarg, "vr") == 0) {
                mode = kvstore::PROTO_VR;
            } else if (strcasecmp(optarg, "vrw") == 0) {
                mode = kvstore::PROTO_VRW;
            } else if (strcasecmp(optarg, "fast") == 0) {
                mode = kvstore::PROTO_FAST;
            } else {
                fprintf(stderr, "unknown mode '%s'\n", optarg);
            }
            break;
        }

        default:
            fprintf(stderr, "Unknown argument %s\n", argv[optind]);
        }
    }

    if (mode == kvstore::PROTO_UNKNOWN) {
        fprintf(stderr, "option -m is required\n");
        exit(0);
    }

    kvstore::Client client(mode, configPath, nShards);

    // Read in the keys from a file and populate the key-value store.
	// TS the key-value store is never actually populated here. Any GETs of keys
	// not previously PUT are going to fail. But we probably shouldn't PUT all the
	// keys here, because there may be many clients starting up. 
    ifstream in;
    in.open(keysPath);
    if (!in) {
        fprintf(stderr, "Could not read keys from: %s\n", keysPath);
        exit(0);
    }
    for (int i = 0; i < nKeys; i++) {
        getline(in, key);
        keys.push_back(key);
		if (populate) {
			client.Put(key, key);
		}
    }
	fprintf(stdout, "Done populating the kvstore...\n");
    in.close();

    
    struct timeval t0, t1, t2, t3;

    int nOps = 0; // Number of operations attempted.
    double tLatency = 0.0; // Total latency across all transactions.
    int getCount = 0;
    double getLatency = 0.0;
    int putCount = 0;
    double putLatency = 0.0;
	bool status;

    gettimeofday(&t0, NULL);
    srand(t0.tv_sec + t0.tv_usec);

    while (1) {
        gettimeofday(&t1, NULL);

		key = keys[rand_key()];

		if (rand() % 100 < wPer) {
			gettimeofday(&t2, NULL);
			client.Put(key, key);
			gettimeofday(&t3, NULL);
			
			putCount++;
			putLatency += ((t3.tv_sec - t2.tv_sec)*1000000 + (t3.tv_usec - t2.tv_usec));
			status = true;
		} else {
			gettimeofday(&t2, NULL);
			status = client.Get(key, value);
			gettimeofday(&t3, NULL);

			getCount++;
			getLatency += ((t3.tv_sec - t2.tv_sec)*1000000 + (t3.tv_usec - t2.tv_usec));
		}

        long latency = (t3.tv_sec - t1.tv_sec)*1000000 + (t3.tv_usec - t1.tv_usec);

		// Keys will always exist?!
		ASSERT(status);
        fprintf(stderr, "%d %ld.%06ld %ld.%06ld %ld %d\n", nOps+1, 
				t1.tv_sec, (long)t1.tv_usec, 
				t2.tv_sec, (long)t2.tv_usec, 
				latency, 1);

		tLatency += latency;
        nOps++;

        gettimeofday(&t1, NULL);
        if ( ((t1.tv_sec-t0.tv_sec)*1000000 + (t1.tv_usec-t0.tv_usec)) > duration*1000000) 
            break;
    }

    printf("# Overall_Latency: %lf\n", tLatency/nOps);
    printf("# Nops: %d\n", nOps);
    printf("# Get: %d, %lf\n", getCount, getLatency/getCount);
    printf("# Put: %d, %lf\n", putCount, putLatency/putCount);
    
    exit(0);
    return 0;
}


int rand_key()
{
    if (alpha < 0) {
        // Uniform selection of keys.
        return (rand() % nKeys);
    } else {
        // Zipf-like selection of keys.
        if (!ready) {
            zipf = new double[nKeys];

            double c = 0.0;
            for (int i = 1; i <= nKeys; i++) {
                c = c + (1.0 / pow((double) i, alpha));
            }
            c = 1.0 / c;

            double sum = 0.0;
            for (int i = 1; i <= nKeys; i++) {
                sum += (c / pow((double) i, alpha));
                zipf[i-1] = sum;
            }
            ready = true;
        }

        double random = 0.0;
        while (random == 0.0 || random == 1.0) {
            random = (1.0 + rand())/RAND_MAX;
        }

        // binary search to find key;
        int l = 0, r = nKeys, mid;
        while (l < r) {
            mid = (l + r) / 2;
            if (random > zipf[mid]) {
                l = mid + 1;
            } else if (random < zipf[mid]) {
                r = mid - 1;
            } else {
                break;
            }
        }
        return mid;
    } 
}
