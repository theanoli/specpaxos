#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <string.h>

#include "rapl-poll.h"

static int detect_cpu(void);

static int total_cores=0,total_packages=0;
static int package_map[MAX_PACKAGES];

typedef struct arg_struct {
    uint64_t duration;
    double poll_period;
    uint64_t monitor_pkg;
    char *out_filename;
} args_t;

typedef struct vals_avail {
    int dram_avail;
    int pp0_avail;
    int pp1_avail;
    int psys_avail;
    int diff_units;
} vals_avail_t;

typedef struct record {
    struct timeval timestamp;
    double energy_counter_joules;
} record_t;

static void get_cpu_vals_avail(vals_avail_t *vals_struct, int cpu_model) {
    switch(cpu_model) {
        case CPU_SANDYBRIDGE_EP:
        case CPU_IVYBRIDGE_EP:
            vals_struct->pp0_avail=1;
            vals_struct->pp1_avail=0;
            vals_struct->dram_avail=1;
            vals_struct->diff_units=0;
            vals_struct->psys_avail=0;
            break;

        case CPU_HASWELL_EP:
        case CPU_BROADWELL_EP:
        case CPU_SKYLAKE_X:
            vals_struct->pp0_avail=1;
            vals_struct->pp1_avail=0;
            vals_struct->dram_avail=1;
            vals_struct->diff_units=1;
            vals_struct->psys_avail=0;
            break;

        case CPU_KNIGHTS_LANDING:
        case CPU_KNIGHTS_MILL:
            vals_struct->pp0_avail=0;
            vals_struct->pp1_avail=0;
            vals_struct->dram_avail=1;
            vals_struct->diff_units=1;
            vals_struct->psys_avail=0;
            break;

        case CPU_SANDYBRIDGE:
        case CPU_IVYBRIDGE:
            vals_struct->pp0_avail=1;
            vals_struct->pp1_avail=1;
            vals_struct->dram_avail=0;
            vals_struct->diff_units=0;
            vals_struct->psys_avail=0;
            break;

        case CPU_HASWELL:
        case CPU_HASWELL_ULT:
        case CPU_HASWELL_GT3E:
        case CPU_BROADWELL:
        case CPU_BROADWELL_GT3E:
        case CPU_ATOM_GOLDMONT:
        case CPU_ATOM_GEMINI_LAKE:
        case CPU_ATOM_DENVERTON:
            vals_struct->pp0_avail=1;
            vals_struct->pp1_avail=1;
            vals_struct->dram_avail=1;
            vals_struct->diff_units=0;
            vals_struct->psys_avail=0;
            break;

        case CPU_SKYLAKE:
        case CPU_SKYLAKE_HS:
        case CPU_KABYLAKE:
        case CPU_KABYLAKE_MOBILE:
            vals_struct->pp0_avail=1;
            vals_struct->pp1_avail=1;
            vals_struct->dram_avail=1;
            vals_struct->diff_units=0;
            vals_struct->psys_avail=1;
            break;

    }
}

static int open_msr(int core) {

    char msr_filename[BUFSIZ];
    int fd;

    sprintf(msr_filename, "/dev/cpu/%d/msr", core);
    fd = open(msr_filename, O_RDONLY);
    if ( fd < 0 ) {
        if ( errno == ENXIO ) {
            fprintf(stderr, "rdmsr: No CPU %d\n", core);
            exit(2);
        } else if ( errno == EIO ) {
            fprintf(stderr, "rdmsr: CPU %d doesn't support MSRs\n",
                    core);
            exit(3);
        } else {
            perror("rdmsr:open");
            fprintf(stderr,"Trying to open %s\n",msr_filename);
            exit(127);
        }
    }

    return fd;
}

static long long read_msr(int fd, int which) {

    uint64_t data;

    if ( pread(fd, &data, sizeof data, which) != sizeof data ) {
        perror("rdmsr:pread");
        exit(127);
    }

    return (long long)data;
}

static void write_out_results(char *out_filename, record_t *samples, uint64_t num_samples) {
    FILE* out_fd = fopen(out_filename, "w");

    fprintf(out_fd, "timestamp,energy_joule\n");
    for (uint64_t i = 0; i < num_samples; i++) {
        fprintf(out_fd, "%ld.%06ld,%f\n", samples[i].timestamp.tv_sec, 
                                            samples[i].timestamp.tv_usec,
                                            samples[i].energy_counter_joules);   
    }
}

static int poll_msrs(args_t *poll_args) {
    printf("Starting to poll\n");
    int fd;
    long long result;

    uint64_t max_num_samples = (uint64_t)(((double)(poll_args->duration)/(poll_args->poll_period)) + 1) + 16;
    record_t energy_samples[max_num_samples];
    uint64_t num_samples = 0;
    double cpu_energy_units[MAX_PACKAGES];
    
    int cpu_model = detect_cpu();
    vals_avail_t features;
    get_cpu_vals_avail(&features, cpu_model);
    
    fd=open_msr(package_map[poll_args->monitor_pkg]);

    result=read_msr(fd,MSR_RAPL_POWER_UNIT);

    for (int j = 0; j < total_packages; j++) {
        cpu_energy_units[j] = pow(0.5,(double)((result>>8)&0x1f));
    }

    struct timespec sleep_time;
    sleep_time.tv_sec = (uint64_t)(poll_args->poll_period);
    sleep_time.tv_nsec = (long)((poll_args->poll_period - sleep_time.tv_sec) * pow(10, 9));

    struct timeval run_time_start, curr_time;

    int msr_fd = open_msr(package_map[poll_args->monitor_pkg]);

    gettimeofday(&run_time_start, NULL);
    record_t *curr_entry;
    while (1) {
        printf("Collecting sample\n");
        curr_entry = &(energy_samples[num_samples]);
        
        gettimeofday(&(curr_entry->timestamp), NULL);
        result = read_msr(msr_fd, MSR_PKG_ENERGY_STATUS);
        curr_entry->energy_counter_joules = (double)(result) * cpu_energy_units[poll_args->monitor_pkg];
        
        gettimeofday(&curr_time, NULL);
        num_samples++;
        if (((curr_time.tv_sec-run_time_start.tv_sec)*1000000 
           + (curr_time.tv_usec-run_time_start.tv_usec)) > (poll_args->duration)*1000000) {
            break;
        }
        else {
            nanosleep(&sleep_time, NULL);
        }
    }
    
    write_out_results(poll_args->out_filename, energy_samples, num_samples);

    return 0;
}

static int detect_cpu(void) {

    FILE *fff;

    int family,model=-1;
    char buffer[BUFSIZ],*result;
    char vendor[BUFSIZ];

    fff=fopen("/proc/cpuinfo","r");
    if (fff==NULL) return -1;

    while(1) {
        result=fgets(buffer,BUFSIZ,fff);
        if (result==NULL) break;

        if (!strncmp(result,"vendor_id",8)) {
            sscanf(result,"%*s%*s%s",vendor);

            if (strncmp(vendor,"GenuineIntel",12)) {
                printf("%s not an Intel chip\n",vendor);
                return -1;
            }
        }

        if (!strncmp(result,"cpu family",10)) {
            sscanf(result,"%*s%*s%*s%d",&family);
            if (family!=6) {
                printf("Wrong CPU family %d\n",family);
                return -1;
            }
        }

        if (!strncmp(result,"model",5)) {
            sscanf(result,"%*s%*s%d",&model);
        }

    }

    fclose(fff);

    printf("Found ");

    switch(model) {
        case CPU_SANDYBRIDGE:
            printf("Sandybridge");
            break;
        case CPU_SANDYBRIDGE_EP:
            printf("Sandybridge-EP");
            break;
        case CPU_IVYBRIDGE:
            printf("Ivybridge");
            break;
        case CPU_IVYBRIDGE_EP:
            printf("Ivybridge-EP");
            break;
        case CPU_HASWELL:
        case CPU_HASWELL_ULT:
        case CPU_HASWELL_GT3E:
            printf("Haswell");
            break;
        case CPU_HASWELL_EP:
            printf("Haswell-EP");
            break;
        case CPU_BROADWELL:
        case CPU_BROADWELL_GT3E:
            printf("Broadwell");
            break;
        case CPU_BROADWELL_EP:
            printf("Broadwell-EP");
            break;
        case CPU_SKYLAKE:
        case CPU_SKYLAKE_HS:
            printf("Skylake");
            break;
        case CPU_SKYLAKE_X:
            printf("Skylake-X");
            break;
        case CPU_KABYLAKE:
        case CPU_KABYLAKE_MOBILE:
            printf("Kaby Lake");
            break;
        case CPU_KNIGHTS_LANDING:
            printf("Knight's Landing");
            break;
        case CPU_KNIGHTS_MILL:
            printf("Knight's Mill");
            break;
        case CPU_ATOM_GOLDMONT:
        case CPU_ATOM_GEMINI_LAKE:
        case CPU_ATOM_DENVERTON:
            printf("Atom");
            break;
        default:
            printf("Unsupported model %d\n",model);
            model=-1;
            break;
    }

    printf(" Processor type\n");

    return model;
}

static int detect_packages(void) {

    char filename[BUFSIZ];
    FILE *fff;
    int package;
    int i;

    for(i=0;i<MAX_PACKAGES;i++) package_map[i]=-1;

    printf("\t");
    for(i=0;i<MAX_CPUS;i++) {
        sprintf(filename,"/sys/devices/system/cpu/cpu%d/topology/physical_package_id",i);
        fff=fopen(filename,"r");
        if (fff==NULL) break;
        fscanf(fff,"%d",&package);
        printf("%d (%d)",i,package);
        if (i%8==7) printf("\n\t"); else printf(", ");
        fclose(fff);

        if (package_map[package]==-1) {
            total_packages++;
            package_map[package]=i;
        }

    }

    printf("\n");

    total_cores=i;

    printf("\tDetected %d cores in %d packages\n\n",
        total_cores,total_packages);

    return 0;
}

int main(int argc, char **argv) {
    int c;

    opterr=0;


    args_t args;
    args.duration = 30;
    args.poll_period = 5.0;
    args.monitor_pkg = 0;
    args.out_filename = NULL;

    detect_packages();

    while ((c = getopt(argc, argv, "m:d:p:o:h")) != -1) {
        switch(c) {
            case 'd': {
                char *strtolPtr;
                args.duration = strtoul(optarg, &strtolPtr, 10);
                if ((*optarg == '\0') || (*strtolPtr != '\0')) {
                    fprintf(stderr, "option -d requires an integer\n");
                }
                break;
            }
            case 'p': {
                char *strtolPtr;
                args.poll_period = strtod(optarg, &strtolPtr);
                if ((*optarg == '\0') || (*strtolPtr != '\0') || (args.poll_period <= 0)) {
                    fprintf(stderr, "option -p requires positive double\n");
                }
                break;
            }
            case 'm': {
                char *strtolPtr;
                args.monitor_pkg = strtoul(optarg, &strtolPtr, 10);
                if ((*optarg == '\0') || (*strtolPtr != '\0')) {
                    fprintf(stderr, "option -m must be an integer\n");
                }
                if (args.monitor_pkg < 0 || args.monitor_pkg >= total_packages) {
                    fprintf(stderr, "option m must be between 0 and %d\n", total_packages-1);
                }
                break;
            }
            case 'o': {
                args.out_filename = optarg;
                break;
            }
            case 'h': {
                printf("Usage: %s [-m monitor_pkg] [-h] [-d duration] [-p period]\n\n",argv[0]);
                printf("\t-c core : specifies which CPU package to monitor\n");
                printf("\t-h      : displays this help\n");
                printf("\t-d      : set how long to run for in seconds\n");
                printf("\t-p      : set how often we poll the counters\n");
                exit(0);
            }
        }
    }

    if (args.out_filename == NULL) {
        fprintf(stderr, "Filename is required\n");
        exit(0);
    }

    poll_msrs(&args);
}