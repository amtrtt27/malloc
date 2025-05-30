/*
 * Mdriver.c - Autolab version of the CS:APP Malloc Lab Driver
 *
 * Uses a collection of trace files to tests a malloc/free
 * implementation in mm.c.
 *
 * Copyright (c) 2004-2016, R. Bryant and D. O'Hallaron, All rights
 * reserved.  May not be used, modified, or copied without permission.
 */

// GNU extensions used: asprintf
#define _GNU_SOURCE 1

#include <assert.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef USE_MSAN
#include <sanitizer/msan_interface.h>
#endif

#include "config.h"
#include "fcyc.h"
#include "memlib.h"
#include "mm.h"
#include "stree.h"
#include "tracefile.h"
#include "mdriver-helper.h"

/**********************
 * Constants and macros
 **********************/

#ifndef REF_ONLY
#define REF_ONLY 0
#endif

/* Returns true if p is ALIGNMENT-byte aligned */
#define IS_ALIGNED(p) ((((unsigned long)(p)) % ALIGNMENT) == 0)

/***************** Misc *********/
#define MAXLINE 1024 /* max string size */

/******************************
 * The key compound data types
 *****************************/

/*
 * There are two different, easily-confusable concepts:
 * - opnum: which line in the file.
 * - index: the block number ; corresponds to something allocated.
 * Remember that index (-1) is the null pointer.
 */

/*
 * Records the extent of each block's payload.
 * Organized as doubly linked list
 */
typedef struct range_t {
    char *lo;           /* low payload address */
    char *hi;           /* high payload address */
    unsigned int index; /* same index as free; for debugging */
    struct range_t *next;
    struct range_t *prev;
} range_t;

/*
 * All information about set of ranges represented as doubly-linked
 * list of ranges, plus a splay tree keyed by lo addresses
 */
typedef struct {
    range_t *list;
    tree_t *lo_tree;
} range_set_t;

/*
 * Holds the params to the xxx_speed functions, which are timed by fcyc.
 * This struct is necessary because fcyc accepts only a pointer array
 * as input.
 */
typedef struct {
    trace_t *trace;
    range_set_t *ranges;
} speed_t;

/* Summarizes the important stats for some malloc function on some trace */
typedef struct {
    /* set from the trace parameters */
    const char *filename;
    weight_t weight;
    unsigned int ops; /* number of ops (malloc/free/realloc) in the trace */

    /* run-time stats defined for both libc and student */
    bool valid;  /* was the trace processed correctly by the allocator? */
    double secs; /* number of secs needed to run the trace */
    double tput; /* throughput for this trace in Kops/s */

    /* defined only for the student malloc package */
    double util; /* space utilization for this trace (always 0 for libc) */

    /* Note: secs and util are only defined if valid is true */
} stats_t;

/* Summarizes the key statistics for a set of traces */
typedef struct {
    double util; /* average utilization expressed as a percentage */
    double ops;  /* total number of operations */
    double secs; /* total number of elapsed seconds */
    double tput; /* average throughput expressed in Kops/s */
} sum_stats_t;

/********************
 * For debugging.  If debug-mode is on, then we have each block start
 * at a "random" place (a hash of the index), and copy random data
 * into it.  With DBG_CHEAP, we check that the data survived when we
 * realloc and when we free.  With DBG_EXPENSIVE, we check every block
 * every operation.
 * randint_t should be a byte, in case students return unaligned memory.
 *******************/
#define RANDOM_DATA_LEN (1 << 16)

typedef unsigned char randint_t;
static const char randint_t_name[] = "byte";
static randint_t random_data[RANDOM_DATA_LEN];

/********************
 * Global variables
 *******************/

/* Global values */
typedef enum { DBG_NONE, DBG_CHEAP, DBG_EXPENSIVE } debug_mode_t;

static debug_mode_t debug_mode = REF_ONLY ? DBG_NONE : DBG_CHEAP;
static unsigned int verbose = REF_ONLY ? 0 : 1; /* verbosity level */
static int errors = 0; /* number of errs found when running student malloc */
static bool onetime_flag = false;
static bool tab_mode = false; /* Print output as tab-separated fields */
/* If set, use sparse memory emulation */
static bool sparse_mode = SPARSE_MODE;
static size_t maxfill = SPARSE_MODE ? MAXFILL_SPARSE : MAXFILL;

#ifdef SPARSE_MODE
size_t queryGlobalSpaceUsage(void);
#endif

/* by default, no timeouts */
static unsigned int set_timeout = 0;

/* Directory where default tracefiles are found */
static const char default_tracedir[] = TRACEDIR;

/* The following are null-terminated lists of tracefiles that may or may not get
 * used */

/* The filenames of the default tracefiles */
static const char *default_tracefiles[] = {DEFAULT_TRACEFILES, NULL};

/* The filenames of the default tracefiles that require sparse emulation */
static const char *default_giant_tracefiles[] = {DEFAULT_GIANT_TRACEFILES,
                                                 NULL};

/* Performance statistics for driver */

/*********************
 * Function prototypes
 *********************/

/* This function enables generating the set of trace files */
static void add_tracefile(char ***tracefiles_p, size_t *num_tracefiles_p,
                          const char *tracedir, const char *trace);

/* these functions manipulate range sets */
static range_set_t *new_range_set(void);
static bool add_range(range_set_t *ranges, char *lo, size_t size,
                      const trace_t *trace, unsigned int opnum,
                      unsigned int index);
static void remove_range(range_set_t *ranges, char *lo);
static void free_range_set(range_set_t *ranges);

/* These functions implement the debugging code */
static void init_random_data(void);
static bool check_index(const trace_t *trace, unsigned int opnum,
                        unsigned int index);
static void randomize_block(trace_t *trace, unsigned int index);

/* Routines for evaluating the correctness and speed of libc malloc */
static bool eval_libc_valid(trace_t *trace);
static void eval_libc_speed(void *ptr);

/* Routines for evaluating correctness, space utilization, and speed
   of the student's malloc package in mm.c */
static bool eval_mm_valid(trace_t *trace, range_set_t *ranges);
static double eval_mm_util(trace_t *trace, size_t tracenum);
static void eval_mm_speed(void *ptr);
static double compute_scaled_score(double value, double min, double max);

/* Various helper routines */
static void printresults(size_t n, stats_t *stats, sum_stats_t *sumstats);
static void printresultsdbg(size_t n, stats_t *stats, sum_stats_t *sumstats);
static void printresultssparse(size_t n, stats_t *stats, sum_stats_t *sumstats);
static void usage(const char *prog);
static void malloc_error(const trace_t *trace, unsigned int opnum,
                         const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void unix_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));
static void app_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2), noreturn));
static unsigned int atoui_or_usage(const char *arg, const char *option,
                                   const char *prog);

static sigjmp_buf timeout_jmpbuf;

/* Timeout signal handler */
static void timeout_handler(int sig __attribute__((unused))) {
    sio_eprintf("The driver timed out after %u secs\n", set_timeout);
    errors = 1;
    longjmp(timeout_jmpbuf, 1);
}

/* Compute throughput from reference implementation */
static double lookup_ref_throughput(bool checkpoint);
static double measure_ref_throughput(bool checkpoint);

static unsigned int trace_line = 0;
static char* trace_file = NULL;
static int trace_state = -1;
static void debugTraceStatus(void)
{
	const char* state = NULL;
	switch (trace_state)
	{
		case 0: state = "correctness"; break;
		case 1: state = "correctness second time"; break;
		case 2: state = "utilization"; break;
		case 3: state = "throughput"; break;
		default: state = "unknown"; break;
	}
	printf("Currently testing %s in trace %s at op %d\n", state, trace_file, trace_line);
}

/*
 * Run the tests; return the number of tests run (may be less than
 * num_tracefiles, if there's a timeout)
 */
static void run_tests(size_t num_tracefiles, char **tracefiles,
                      stats_t *mm_stats, speed_t *speed_params) {
    volatile size_t i;
    range_set_t *volatile ranges = 0;

    for (i = 0; i < num_tracefiles; i++) {
        /* initialize simulated memory system in memlib.c *
         * start each trace with a clean system */
        mem_init(sparse_mode);
        ranges = new_range_set();

        // NOTE: If times out, then it will reread the trace file

        trace_t *trace = read_trace(tracefiles[i], verbose);
        mm_stats[i].filename = tracefiles[i];
        mm_stats[i].weight = trace->weight;
        mm_stats[i].ops = trace->num_ops;
		trace_file = tracefiles[i];

        /* Prepare for timeout */
        if (setjmp(timeout_jmpbuf) != 0) {
            mm_stats[i].valid = false;
        } else {
            if (verbose > 1) {
                fprintf(stderr, "[%zu/%zu] Checking mm malloc for correctness",
                        i, num_tracefiles);
                fflush(stderr);
            }
			trace_state = 0;
            mm_stats[i].valid =
                /* Do 2 tests, since may fail to reinitialize properly */
                eval_mm_valid(trace, ranges);

			trace_state = 1;
            free_range_set(ranges);
            ranges = new_range_set();
            mm_stats[i].valid =
                mm_stats[i].valid && eval_mm_valid(trace, ranges);

            if (onetime_flag) {
                if (verbose > 1) {
                    fputs(".\n", stderr);
                    fflush(stderr);
                }
                free_trace(trace);
                free_range_set(ranges);
                return;
            }
        }
#if !defined DEBUG && !defined USE_ASAN && !defined USE_MSAN
        if (mm_stats[i].valid) {
            if (verbose > 1) {
                fputs(", efficiency", stderr);
                fflush(stderr);
            }
			trace_state = 2;
            mm_stats[i].util = eval_mm_util(trace, i);
            speed_params->trace = trace;
            speed_params->ranges = ranges;
            if (verbose > 1) {
                fputs(", and performance", stderr);
                fflush(stderr);
            }
			trace_state = 3;
            mm_stats[i].secs =
                sparse_mode ? 1.0 : fsec(eval_mm_speed, speed_params);
            mm_stats[i].tput = mm_stats[i].ops / (mm_stats[i].secs * 1000.0);
        }
#endif
        if (verbose > 0) {
            putc('.', stderr);
            if (verbose > 2)
                fprintf(stderr, " %d operations.  %ld comparisons.  Avg = %.1f",
                        trace->num_ops, ranges->lo_tree->comparison_count,
                        (double)ranges->lo_tree->comparison_count /
                            trace->num_ops);
            if (verbose > 1)
                putc('\n', stderr);
            fflush(stderr);
        }

        free_trace(trace);
        free_range_set(ranges);

        /* clean up memory system */
        mem_deinit();
    }
}

/**************
 * Main routine
 **************/
int main(int argc, char **argv) {
    char **tracefiles = NULL;  /* array of trace file names */
    size_t num_tracefiles = 0; /* the number of traces in that array */

    stats_t *libc_stats = NULL; /* libc stats for each trace */
    stats_t *mm_stats = NULL;   /* mm (i.e. student) stats for each trace */
    speed_t speed_params;       /* input parameters to the xx_speed routines */

    sum_stats_t libc_sum_stats;
    sum_stats_t mm_sum_stats;

    bool run_libc = false;   /* If set, run libc malloc (set by -l) */
    bool autograder = false; /* if set then called by autograder (-A) */
    bool checkpoint = false;

    const char *tracedir = default_tracedir;

    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IOLBF, 0);

    double min_throughput = -1;
    double max_throughput = -1;

#if !REF_ONLY

    int c;
    /*
     * Read and interpret the command line arguments
     */
    while ((c = getopt(argc, argv, "d:f:c:s:t:v:hpCOVAlDT")) != EOF) {
        switch (c) {

        case 'A': /* Hidden Autolab driver argument */
            autograder = true;
            break;
        case 'p':
        case 'C':
            checkpoint = true;
            break;

        case 'f': /* Use one specific trace file only (relative to curr dir) */
            add_tracefile(&tracefiles, &num_tracefiles, "./", optarg);
            break;

        case 'c': /* Use one specific trace file and run only once */
            add_tracefile(&tracefiles, &num_tracefiles, "./", optarg);
            onetime_flag = true;
            break;

        case 't': /* Directory where the traces are located */
            if (num_tracefiles >= 1) {
                app_error("'-t' option must precede any use of '-f'");
            }

            // tracedir must always end with a '/'
            if (optarg[strlen(optarg) - 1] == '/') {
                if ((tracedir = strdup(optarg)) == NULL) {
                    unix_error("strdup failed while processing '-t' option");
                }
            } else {
                if (asprintf((char **)&tracedir, "%s/", optarg) == -1) {
                    unix_error("asprintf failed while processing '-t' option");
                }
            }
            break;

        case 'l': /* Run libc malloc */
            run_libc = true;
            break;

        case 'V': /* Increase verbosity level */
            verbose += 1;
            break;

        case 'v': /* Set verbosity level */
            verbose = atoui_or_usage(optarg, "-v", argv[0]);
            break;

        case 'd':
            debug_mode = atoui_or_usage(optarg, "-d", argv[0]);
            break;

        case 'D':
            debug_mode = DBG_EXPENSIVE;
            break;

        case 's':
            set_timeout = atoui_or_usage(optarg, "-s", argv[0]);
            break;

        case 'T':
            tab_mode = true;
            break;

        case 'h': /* Print usage message */
            usage(argv[0]);
            exit(0);

            /* getopt(3) says '?' means an invalid option, and an
               error message has already been printed */
        case '?':
            usage(argv[0]);
            exit(1);

        default:
            app_error("getopt returned unexpected code '%c'", c);
        }
    }
#endif /* !REF_ONLY */

    if (num_tracefiles == 0) {
        int i;
        if (sparse_mode & !run_libc) {
            for (i = 0; default_giant_tracefiles[i]; i++) {
                add_tracefile(&tracefiles, &num_tracefiles, tracedir,
                              default_giant_tracefiles[i]);
            }
        }
        for (i = 0; default_tracefiles[i]; i++) {
            add_tracefile(&tracefiles, &num_tracefiles, tracedir,
                          default_tracefiles[i]);
        }
    }

    if (debug_mode != DBG_NONE) {
        init_random_data();
    }

    /* Initialize the timeout */
    if (set_timeout > 0) {
        Signal(SIGALRM, timeout_handler);
        alarm((unsigned int)set_timeout);
    }

    /*
     * Optionally run and evaluate the libc malloc package
     */
    if (run_libc) {
        if (verbose > 1)
            fputs("\nTesting libc malloc\n", stderr);

        /* Allocate libc stats array, with one stats_t struct per tracefile */
        libc_stats = calloc(num_tracefiles, sizeof(stats_t));
        if (libc_stats == NULL)
            unix_error("libc_stats calloc in main failed");

        /* Evaluate the libc malloc package using the K-best scheme */
        for (size_t i = 0; i < num_tracefiles; i++) {
            trace_t *trace = read_trace(tracefiles[i], verbose);
            libc_stats[i].filename = tracefiles[i];
            libc_stats[i].weight = trace->weight;
            libc_stats[i].ops = trace->num_ops;

            if (verbose > 1) {
                fprintf(stderr,
                        "[%zu/%zu] Checking libc malloc for correctness", i,
                        num_tracefiles);
                fflush(stderr);
            }
            libc_stats[i].valid = eval_libc_valid(trace);
            if (libc_stats[i].valid) {
                speed_params.trace = trace;
                if (verbose > 1) {
                    fputs(" and performance", stderr);
                    fflush(stderr);
                }
                libc_stats[i].secs = fsec(eval_libc_speed, &speed_params);
            }
            free_trace(trace);
            if (verbose > 1) {
                fputs(".\n", stderr);
                fflush(stderr);
            }
        }

        /* Display the libc results in a compact table and return the
           summary statistics */
        if (verbose > 0) {
            puts("\nResults for libc malloc:");
#if !defined DEBUG
            if (!sparse_mode)
                printresults(num_tracefiles, libc_stats, &libc_sum_stats);
            else 
                printresultssparse(num_tracefiles, libc_stats, &libc_sum_stats);
#else  
            printresultsdbg(num_tracefiles, libc_stats, &libc_sum_stats);
#endif
        }
    }

    if (!REF_ONLY && !onetime_flag && num_tracefiles > 1) {
        /*
         * Get benchmark throughput
         */
        double ref_throughput = measure_ref_throughput(checkpoint);

        min_throughput =
            ref_throughput *
            (checkpoint ? MIN_SPEED_RATIO_CHECKPOINT : MIN_SPEED_RATIO);

        max_throughput =
            ref_throughput *
            (checkpoint ? MAX_SPEED_RATIO_CHECKPOINT : MAX_SPEED_RATIO);

        if (verbose > 0) {
#if !defined DEBUG
            printf("Throughput targets: min=%.0f, max=%.0f, benchmark=%.0f\n",
                   min_throughput, max_throughput, ref_throughput);
#endif
        }
    }

    /*
     * Always run and evaluate the student's mm package
     */
    if (verbose > 1)
        fputs("\nTesting mm malloc\n", stderr);

    /* Allocate the mm stats array, with one stats_t struct per tracefile */
    mm_stats = calloc(num_tracefiles, sizeof(stats_t));
    if (mm_stats == NULL)
        unix_error("mm_stats calloc in main failed");

    run_tests(num_tracefiles, tracefiles, mm_stats, &speed_params);

    /* Display the mm results in a compact table */
    if (verbose) {
        if (onetime_flag) {
            assert(tracefiles != NULL);
            bool ok = mm_stats[num_tracefiles - 1].valid;
            printf("%s: tracefile \"%s\": mm malloc behaves %scorrectly.\n",
                   ok ? "ok" : "FAIL", tracefiles[num_tracefiles - 1],
                   ok ? "" : "in");
        } else {
            puts("\nResults for mm malloc:");
#if !defined DEBUG
            if (!sparse_mode)
                printresults(num_tracefiles, mm_stats, &mm_sum_stats);
            else
                printresultssparse(num_tracefiles, mm_stats, &mm_sum_stats);
#else
            printresultsdbg(num_tracefiles, mm_stats, &mm_sum_stats);
#endif
        }
    }

    /* Optionally compare the performance of mm and libc */
    if (run_libc) {
        printf("Comparison with libc malloc: mm/libc = %.0f Kops / %.0f Kops = "
               "%.2f\n",
               (float)mm_sum_stats.tput, (float)libc_sum_stats.tput,
               (float)(mm_sum_stats.tput / libc_sum_stats.tput));
    }

    /* temporaries used to compute the performance index */
    double avg_mm_util = 0.0;
    double avg_mm_harm_throughput = 0.0;
    double p1, p1_checkpoint; // util index
    double p2, p2_checkpoint; // throughput index
    double perfindex, perfindex_checkpoint;
    int util_weight = 0;
    int perf_weight = 0;

    /*
     * Accumulate the aggregate statistics for the student's mm package
     */
    double util = 0.0;
    double tput_harm = 0.0;
    int numcorrect = 0;

    /*
     * trace weight:
     * weight 0 => ignore
     *        1 => count both util and perf
     *        2 => count only util
     *        3 => count only perf
     */
    for (size_t i = 0; i < num_tracefiles; i++) {
        if (mm_stats[i].weight == WALL || mm_stats[i].weight == WPERF) {
            tput_harm += 1. / mm_stats[i].tput;
            perf_weight++;
        }
        if (mm_stats[i].weight == WALL || mm_stats[i].weight == WUTIL) {
            util += mm_stats[i].util;
            util_weight++;
        }
        if (mm_stats[i].valid) {
            numcorrect++;
        }
    }

    /*
     * Calculate harmonic mean of throughput.
     */
    tput_harm = (float)perf_weight / tput_harm;

    if (util_weight == 0) {
        avg_mm_util = 0.0;
    } else {
        avg_mm_util = util / util_weight;
    }
    if (sparse_mode || perf_weight == 0) {
        avg_mm_harm_throughput = 0.0;
    } else {
        /* Measure in KOPS */
        avg_mm_harm_throughput = tput_harm;
    }

    /*
     * Compute and print the performance index
     */
    if (errors > 0) {
        perfindex = 0.0;
        perfindex_checkpoint = 0.0;
        printf("Terminated with %d errors\n", errors);

    } else if (num_tracefiles > 1) {
        /* p1 - utilization */
        p1 = UTIL_WEIGHT *
             compute_scaled_score(avg_mm_util, MIN_SPACE, MAX_SPACE);
        p1_checkpoint = UTIL_WEIGHT_CHECKPOINT *
                        compute_scaled_score(avg_mm_util, MIN_SPACE_CHECKPOINT,
                                             MAX_SPACE_CHECKPOINT);

        /* p2 - throughput index */
        p2 = (1.0 - UTIL_WEIGHT) * compute_scaled_score(avg_mm_harm_throughput,
                                                        min_throughput,
                                                        max_throughput);
        p2_checkpoint = (1.0 - UTIL_WEIGHT_CHECKPOINT) *
                        compute_scaled_score(avg_mm_harm_throughput,
                                             min_throughput, max_throughput);

        perfindex = (p1 + p2) * 100.0;
        perfindex_checkpoint = (p1_checkpoint + p2_checkpoint) * 100.0;

#if REF_ONLY
        printf("%.0f\n", avg_mm_harm_throughput);
#elif !defined DEBUG /* !REF_ONLY && !DEBUG */
        printf("Arithmetic mean utilization = %.1f%%.\n", avg_mm_util * 100);

        // Don't measure throughput in sparse mode
        if (!sparse_mode) {
            printf("Harmonic mean throughput (Kops/sec) = %.0f.\n",
                   avg_mm_harm_throughput);
            if (checkpoint) {
                printf("Checkpoint Perf index = %.1f (util) + %.1f (thru) = "
                       "%.1f/100\n",
                       p1_checkpoint * 100, p2_checkpoint * 100,
                       perfindex_checkpoint);
            } else {
                printf("Perf index = %.1f (util) + %.1f (thru) = %.1f/100\n",
                       p1 * 100, p2 * 100, perfindex);
            }
        }
#endif
    }

    double spaceDeduct = 0.0;
#if SPARSE_MODE
    size_t globalUsage = queryGlobalSpaceUsage();
    if (globalUsage > 128) {
        spaceDeduct = (double)(globalUsage - 128) / 8.0;
        if (spaceDeduct > 20)
            spaceDeduct = 20;
        printf("128-byte global data limit exceeded (using %lu bytes), "
               "deducting %.2f points!\n",
               globalUsage, spaceDeduct);
        // Make spaceDeduct negative here, instead of unconditionally
        // negating it in the printf call below, so that we will not
        // print -0 in the case where it was originally zero.
        spaceDeduct = -spaceDeduct;
    }
#endif

    /* Optionally emit autoresult string */
    if (autograder) {
        double score = checkpoint ? perfindex_checkpoint : perfindex;
        // Scoreboard shows: score, deductions, throughput, utilization
        // note: at this point spaceDeduct <= 0

        printf("{\"scores\": {\"Autograded Score\": %.1f}, \"scoreboard\": "
               "[%.1f, %.0f, %.0f, %.1f]}\n",
               score + spaceDeduct, score, spaceDeduct, avg_mm_harm_throughput,
               avg_mm_util * 100);
    }

    return 0;
}

/*****************************************************************
 * Add trace to global list of tracefiles
 ****************************************************************/
static void add_tracefile(char ***tracefiles_p, size_t *num_tracefiles_p,
                          const char *tracedir, const char *trace) {
    char **tracefiles = *tracefiles_p;
    size_t num_tracefiles = *num_tracefiles_p;

    tracefiles = realloc(tracefiles, (num_tracefiles + 1) * sizeof(char *));
    if (!tracefiles) {
        unix_error("realloc in add_tracefile failed");
    }

    if (asprintf(&tracefiles[num_tracefiles++], "%s%s", tracedir, trace) ==
        -1) {
        unix_error("asprintf in add_tracefile failed");
    }

    *tracefiles_p = tracefiles;
    *num_tracefiles_p = num_tracefiles;
}

/*****************************************************************
 * The following routines manipulate the range list, which keeps
 * track of the extent of every allocated block payload. We use the
 * range list to detect any overlapping allocated blocks.
 ****************************************************************/

/*
 * new_range_set - Create an empty range set
 */
static range_set_t *new_range_set(void) {
    range_set_t *ranges = malloc(sizeof(range_set_t));
    ranges->list = NULL;
    ranges->lo_tree = tree_new();
    return ranges;
}

/*
 * add_range - As directed by request opnum in trace tracenum,
 *     we've just called the student's mm_malloc to allocate a block of
 *     size bytes at addr lo. After checking the block for correctness,
 *     we create a range struct for this block and add it to the range list.
 */
static bool add_range(range_set_t *ranges, char *lo, size_t size,
                      const trace_t *trace, unsigned int opnum,
                      unsigned int index) {
    char *hi = lo + size - 1;

    assert(size > 0);

    /* Payload addresses must be ALIGNMENT-byte aligned */
    if (!IS_ALIGNED(lo)) {
        malloc_error(trace, opnum,
                     "Payload address (%p) not aligned to %d bytes", (void *)lo,
                     ALIGNMENT);
        return false;
    }

    /* The payload must lie within the extent of the heap */
    if ((lo < (char *)mem_heap_lo()) || (lo > (char *)mem_heap_hi()) ||
        (hi < (char *)mem_heap_lo()) || (hi > (char *)mem_heap_hi())) {
        malloc_error(trace, opnum, "Payload (%p:%p) lies outside heap (%p:%p)",
                     (void *)lo, (void *)hi, (void *)mem_heap_lo(),
                     (void *)mem_heap_hi());
        return false;
    }

    /* If we can't afford the linear-time loop, we check less thoroughly and
       just assume the overlap will be caught by writing random bits. */
    if (debug_mode == DBG_NONE)
        return 1;

    /* Look in the tree for the predecessor block */
    range_t *prev = tree_find_nearest(ranges->lo_tree, (tkey_t)lo);
    range_t *next = prev ? prev->next : NULL;
    /* See if it overlaps previous or next blocks */
    if (prev && lo <= prev->hi) {
        malloc_error(
            trace, opnum, "Payload (%p:%p) overlaps another payload (%p:%p)",
            (void *)lo, (void *)hi, (void *)prev->lo, (void *)prev->hi);
        return false;
    }
    if (next && hi >= next->lo) {
        malloc_error(
            trace, opnum, "Payload (%p:%p) overlaps another payload (%p:%p)",
            (void *)lo, (void *)hi, (void *)prev->lo, (void *)prev->hi);
        return false;
    }
    /*
     * Everything looks OK, so remember the extent of this block
     * by creating a range struct and adding it the range list.
     */
    range_t *p;
    if ((p = malloc(sizeof(range_t))) == NULL)
        unix_error("malloc error in add_range");
    p->prev = prev;
    if (prev)
        prev->next = p;
    else
        ranges->list = p;
    p->next = next;
    if (next)
        next->prev = p;
    p->lo = lo;
    p->hi = hi;
    p->index = index;
    tree_insert(ranges->lo_tree, (tkey_t)lo, (void *)p);
    return true;
}

/*
 * remove_range - Free the range record of block whose payload starts at lo
 */
static void remove_range(range_set_t *ranges, char *lo) {
    range_t *p = (range_t *)tree_remove(ranges->lo_tree, (tkey_t)lo);
    if (!p)
        return;
    range_t *prev = p->prev;
    range_t *next = p->next;
    if (prev)
        prev->next = next;
    else
        ranges->list = next;
    if (next)
        next->prev = prev;
    free(p);
}

/*
 * free_range_set - free all of the range records for a trace
 */
static void free_range_set(range_set_t *ranges) {
    tree_free(ranges->lo_tree, free);
    free(ranges);
}

/**********************************************
 * The following routines handle the random data used for
 * checking memory access.
 *********************************************/

static void init_random_data(void) {
    int len;

    if (debug_mode == DBG_NONE)
        return;

    for (len = 0; len < RANDOM_DATA_LEN; ++len) {
        random_data[len] = (unsigned char)random();
    }
}

static void randomize_block(trace_t *traces, unsigned int index) {
    size_t size, fsize;
    size_t i;
    randint_t *block;
    size_t base;

    if (debug_mode == DBG_NONE)
        return;

    traces->block_rand_base[index] = (size_t)random();

    block = (randint_t *)traces->blocks[index];
    size = traces->block_sizes[index] / sizeof(*block);
    if (size == 0)
        return;
    fsize = size;
    if (fsize > maxfill)
        fsize = maxfill;
    base = traces->block_rand_base[index];

    // NOTE: It's expensive to do this one byte at a time.

    // NOTE: It would be nice to also fill in at end of block, but
    // this gets messy with REALLOC

    for (i = 0; i < fsize; i++) {
        mem_write(&block[i], random_data[(base + i) % RANDOM_DATA_LEN],
                  sizeof(randint_t));
    }

#ifdef USE_MSAN
    /* Mark payload data as uninitialized */
    __msan_allocated_memory(traces->blocks[index], traces->block_sizes[index]);
#endif
}

static bool check_index(const trace_t *trace, unsigned int opnum,
                        unsigned int index) {
    size_t size, fsize;
    size_t i;
    randint_t *block;
    size_t base;
    int ngarbled = 0;
    size_t firstgarbled = (size_t)-1;

    if (index == (unsigned int)-1)
        return true; /* we're doing free(NULL) */
    if (debug_mode == DBG_NONE)
        return true;

    block = (randint_t *)trace->blocks[index];
    size = trace->block_sizes[index] / sizeof(*block);
    if (size == 0)
        return true;
    fsize = size;

    size_t thresh = maxfill;
    if (fsize > thresh)
        fsize = thresh;

    base = trace->block_rand_base[index];

#ifdef USE_MSAN
    /* Mark memory as initialized so the following won't cause an error */
    __msan_unpoison(trace->blocks[index], trace->block_sizes[index]);
#endif

    // NOTE: It's expensive to do this one byte at a time.
    setUBCheck(false);
    for (i = 0; i < fsize; i++) {
        if (mem_read(&block[i], sizeof(randint_t)) !=
            random_data[(base + i) % RANDOM_DATA_LEN]) {
            if (firstgarbled == (size_t)-1)
                firstgarbled = i;
            ngarbled++;
        }
    }
    setUBCheck(true);
    if (ngarbled != 0) {
        malloc_error(trace, opnum,
                     "block %d (at %p) has %d garbled %s%s, "
                     "starting at byte %zu",
                     index, (void *)&block[firstgarbled], ngarbled,
                     randint_t_name, (ngarbled > 1 ? "s" : ""),
                     sizeof(randint_t) * firstgarbled);
        return false;
    }
    return true;
}

/**********************************************************************
 * The following functions evaluate the correctness, space utilization,
 * and throughput of the libc and mm malloc packages.
 **********************************************************************/

/*
 * eval_mm_valid - Check the mm malloc package for correctness
 */
static bool eval_mm_valid(trace_t *trace, range_set_t *ranges) {
    unsigned int i;
    unsigned int index;
    size_t size;
    char *newp;
    char *oldp;
    char *p;
    bool allCheck = true;

    /* Reset the heap and free any records in the range list */
    mem_reset_brk();
    reinit_trace(trace);

    /* Call the mm package's init function */
    if (!mm_init()) {
        malloc_error(trace, 0, "mm_init failed");
        return false;
    }

    /* Interpret each operation in the trace in order */
    for (i = 0; i < trace->num_ops; i++) {
        index = trace->ops[i].index;
        size = trace->ops[i].size;
		trace_line = i;

        if (debug_mode == DBG_EXPENSIVE) {
            range_t *r;

            /* Let the students check their own heap */
            if (!mm_checkheap(0)) {
                malloc_error(trace, i, "mm_checkheap returned false");
                return false;
            };

            /* Now check that all our allocated blocks have the right data */
            r = ranges->list;
            while (r) {
                if (!check_index(trace, i, r->index)) {
                    allCheck = false;
                }
                r = r->next;
            }
        }

        switch (trace->ops[i].type) {

        case ALLOC: /* mm_malloc */

            /* Call the student's malloc */
            if ((p = mm_malloc(size)) == NULL) {
                malloc_error(trace, i, "mm_malloc failed");
                return false;
            }

            /*
             * Test the range of the new block for correctness and add it
             * to the range list if OK. The block must be  be aligned properly,
             * and must not overlap any currently allocated block.
             */
            if (add_range(ranges, p, size, trace, i, index) == 0)
                return false;

            /* Remember region */
            trace->blocks[index] = p;
            trace->block_sizes[index] = size;

            /* Set to random data, for debugging. */
            randomize_block(trace, index);
            break;

        case REALLOC: /* mm_realloc */
            if (!check_index(trace, i, index)) {
                allCheck = false;
            }

            /* Call the student's realloc */
            oldp = trace->blocks[index];
            setUBCheck(false);
            newp = mm_realloc(oldp, size);
            setUBCheck(true);
            if ((newp == NULL) && (size != 0)) {
                malloc_error(trace, i, "mm_realloc failed");
                return false;
            }
            if ((newp != NULL) && (size == 0)) {
                malloc_error(trace, i,
                             "mm_realloc with size 0 returned "
                             "non-NULL");
                return false;
            }

            /* Remove the old region from the range list */
            remove_range(ranges, oldp);

            /* Check new block for correctness and add it to range list */
            if (size > 0) {
                if (add_range(ranges, newp, size, trace, i, index) == 0)
                    return false;
            }

            /* Move the region from where it was.
             * Check up to min(size, oldsize) for correct copying. */
            trace->blocks[index] = newp;
            if (size < trace->block_sizes[index]) {
                trace->block_sizes[index] = size;
            }
            // NOTE: Might help to pass old size here to check bytes at each end
            // of allocation

            if (!check_index(trace, i, index)) {
                allCheck = false;
            }
            trace->block_sizes[index] = size;

            /* Set to random data, for debugging. */
            randomize_block(trace, index);
            break;

        case FREE: /* mm_free */
            if (!check_index(trace, i, index)) {
                allCheck = false;
            }

            /* Remove region from list and call student's free function */
            if (index == (unsigned int)-1) {
                p = 0;
            } else {
                p = trace->blocks[index];
                remove_range(ranges, p);
            }
            mm_free(p);
            break;

        default:
            app_error("Invalid request type in eval_mm_valid");
        }
    }
    /* As far as we know, this is a valid malloc package */
    return allCheck;
}

/*
 * eval_mm_util - Evaluate the space utilization of the student's package
 *   The idea is to remember the high water mark "hwm" of the heap for
 *   an optimal allocator, i.e., no gaps and no internal fragmentation.
 *   Utilization is the ratio hwm/heapsize, where heapsize is the
 *   size of the heap in bytes after running the student's malloc
 *   package on the trace. Note that our implementation of mem_sbrk()
 *   doesn't allow the students to decrement the brk pointer, so brk
 *   is always the high water mark of the heap.
 *
 *   A higher number is better: 1 is optimal.
 */
static double eval_mm_util(trace_t *trace, size_t tracenum) {
    unsigned int i;
    unsigned int index;
    size_t size, newsize, oldsize;
    size_t max_total_size = 0;
    size_t total_size = 0;
    char *p;
    char *newp, *oldp;

    reinit_trace(trace);

    /* initialize the heap and the mm malloc package */
    mem_reset_brk();
    if (!mm_init())
        app_error("trace %zd: mm_init failed in eval_mm_util", tracenum);

    for (i = 0; i < trace->num_ops; i++) {
		trace_line = i;
        switch (trace->ops[i].type) {

        case ALLOC: /* mm_alloc */
            index = trace->ops[i].index;
            size = trace->ops[i].size;

            if ((p = mm_malloc(size)) == NULL) {
                app_error("trace %zd: mm_malloc failed in eval_mm_util",
                          tracenum);
            }

            /* Remember region and size */
            trace->blocks[index] = p;
            trace->block_sizes[index] = size;

            total_size += size;
            break;

        case REALLOC: /* mm_realloc */
            index = trace->ops[i].index;
            newsize = trace->ops[i].size;
            oldsize = trace->block_sizes[index];

            oldp = trace->blocks[index];
            setUBCheck(false);
            if ((newp = mm_realloc(oldp, newsize)) == NULL && newsize != 0) {
                app_error("trace %zd: mm_realloc failed in eval_mm_util",
                          tracenum);
            }
            setUBCheck(true);

            /* Remember region and size */
            trace->blocks[index] = newp;
            trace->block_sizes[index] = newsize;

            total_size += (newsize - oldsize);
            break;

        case FREE: /* mm_free */
            index = trace->ops[i].index;
            if (index == (unsigned int)-1) {
                size = 0;
                p = 0;
            } else {
                size = trace->block_sizes[index];
                p = trace->blocks[index];
            }

            mm_free(p);

            total_size -= size;
            break;

        default:
            app_error("trace %zd: Nonexistent request type in eval_mm_util",
                      tracenum);
        }

        /* update the high-water mark */
        max_total_size =
            (total_size > max_total_size) ? total_size : max_total_size;
    }

    return ((double)max_total_size / (double)mem_heapsize());
}

/*
 * eval_mm_speed - This is the function that is used by fcyc()
 *    to measure the running time of the mm malloc package.
 */
static void eval_mm_speed(void *ptr) {
    unsigned int i, index;
    size_t size, newsize;
    char *p, *newp, *oldp, *block;
    trace_t *trace = ((speed_t *)ptr)->trace;
    reinit_trace(trace);

    /* Reset the heap and initialize the mm package */
    mem_reset_brk();
    if (!mm_init())
        app_error("mm_init failed in eval_mm_speed");

    /* Interpret each trace request */
    for (i = 0; i < trace->num_ops; i++) {
		trace_line = i;
        switch (trace->ops[i].type) {

        case ALLOC: /* mm_malloc */
            index = trace->ops[i].index;
            size = trace->ops[i].size;
            if ((p = mm_malloc(size)) == NULL)
                app_error("mm_malloc error in eval_mm_speed");
            trace->blocks[index] = p;
            break;

        case REALLOC: /* mm_realloc */
            index = trace->ops[i].index;
            newsize = trace->ops[i].size;
            oldp = trace->blocks[index];
            setUBCheck(false);
            if ((newp = mm_realloc(oldp, newsize)) == NULL && newsize != 0)
                app_error("mm_realloc error in eval_mm_speed");
            setUBCheck(true);
            trace->blocks[index] = newp;
            break;

        case FREE: /* mm_free */
            index = trace->ops[i].index;
            if (index == (unsigned int)-1) {
                block = 0;
            } else {
                block = trace->blocks[index];
            }
            mm_free(block);
            break;

        default:
            app_error("Nonexistent request type in eval_mm_speed");
        }
	}
}

/*
 * eval_libc_valid - We run this function to make sure that the
 *    libc malloc can run to completion on the set of traces.
 *    We'll be conservative and terminate if any libc malloc call fails.
 *
 */
static bool eval_libc_valid(trace_t *trace) {
    unsigned int i;
    size_t newsize;
    char *p, *newp, *oldp;

    reinit_trace(trace);

    for (i = 0; i < trace->num_ops; i++) {
        switch (trace->ops[i].type) {

        case ALLOC: /* malloc */
            if ((p = malloc(trace->ops[i].size)) == NULL) {
                malloc_error(trace, i, "libc malloc failed: %s",
                             strerror(errno));
            }
            trace->blocks[trace->ops[i].index] = p;
            break;

        case REALLOC: /* realloc */
            newsize = trace->ops[i].size;
            oldp = trace->blocks[trace->ops[i].index];
            if ((newp = realloc(oldp, newsize)) == NULL && newsize != 0) {
                malloc_error(trace, i, "libc realloc failed: %s",
                             strerror(errno));
            }
            trace->blocks[trace->ops[i].index] = newp;
            break;

        case FREE: /* free */
            if (trace->ops[i].index != (unsigned int)-1) {
                free(trace->blocks[trace->ops[i].index]);
            } else {
                free(0);
            }
            break;

        default:
            app_error("invalid operation type  in eval_libc_valid");
        }
    }

    return true;
}

/*
 * eval_libc_speed - This is the function that is used by fcyc() to
 *    measure the running time of the libc malloc package on the set
 *    of traces.
 */
static void eval_libc_speed(void *ptr) {
    unsigned int i;
    unsigned int index;
    size_t size, newsize;
    char *p, *newp, *oldp, *block;
    trace_t *trace = ((speed_t *)ptr)->trace;

    reinit_trace(trace);

    for (i = 0; i < trace->num_ops; i++) {
        switch (trace->ops[i].type) {
        case ALLOC: /* malloc */
            index = trace->ops[i].index;
            size = trace->ops[i].size;
            if ((p = malloc(size)) == NULL)
                unix_error("malloc failed in eval_libc_speed");
            trace->blocks[index] = p;
            break;

        case REALLOC: /* realloc */
            index = trace->ops[i].index;
            newsize = trace->ops[i].size;
            oldp = trace->blocks[index];
            if ((newp = realloc(oldp, newsize)) == NULL && newsize != 0)
                unix_error("realloc failed in eval_libc_speed");

            trace->blocks[index] = newp;
            break;

        case FREE: /* free */
            index = trace->ops[i].index;
            if (index != (unsigned int)-1) {
                block = trace->blocks[index];
                free(block);
            } else {
                free(0);
            }
            break;
        }
    }
}

/*************************************
 * Some miscellaneous helper routines
 ************************************/

/*
 * printresults - prints a performance summary for some malloc package and
 * returns a summary of the stats to the caller.
 */
static void printresults(size_t n, stats_t *stats, sum_stats_t *sumstats) {
    size_t i;

    /* weighted sums all */
    double sumsecs = 0;
    double sumops = 0;
    double sumtput = 0;
    double sumutil = 0;
    int sum_perf_weight = 0;
    int sum_util_weight = 0;

    char wstr;
    const char *tabstr;

    /* Print the individual results for each trace */
    if (tab_mode) {
        printf("valid\tthru?\tutil?\tutil\tops\tmsecs\tKops/s\ttrace\n");
    } else {
        printf("  %5s  %6s %7s%8s%8s  %s\n", "valid", "util", "ops", "msecs",
               "Kops/s", "trace");
    }
    for (i = 0; i < n; i++) {
        if (stats[i].valid) {
            switch (stats[i].weight) {
            case WNONE:
                wstr = ' ';
                tabstr = "0\t0\t";
                break;
            case WALL:
                wstr = '*';
                tabstr = "1\t1\t";
                break;
            case WUTIL:
                wstr = 'u';
                tabstr = "0\t1\t";
                break;
            case WPERF:
                wstr = 'p';
                tabstr = "1\t0\t";
                break;
            default:
                app_error("wrong value for weight found!");
            }

            /* Valid = whether performance and/or throughput counted */
            if (tab_mode) {
                printf("1\t%s", tabstr);
            } else {
                /* prints done in a somewhat silly way to avoid hassle
                 * if future columns need to be added/modified like this time */
                printf("%2c", wstr);
                printf("%4s", "yes");
            }

            /* Utilization */
            if (tab_mode) {
                printf("%.1f\t", stats[i].util * 100.0);
            } else {
                /* print '--' if util isn't weighted */
                if (stats[i].weight == WNONE || stats[i].weight == WALL ||
                    stats[i].weight == WUTIL)
                    printf(" %7.1f%%", stats[i].util * 100.0);
                else
                    printf(" %8s", "--");
            }

            /* Ops + Time */
            double msecs = sparse_mode ? 0.0 : stats[i].secs * 1000.0;
            double kops = sparse_mode ? 0.0 : stats[i].tput;
            if (tab_mode) {
                printf("%u\t%.3f\t%.0f\t", stats[i].ops, msecs, kops);
            } else {
                /* print '--' if perf isn't weighted */
                if (stats[i].weight == WNONE || stats[i].weight == WALL ||
                    stats[i].weight == WPERF)
                    printf("%8u%10.3f%7.0f ", stats[i].ops, msecs, kops);
                else
                    printf("%8s%10s%7s ", "--", "--", "--");
            }

            printf("%s\n", stats[i].filename);

            if (stats[i].weight == WALL || stats[i].weight == WPERF) {
                sum_perf_weight += 1;
                sumsecs += stats[i].secs;
                sumops += stats[i].ops;
                sumtput += stats[i].tput;
            }
            if (stats[i].weight == WALL || stats[i].weight == WUTIL) {
                sum_util_weight += 1;
                sumutil += stats[i].util;
            }
        } else {
            if (tab_mode) {
                printf("no\t\t\t\t\t\t\t%s\n", stats[i].filename);
            } else {
                printf("%2s%4s%7s%10s%7s%10s %s\n",
                       stats[i].weight != 0 ? "*" : "", "no", "-", "-", "-",
                       "-", stats[i].filename);
            }
        }
    }

    /* Print the aggregate results for the set of traces.  Record the
       summary statistics so we can compare libc and mm.cc.  */
    if (sum_perf_weight == 0 && sum_util_weight == 0) {
        sumstats->util = 0;
        sumstats->ops = 0;
        sumstats->secs = 0;
        sumstats->tput = 0;
    } else if (errors > 0) {
        if (!tab_mode) {
            printf("     %8s%10s%7s\n", "-", "-", "-");
        }
        sumstats->util = 0;
        sumstats->ops = 0;
        sumstats->secs = 0;
        sumstats->tput = 0;
    } else {
        if (sum_perf_weight == 0)
            sum_perf_weight = 1;
        if (sum_util_weight == 0)
            sum_util_weight = 1;

        double util = sumutil / (double)sum_util_weight;
        double tput = sparse_mode ? 0.0 : sumtput / (double)sum_perf_weight;
        if (sparse_mode)
            sumsecs = 0;
        if (tab_mode) {
            // "valid\tthru?\tutil?\tutil\tops\tmsecs\tKops\ttrace"
            printf("Sum\t%d\t%d\t%.1f\t%.0f\t%.2f\n", sum_perf_weight,
                   sum_util_weight, sumutil * 100.0, sumops, sumsecs * 1000.0);
            printf("Avg\t\t\t%.1f\t\t\t\n", util * 100.0);
        } else {
            printf("%2d %2d  %7.1f%%%8.0f%10.3f\n", sum_util_weight,
                   sum_perf_weight, util * 100.0, sumops, sumsecs * 1000.0);
        }

        sumstats->util = util;
        sumstats->ops = sumops;
        sumstats->secs = sumsecs;
        sumstats->tput = tput;
    }
}

/*
 * printresultsdbg - prints a correctness summary (without performance) for some malloc package and
 * returns a summary of the stats to the caller.
 */
 static void printresultsdbg(size_t n, stats_t *stats, sum_stats_t *sumstats) {
    size_t i;

    char wstr;
    const char *tabstr;

    /* Print the individual results for each trace */
    if (tab_mode) {
        printf("valid\ttrace\n");
    } else {
        printf("  %5s  %s\n", "valid", "trace");
    }
    for (i = 0; i < n; i++) {
        if (stats[i].valid) {
            switch (stats[i].weight) {
            case WNONE:
                wstr = ' ';
                tabstr = "0\t0\t";
                break;
            case WALL:
                wstr = '*';
                tabstr = "1\t1\t";
                break;
            case WUTIL:
                wstr = 'u';
                tabstr = "0\t1\t";
                break;
            case WPERF:
                wstr = 'p';
                tabstr = "1\t0\t";
                break;
            default:
                app_error("wrong value for weight found!");
            }

            /* Valid = whether performance and/or throughput counted */
            if (tab_mode) {
                printf("1\t%s", tabstr);
            } else {
                /* prints done in a somewhat silly way to avoid hassle
                 * if future columns need to be added/modified like this time */
                printf("%2c", wstr);
                printf("%4s", "yes");
            }

            printf("\t%s\n", stats[i].filename);

        } else {
            if (tab_mode) {
                printf("no\t%s\n", stats[i].filename);
            } else {
                printf("%2s%4s %s\n",
                       stats[i].weight != 0 ? "*" : "", "no", stats[i].filename);
            }
        }
    }
}

/*
 * printresultssparse - prints a performance summary without throughput for some malloc package and
 * returns a summary of the stats to the caller. Used for emulate.
 */
 static void printresultssparse(size_t n, stats_t *stats, sum_stats_t *sumstats) {
    size_t i;

    /* weighted sums all */
    double sumsecs = 0;
    double sumops = 0;
    double sumtput = 0;
    double sumutil = 0;
    int sum_perf_weight = 0;
    int sum_util_weight = 0;

    char wstr;
    const char *tabstr;

    /* Print the individual results for each trace */
    if (tab_mode) {
        printf("valid\tthru?\tutil?\tutil\tops\ttrace\n");
    } else {
        printf("  %5s  %6s %7s  %s\n", "valid", "util", "ops", "trace");
    }
    for (i = 0; i < n; i++) {
        if (stats[i].valid) {
            switch (stats[i].weight) {
            case WNONE:
                wstr = ' ';
                tabstr = "0\t0\t";
                break;
            case WALL:
                wstr = '*';
                tabstr = "1\t1\t";
                break;
            case WUTIL:
                wstr = 'u';
                tabstr = "0\t1\t";
                break;
            case WPERF:
                wstr = 'p';
                tabstr = "1\t0\t";
                break;
            default:
                app_error("wrong value for weight found!");
            }

            /* Valid = whether performance and/or throughput counted */
            if (tab_mode) {
                printf("1\t%s", tabstr);
            } else {
                /* prints done in a somewhat silly way to avoid hassle
                 * if future columns need to be added/modified like this time */
                printf("%2c", wstr);
                printf("%4s", "yes");
            }

            /* Utilization */
            if (tab_mode) {
                printf("%.1f\t", stats[i].util * 100.0);
            } else {
                /* print '--' if util isn't weighted */
                if (stats[i].weight == WNONE || stats[i].weight == WALL ||
                    stats[i].weight == WUTIL)
                    printf(" %7.1f%%", stats[i].util * 100.0);
                else
                    printf(" %8s", "--");
            }

            /* Ops */
            if (tab_mode) {
                printf("%u\t", stats[i].ops);
            } else {
                /* print '--' if perf isn't weighted */
                if (stats[i].weight == WNONE || stats[i].weight == WALL ||
                    stats[i].weight == WPERF)
                    printf("%8u ", stats[i].ops);
                else
                    printf("%8s ", "--");
            }

            printf("%s\n", stats[i].filename);

            if (stats[i].weight == WALL || stats[i].weight == WPERF) {
                sum_perf_weight += 1;
                sumsecs += stats[i].secs;
                sumops += stats[i].ops;
                sumtput += stats[i].tput;
            }
            if (stats[i].weight == WALL || stats[i].weight == WUTIL) {
                sum_util_weight += 1;
                sumutil += stats[i].util;
            }
        } else {
            if (tab_mode) {
                printf("no\t\t\t\t\t%s\n", stats[i].filename);
            } else {
                printf("%2s%4s%7s%10s %s\n",
                       stats[i].weight != 0 ? "*" : "", "no", "-", "-", stats[i].filename);
            }
        }
    }

    /* Print the aggregate results for the set of traces.  Record the
       summary statistics so we can compare libc and mm.cc.  */
    if (sum_perf_weight == 0 && sum_util_weight == 0) {
        sumstats->util = 0;
        sumstats->ops = 0;
        sumstats->secs = 0;
        sumstats->tput = 0;
    } else if (errors > 0) {
        if (!tab_mode) {
            printf("     %8s\n", "-");
        }
        sumstats->util = 0;
        sumstats->ops = 0;
        sumstats->secs = 0;
        sumstats->tput = 0;
    } else {
        if (sum_perf_weight == 0)
            sum_perf_weight = 1;
        if (sum_util_weight == 0)
            sum_util_weight = 1;

        double util = sumutil / (double)sum_util_weight;
        double tput = sparse_mode ? 0.0 : sumtput / (double)sum_perf_weight;
        if (sparse_mode)
            sumsecs = 0;
        if (tab_mode) {
            // "valid\tthru?\tutil?\tutil\tops\tmsecs\tKops\ttrace"
            printf("Sum\t%d\t%d\t%.1f\t%.0f\n", sum_perf_weight,
                   sum_util_weight, sumutil * 100.0, sumops);
            printf("Avg\t\t\t%.1f\t\t\t\n", util * 100.0);
        } else {
            printf("%2d %2d  %7.1f%%%8.0f\n", sum_util_weight,
                   sum_perf_weight, util * 100.0, sumops);
        }

        sumstats->util = util;
        sumstats->ops = sumops;
        sumstats->secs = sumsecs;
        sumstats->tput = tput;
    }
}

/*
 * app_error - Report an arbitrary application error
 */
void app_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    putc('\n', stderr);
    exit(1);
}

/*
 * unix_error - Report the error and its errno.
 */
void unix_error(const char *fmt, ...) {
    // Capture errno now, because vfprintf might clobber it.
    int err = errno;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, ": %s\n", strerror(err));
    exit(1);
}

/*
 * malloc_error - Report an error returned by the mm_malloc package
 */
void malloc_error(const trace_t *trace, unsigned int opnum, const char *fmt,
                  ...) {

    errors++;
    fprintf(stderr, "ERROR [trace %s, line %d]: ", trace->filename,
            trace->ops[opnum].lineno);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    putc('\n', stderr);
}

/*
 * compute_scaled_score: Scales a raw score in the range from lo to hi to the
 * range from 0.0 to 1.0. In other words, a raw score of lo returns 0.0, a raw
 * score of hi returns 1.0, and anything in between is interpolated.
 */
static double compute_scaled_score(double value, double lo, double hi) {
    if (value < lo)
        return 0.0;
    else if (value > hi)
        return 1.0;
    else
        return (value - lo) / (hi - lo);
}

/*****
 * Routines for reference throughput lookup
 *****/

/*
 * cparse.  Rewrite string by removing whitespace and replacing ':' with NULL.
 * Second argument filled with starting addresses of the resulting substrings
 * Return number of tokens generated
 */

#define PLIMIT 10
static int cparse(char *s, char **index) {
    int found = 0;
    bool done = false;
    char *last_start = s;
    char *rpos = s;
    char *wpos = s;
    while (found < PLIMIT && !done) {
        char c = *rpos++;
        switch (c) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            /* Skip */
            break;
        case ':':
            /* Finish off current token */
            *wpos++ = 0;
            index[found++] = last_start;
            last_start = wpos;
            break;
        case 0:
            /* End of string.  Finish off token */
            *wpos = 0;
            index[found++] = last_start;
            done = true;
            break;
        default:
            *wpos++ = c;
        }
    }
    return found;
}

/* Read throughput from file */
static double lookup_ref_throughput(bool checkpoint) {
    char buf[MAXLINE];
    char *tokens[PLIMIT];
    char cpu_type[MAXLINE] = "";
    double tput = 0.0;
    const char *bench_type = checkpoint ? BENCH_KEY_CHECKPOINT : BENCH_KEY;

    /* Scan file to find CPU type */
    FILE *ifile = fopen(CPU_FILE, "r");
    if (!ifile) {
        fprintf(stderr, "Warning: Could not find file '%s'\n", CPU_FILE);
        return tput;
    }
    /* Read lines in file.  Parse each one to look for key */
    bool found = false;
    while (fgets(buf, MAXLINE, ifile) != NULL) {
        int t = cparse(buf, tokens);
        if (t < 2)
            continue;
        if (strcmp(CPU_KEY, tokens[0]) == 0) {
            strcpy(cpu_type, tokens[1]);
            found = true;
            break;
        }
    }
    fclose(ifile);
    if (!found) {
        fprintf(stderr, "Warning: Could not find CPU type in file '%s'\n",
                CPU_FILE);
        return tput;
    }
    /* Now try to find matching entry in throughput file */
    FILE *tfile = fopen(THROUGHPUT_FILE, "r");
    if (tfile == NULL) {
        fprintf(stderr, "Warning: Could not open throughput file '%s'\n",
                THROUGHPUT_FILE);
        return tput;
    }
    while (fgets(buf, MAXLINE, tfile) != NULL) {
        int t = cparse(buf, tokens);
        if (t < 3)
            continue;
        if (strcmp(tokens[0], cpu_type) == 0 &&
            strcmp(tokens[1], bench_type) == 0) {
            tput = atof(tokens[2]);
            break;
        }
    }
    fclose(tfile);
    if (tput == 0.0) {
        fprintf(stderr,
                "Warning: Could not find CPU '%s' benchmark '%s' in throughput "
                "file '%s'\n",
                cpu_type, bench_type, THROUGHPUT_FILE);
    }
    if (tput > 0.0 && verbose > 0) {
        printf(
            "Found benchmark throughput %.0f for cpu type %s, benchmark %s\n",
            tput, cpu_type, bench_type);
    }
    return tput;
}

/*
 * measure_ref_throughput: Measure throughput achieved by reference
 * implementation
 */
static double measure_ref_throughput(bool checkpoint) {
    double tput = lookup_ref_throughput(checkpoint);
    if (tput > 0)
        return tput;

    const char *cmd = checkpoint ? REF_DRIVER_CHECKPOINT : REF_DRIVER;
    FILE *f = popen(cmd, "r");
    if (!f) {
        fprintf(stderr, "Couldn't execute '%s': %s\n", cmd, strerror(errno));
        exit(1);
    }
    if (fscanf(f, "%lf", &tput) != 1) {
        fprintf(stderr, "Couldn't read throughput from '%s'\n", cmd);
        exit(1);
    }
    while (getc(f) != EOF) {
        /* do nothing */
    }
    if (pclose(f) != 0) {
        fprintf(stderr, "Error in pipe from '%s'\n", cmd);
        exit(1);
    }
    return tput;
}

/*
 * atoui_or_usage - Parse ARG as an unsigned integer.  If it is
 * syntactically invalid or outside the range [0, UINT_MAX], print an
 * error and exit, otherwise return the value of ARG.  The error
 * message assumes that ARG came from the command line arguments.
 * OPTION should be the name of the option that ARG was an argument to.
 */
static unsigned int atoui_or_usage(const char *arg, const char *option,
                                   const char *prog) {
    errno = 0;
    char *endp;
    unsigned long val = strtoul(arg, &endp, 10);
    if (endp == arg || *endp != '\0' || val > UINT_MAX || errno) {
        fprintf(stderr, "%s: invalid argument to option '%s' -- '%s'\n", prog,
                option, arg);
        usage(prog);
        exit(1);
    }
    return (unsigned int)val;
}

/*
 * usage - Explain the command line arguments
 */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-hlVCdD] [-f <file>]\n", prog);
    fprintf(stderr, "Options\n");
    fprintf(stderr, "\t-C         Calculate Checkpoint Score.\n");
    fprintf(stderr, "\t-d <i>     Debug: 0 off; 1 default; 2 lots.\n");
    fprintf(stderr, "\t-D         Equivalent to -d2.\n");
    fprintf(stderr, "\t-c <file>  Run trace file <file> twice, check for "
                    "correctness only.\n");
    fprintf(stderr, "\t-t <dir>   Directory to find default traces.\n");
    fprintf(stderr, "\t-h         Print this message.\n");
    fprintf(stderr, "\t-l         Run libc malloc as well.\n");
    fprintf(stderr, "\t-V         Print diagnostics as each trace is run.\n");
    fprintf(stderr, "\t-v <i>     Set Verbosity Level to <i>\n");
    fprintf(stderr, "\t-s <s>     Timeout after s secs (default no timeout)\n");
    fprintf(stderr, "\t-T         Print diagnostics in tab mode\n");
    fprintf(stderr, "\t-f <file>  Use <file> as the trace file\n");
}
