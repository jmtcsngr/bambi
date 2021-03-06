/*  decode.c -- index decoder subcommand.

    Copyright (C) 2017 Genome Research Ltd.

    Author: Jennifer Liddle <js10@sanger.ac.uk>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published
by the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <bambi.h>
#include <assert.h>
#include <htslib/sam.h>
#include <string.h>
#include <getopt.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <unistd.h>
#include <regex.h>
#include <htslib/khash.h>
#include <cram/sam_header.h>
#include <inttypes.h>

#include "bamit.h"
#include "hash_table.h"

#define xstr(s) str(s)
#define str(s) #s

#define DEFAULT_MAX_LOW_QUALITY_TO_CONVERT 15
#define DEFAULT_MAX_NO_CALLS 2
#define DEFAULT_MAX_MISMATCHES 1
#define DEFAULT_MIN_MISMATCH_DELTA 1
#define DEFAULT_BARCODE_TAG "BC"
#define DEFAULT_QUALITY_TAG "QT"

enum match {
    MATCHED_NONE,
    MATCHED_FIRST,
    MATCHED_SECOND,
    MATCHED_BOTH,
    MATCHED_NEW
};


/*
 * structure to hold options
 */
typedef struct {
    char *input_name;
    char *output_name;
    char *barcode_name;
    char *metrics_name;
    char *barcode_tag_name;
    char *quality_tag_name;
    bool verbose;
    int max_low_quality_to_convert;
    bool convert_low_quality;
    int max_no_calls;
    int max_mismatches;
    int min_mismatch_delta;
    bool change_read_name;
    char *argv_list;
    char *input_fmt;
    char *output_fmt;
    char compression_level;
    int idx1_len, idx2_len;
    bool ignore_pf;
    unsigned short dual_tag;
} opts_t;

static void free_opts(opts_t* opts)
{
    if (!opts) return;
    free(opts->input_name);
    free(opts->output_name);
    free(opts->barcode_name);
    free(opts->barcode_tag_name);
    free(opts->quality_tag_name);
    free(opts->argv_list);
    free(opts->input_fmt);
    free(opts->output_fmt);
    free(opts->metrics_name);
    free(opts);
}

/*
 * details read from barcode file
 * Plus metrics information for each barcode
 */
typedef struct {
    char *seq, *idx1, *idx2;
    char *name;
    char *lib;
    char *sample;
    char *desc;
    uint64_t reads, pf_reads, perfect, pf_perfect, one_mismatch, pf_one_mismatch;
} bc_details_t;

/*
 * Print matrics file header
 */
static void print_header(FILE* f, opts_t* opts, bool metrics) {
    // print header
    fprintf(f, "##\n");
    fprintf(f, "# BARCODE_TAG_NAME=%s ", opts->barcode_tag_name);
    fprintf(f, "MAX_MISMATCHES=%d ", opts->max_mismatches);
    fprintf(f, "MIN_MISMATCH_DELTA=%d ", opts->min_mismatch_delta);
    fprintf(f, "MAX_NO_CALLS=%d ", opts->max_no_calls);
    fprintf(f, "\n");
    fprintf(f, "##\n");
    fprintf(f, "# ID:bambi VN:%s (htslib %s) CL:%s\n", bambi_version(), hts_version(), opts->argv_list);
    fprintf(f, "\n");
    fprintf(f, "##\n");
    fprintf(f, "BARCODE\t");
    if (metrics) {
        fprintf(f, "BARCODE_NAME\t");
        fprintf(f, "LIBRARY_NAME\t");
        fprintf(f, "SAMPLE_NAME\t");
        fprintf(f, "DESCRIPTION\t");
    }
    fprintf(f, "READS\t");
    if (!opts->ignore_pf) {
        fprintf(f, "PF_READS\t");
    }
    fprintf(f, "PERFECT_MATCHES\t");
    if (!opts->ignore_pf) {
        fprintf(f, "PF_PERFECT_MATCHES\t");
    }
    if (metrics) {
        fprintf(f, "ONE_MISMATCH_MATCHES\t");
        if (!opts->ignore_pf) {
            fprintf(f, "PF_ONE_MISMATCH_MATCHES\t");
        }
    }
    fprintf(f, "PCT_MATCHES\t");
    fprintf(f, "RATIO_THIS_BARCODE_TO_BEST_BARCODE_PCT");
    if (!opts->ignore_pf) {
        fprintf(f, "\tPF_PCT_MATCHES");
    }
    if (!opts->ignore_pf) {
        fprintf(f, "\tPF_RATIO_THIS_BARCODE_TO_BEST_BARCODE_PCT");
    }
    if (!opts->ignore_pf) {
        fprintf(f, "\tPF_NORMALIZED_MATCHES");
    }
    fprintf(f, "\n");
}

void free_bcd(void *entry)
{
    bc_details_t *bcd = (bc_details_t *)entry;
    free(bcd->seq);
    free(bcd->idx1);
    free(bcd->idx2);
    free(bcd->name);
    free(bcd->lib);
    free(bcd->sample);
    free(bcd->desc);
    free(bcd);
}

static int compareTagHops(const void *t1, const void *t2) {
    bc_details_t *th1 = *(bc_details_t **)t1;
    bc_details_t *th2 = *(bc_details_t **)t2;

    int read_diff = th1->reads - th2->reads;
    //if read count is equal, sort by number of perfect matches
    return read_diff ? -read_diff : -(th1->perfect - th2->perfect);
}

static void sortTagHops(va_t *tagHopArray) {
    qsort(tagHopArray->entries, tagHopArray->end, sizeof(bc_details_t*), compareTagHops);
}

static void freeRecord(void *r) { bam_destroy1((bam1_t *)r); }

/*
 * display usage information
 */
static void usage(FILE *write_to)
{
    fprintf(write_to,
"Usage: bambi decode [options] filename\n"
"\n"
"Options:\n"
"  -o   --output                        output file [default: stdout]\n"
"  -v   --verbose                       verbose output\n"
"  -b   --barcode-file                  file containing barcodes\n"
"       --convert-low-quality           Convert low quality bases in barcode read to 'N'\n"
"       --max-low-quality-to-convert    Max low quality phred value to convert bases in barcode\n"
"                                       read to 'N' [default: " xstr(DEFAULT_MAX_LOW_QUALITY_TO_CONVERT) "]\n"
"       --max-no-calls                  Max allowable number of no-calls in a barcode read before\n"
"                                       it is considered unmatchable [default: " xstr(DEFAULT_MAX_NO_CALLS) "]\n"
"       --max-mismatches                Maximum mismatches for a barcode to be considered a match\n"
"                                       [default: " xstr(DEFAULT_MAX_MISMATCHES) "]\n"
"       --min-mismatch-delta            Minimum difference between number of mismatches in the best\n"
"                                       and second best barcodes for a barcode to be considered a\n"
"                                       match [default: " xstr(DEFAULT_MIN_MISMATCH_DELTA) "]\n"
"       --change-read-name              Change the read name by adding #<barcode> suffix\n"
"       --metrics-file                  Per-barcode and per-lane metrics written to this file\n"
"       --barcode-tag-name              Barcode tag name [default: " DEFAULT_BARCODE_TAG "]\n"
"       --quality-tag-name              Quality tag name [default: " DEFAULT_QUALITY_TAG "]\n"
"       --input-fmt                     format of input file [sam/bam/cram]\n"
"       --output-fmt                    format of output file [sam/bam/cram]\n"
"       --compression-level             Compression level of output file [0..9]\n"
"       --ignore-pf                     Doesn't output PF statistics\n"
"       --dual-tag                      Dual tag position in the barcode string (between 2 and barcode length - 1)\n"
);
}

/*
 * Takes the command line options and turns them into something we can understand
 */
static opts_t* parse_args(int argc, char *argv[])
{
    if (argc == 1) { usage(stdout); return NULL; }

    const char* optstring = "i:o:vb:";

    static const struct option lopts[] = {
        { "input",                      1, 0, 'i' },
        { "output",                     1, 0, 'o' },
        { "verbose",                    0, 0, 'v' },
        { "max-low-quality-to-convert", 1, 0, 0 },
        { "convert-low-quality",        0, 0, 0 },
        { "barcode-file",               1, 0, 'b' },
        { "max-no-calls",               1, 0, 0 },
        { "max-mismatches",             1, 0, 0 },
        { "min-mismatch-delta",         1, 0, 0 },
        { "change-read-name",           0, 0, 0 },
        { "metrics-file",               1, 0, 0 },
        { "barcode-tag-name",           1, 0, 0 },
        { "quality-tag-name",           1, 0, 0 },
        { "input-fmt",                  1, 0, 0 },
        { "output-fmt",                 1, 0, 0 },
        { "compression-level",          1, 0, 0 },
        { "ignore-pf",                  0, 0, 0 },
        { "dual-tag",                   1, 0, 0 },
        { NULL, 0, NULL, 0 }
    };

    opts_t* opts = calloc(sizeof(opts_t), 1);
    if (!opts) { perror("cannot allocate option parsing memory"); return NULL; }

    opts->argv_list = stringify_argv(argc+1, argv-1);
    if (opts->argv_list[strlen(opts->argv_list)-1] == ' ') opts->argv_list[strlen(opts->argv_list)-1] = 0;

    // set defaults
    opts->max_low_quality_to_convert = DEFAULT_MAX_LOW_QUALITY_TO_CONVERT;
    opts->max_no_calls = DEFAULT_MAX_NO_CALLS;
    opts->max_mismatches = DEFAULT_MAX_MISMATCHES;
    opts->min_mismatch_delta = DEFAULT_MIN_MISMATCH_DELTA;
    opts->verbose = false;
    opts->convert_low_quality = false;
    opts->change_read_name = false;
    opts->barcode_tag_name = NULL;
    opts->quality_tag_name = NULL;
    opts->ignore_pf = 0;
    opts->dual_tag = 0;

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, optstring, lopts, &option_index)) != -1) {
        const char *arg;
        switch (opt) {
        case 'i':   opts->input_name = strdup(optarg);
                    break;
        case 'o':   opts->output_name = strdup(optarg);
                    break;
        case 'v':   opts->verbose = true;
                    break;
        case 'b':   opts->barcode_name = strdup(optarg);
                    break;
        case 0:     arg = lopts[option_index].name;
                         if (strcmp(arg, "metrics-file") == 0)               opts->metrics_name = strdup(optarg);
                    else if (strcmp(arg, "max-low-quality-to-convert") == 0) opts->max_low_quality_to_convert = atoi(optarg);
                    else if (strcmp(arg, "convert-low-quality") == 0)        opts->convert_low_quality = true;
                    else if (strcmp(arg, "max-no-calls") == 0)               opts->max_no_calls = atoi(optarg);
                    else if (strcmp(arg, "max-mismatches") == 0)             opts->max_mismatches = atoi(optarg);
                    else if (strcmp(arg, "min-mismatch-delta") == 0)         opts->min_mismatch_delta = atoi(optarg);
                    else if (strcmp(arg, "change-read-name") == 0)           opts->change_read_name = true;
                    else if (strcmp(arg, "barcode-tag-name") == 0)           opts->barcode_tag_name = strdup(optarg);
                    else if (strcmp(arg, "quality-tag-name") == 0)           opts->quality_tag_name = strdup(optarg);
                    else if (strcmp(arg, "input-fmt") == 0)                  opts->input_fmt = strdup(optarg);
                    else if (strcmp(arg, "output-fmt") == 0)                 opts->output_fmt = strdup(optarg);
                    else if (strcmp(arg, "compression-level") == 0)          opts->compression_level = *optarg;
                    else if (strcmp(arg, "ignore-pf") == 0)                  opts->ignore_pf = true;
                    else if (strcmp(arg, "dual-tag") == 0)                  {opts->dual_tag = (short)atoi(optarg);
                                                                             opts->max_no_calls = 0;}  
                    else {
                        printf("\nUnknown option: %s\n\n", arg); 
                        usage(stdout); free_opts(opts);
                        return NULL;
                    }
                    break;
        default:    printf("Unknown option: '%c'\n", opt);
            /* else fall-through */
        case '?':   usage(stdout); free_opts(opts); return NULL;
        }
    }

    argc -= optind;
    argv += optind;

    if (argc > 0) opts->input_name = strdup(argv[0]);
    optind = 0;

    // some validation and tidying
    if (!opts->input_name) {
        fprintf(stderr,"You must specify an input file (-i or --input)\n");
        usage(stderr); free_opts(opts);
        return NULL;
    }
    if (!opts->barcode_name) {
        fprintf(stderr,"You must specify a barcode (tags) file (-b or --barcode-file)\n");
        usage(stderr); free_opts(opts);
        return NULL;
    }

    if (!opts->barcode_tag_name) opts->barcode_tag_name = strdup(DEFAULT_BARCODE_TAG);
    if (!opts->quality_tag_name) opts->quality_tag_name = strdup(DEFAULT_QUALITY_TAG);

    // output defaults to stdout
    if (!opts->output_name) opts->output_name = strdup("-");

    return opts;
}

//
// char *checkBarcodeQuality(char *barcode, char *quality);
//
// return a new barcode read string with low quality bases converted to 'N'
//
static char *checkBarcodeQuality(char *bc_tag, char *qt_tag, opts_t *opts)
{

    char *newBarcode = strdup(bc_tag);
    if (!qt_tag) return newBarcode;

    if (!bc_tag || strlen(bc_tag) != strlen(qt_tag)) {
        fprintf(stderr, "checkBarcodeQuality(): barcode and quality are different lengths\n");
        return NULL;
    }

    int mlq = opts->max_low_quality_to_convert ? opts->max_low_quality_to_convert 
                                               : DEFAULT_MAX_LOW_QUALITY_TO_CONVERT;
    for (int i=0; i < strlen(qt_tag); i++) {
        int qual = qt_tag[i] - 33;
        if (isalpha(newBarcode[i]) && (qual <= mlq)) newBarcode[i] = 'N';
    }

    return newBarcode;
}

void writeMetricsLine(FILE *f, bc_details_t *bcd, opts_t *opts, uint64_t total_reads, uint64_t max_reads, uint64_t total_pf_reads, uint64_t max_pf_reads, uint64_t total_pf_reads_assigned, uint64_t nReads, bool metrics)
{
    fprintf(f, "%s", bcd->idx1);
    if (bcd->idx2 && *bcd->idx2) fprintf(f, "-%s", bcd->idx2);
    fprintf(f, "\t");
    if (metrics) {
        fprintf(f, "%s\t", bcd->name);
        fprintf(f, "%s\t", bcd->lib);
        fprintf(f, "%s\t", bcd->sample);
        fprintf(f, "%s\t", bcd->desc);
    }
    fprintf(f, "%"PRIu64"\t", bcd->reads);
    if (!opts->ignore_pf) {
        fprintf(f, "%"PRIu64"\t", bcd->pf_reads); 
    }
    fprintf(f, "%"PRIu64"\t", bcd->perfect);
    if (!opts->ignore_pf) {
        fprintf(f, "%"PRIu64"\t", bcd->pf_perfect); 
    }
    if (metrics) {
        fprintf(f, "%"PRIu64"\t", bcd->one_mismatch);
        if (!opts->ignore_pf) {
            fprintf(f, "%"PRIu64"\t", bcd->pf_one_mismatch); 
        }
    }
    fprintf(f, "%.3f\t", total_reads ? bcd->reads / (double)total_reads : 0 );
    fprintf(f, "%.3f", max_reads ? bcd->reads / (double)max_reads  : 0 );
    if (!opts->ignore_pf) {
        fprintf(f, "\t%.3f", total_pf_reads ? bcd->pf_reads / (double)total_pf_reads  : 0 ); 
    }
    if (!opts->ignore_pf) {
        fprintf(f, "\t%.3f", max_pf_reads ? bcd->pf_reads / (double)max_pf_reads  : 0 ); 
    }
    if (!opts->ignore_pf) {
        fprintf(f, "\t%.3f", total_pf_reads_assigned ? bcd->pf_reads * nReads / (double)total_pf_reads_assigned  : 0);
    }
    fprintf(f, "\n");
}


/*
 *
 */
int writeMetrics(va_t *barcodeArray, HashTable *tagHopHash, opts_t *opts)
{
    bc_details_t *bcd = barcodeArray->entries[0];
    uint64_t total_reads = bcd->reads;
    uint64_t total_pf_reads = bcd->pf_reads;
    uint64_t total_pf_reads_assigned = 0;
    uint64_t max_reads = 0;
    uint64_t total_original_reads = 0;
    uint64_t total_hop_reads = 0;
    uint64_t max_pf_reads = 0;
    uint64_t nReads = 0;
    int n;

    // Open the metrics file
    FILE *f = fopen(opts->metrics_name, "w");
    if (!f) {
        fprintf(stderr,"Can't open metrics file %s\n", opts->metrics_name);
        return 1;
    }

    // first loop to count things
    for (n=1; n < barcodeArray->end; n++) {
        bc_details_t *bcd = barcodeArray->entries[n];;
        total_reads += bcd->reads;
        total_original_reads += bcd->reads;
        total_pf_reads += bcd->pf_reads;
        total_pf_reads_assigned += bcd->pf_reads;
        if (max_reads < bcd->reads) max_reads = bcd->reads;
        if (max_pf_reads < bcd->pf_reads) max_pf_reads = bcd->pf_reads;
        nReads++;
    }

    // Copy the tag hop hash into an array and sort it
    va_t *tagHopArray = NULL;
    if (tagHopHash && tagHopHash->nused) {
        tagHopArray = va_init(barcodeArray->end, free_bcd);
        HashIter *iter = HashTableIterCreate();
        HashItem *hi;
        while ( (hi = HashTableIterNext(tagHopHash, iter)) != NULL) {
            va_push(tagHopArray, hi->data.p);
        }
        HashTableIterDestroy(iter);
        sortTagHops(tagHopArray);
    }

    if (tagHopArray) {
        for (n=0; n < tagHopArray->end; n++) {
            bc_details_t *bcd = tagHopArray->entries[n];
            total_hop_reads += bcd->reads;
        }
    }

    // print header
    print_header(f, opts, true);

    // second loop to print things
    for (n=1; n < barcodeArray->end; n++) {
        bc_details_t *bcd = barcodeArray->entries[n];
        writeMetricsLine(f, bcd, opts, total_reads, max_reads, total_pf_reads, max_pf_reads, total_pf_reads_assigned, nReads, true);
    }
    // treat Tag 0 as a special case
    bcd = barcodeArray->entries[0];
    bcd->perfect = 0;
    bcd->pf_perfect = 0;
    bcd->name[0] = 0;
    writeMetricsLine(f, bcd, opts, total_reads, max_reads, total_pf_reads, max_pf_reads, 0, nReads, true);

    fclose(f);

    /*
     * Now write tag hop metrics file - if there are any
     */

    if (opts->idx2_len) {
        char *metrics_hops_name = malloc(strlen(opts->metrics_name)+6);
        strcpy(metrics_hops_name, opts->metrics_name);
        strcat(metrics_hops_name, ".hops");
        FILE *g = fopen(metrics_hops_name, "w");
        if (!g) {
            fprintf(stderr,"Can't open tag hops file %s\n", metrics_hops_name);
        } else {
            fprintf(g, "##\n");
            fprintf(g, "# TOTAL_READS=%"PRIu64", ", total_reads);
            fprintf(g, "TOTAL_ORIGINAL_TAG_READS=%"PRIu64", ", total_original_reads);
            fprintf(g, "TOTAL_TAG_HOP_READS=%"PRIu64", ", total_hop_reads);
            fprintf(g, "MAX_READ_ON_A_TAG=%"PRIu64", ", max_reads);
            fprintf(g, "TOTAL_TAG_HOPS=%d, ", (tagHopArray ? tagHopArray->end : 0));
            fprintf(g, "PCT_TAG_HOPS=%f\n", (float)total_hop_reads / total_reads * 100);
            print_header(g, opts, false);

            if (tagHopArray) {
                sortTagHops(tagHopArray);
                for (n=0; n < tagHopArray->end; n++) {
                    bc_details_t *bcd = tagHopArray->entries[n];
                    writeMetricsLine(g, bcd, opts, total_reads, max_reads, total_pf_reads, max_pf_reads, total_pf_reads_assigned, nReads, false);
                }
            }
            fclose(g);
        }
        free(metrics_hops_name);
    }

    va_free(tagHopArray);
    return 0;
}

/*
 * split a dual index (eg ACACAC-TGTGTG) into two different indexes.
 * If a single index is given, then the second index is an empty string.
 */
static void split_index(char *seq, int dual_tag, char **idx1_ptr, char **idx2_ptr)
{
    char *seq_copy = strdup(seq);

    if (dual_tag) {
        *idx2_ptr = strdup(seq_copy + dual_tag);
        seq_copy[dual_tag-1] = 0;
        *idx1_ptr = strdup(seq_copy);
    } else {
        char *saveptr;
        char *p;
        p = strtok_r(seq_copy, INDEX_SEPARATOR, &saveptr);
        if (p) *idx1_ptr = strdup(p);
        p = strtok_r(NULL, INDEX_SEPARATOR, &saveptr);
        if (p) *idx2_ptr = strdup(p); else *idx2_ptr = strdup("");
    }
    free(seq_copy);
}

/*
 * Read the barcode file into an array
 */
static va_t *loadBarcodeFile(opts_t *opts)
{
    int idx1_len=0, idx2_len=0;
    va_t *barcodeArray = va_init(100,free_bcd);

    // initialise first entry for null metrics
    bc_details_t *bcd = calloc(1, sizeof(bc_details_t));
    bcd->seq = NULL;
    bcd->idx1 = NULL;
    bcd->idx2 = NULL;
    bcd->name = strdup("0");
    bcd->lib = strdup("");
    bcd->sample = strdup("");
    bcd->desc = strdup("");
    va_push(barcodeArray,bcd);

    FILE *fh = fopen(opts->barcode_name,"r");
    if (!fh) {
        fprintf(stderr,"ERROR: Can't open barcode file %s\n", opts->barcode_name);
        return NULL;
    }
    
    char *buf = NULL;
    size_t n;
    if (getline(&buf,&n,fh) < 0) {    // burn first line which is a header
        fprintf(stderr,"ERROR: problem reading barcode file\n");
        return NULL;
    }
    free(buf); buf=NULL;

    while (getline(&buf, &n, fh) > 0) {
        char *s;
        if (buf[strlen(buf)-1] == '\n') buf[strlen(buf)-1]=0;   // remove trailing lf
        bc_details_t *bcd = calloc(1,sizeof(bc_details_t));
        s = strtok(buf,"\t");  bcd->seq     = strdup(s);
        s = strtok(NULL,"\t"); bcd->name    = strdup(s);
        s = strtok(NULL,"\t"); bcd->lib     = strdup(s);
        s = strtok(NULL,"\t"); bcd->sample  = strdup(s);
        s = strtok(NULL,"\t"); bcd->desc    = strdup(s);

        split_index(bcd->seq, opts->dual_tag, &bcd->idx1, &bcd->idx2);

        va_push(barcodeArray,bcd);
        free(buf); buf=NULL;

        if (idx1_len == 0) {
            idx1_len = strlen(bcd->idx1);
            idx2_len = strlen(bcd->idx2);
        } else {
            if ( (idx1_len != strlen(bcd->idx1)) && (idx2_len != strlen(bcd->idx2)) ) {
                fprintf(stderr,"ERROR: Tag '%s' is a different length to the previous tag\n", bcd->seq);
                return NULL;
            }
        }
    }

    opts->idx1_len = idx1_len;
    opts->idx2_len = idx2_len;
    bcd = barcodeArray->entries[0];
    bcd->idx1 = calloc(1,idx1_len+1); memset(bcd->idx1, 'N', idx1_len);
    bcd->idx2 = calloc(1,idx2_len+1); memset(bcd->idx2, 'N', idx2_len);
    bcd->seq = calloc(1,idx1_len+idx2_len+2);
    strcpy(bcd->seq,bcd->idx1);
    if (idx2_len) strcat(bcd->seq,INDEX_SEPARATOR);
    strcat(bcd->seq,bcd->idx2);

    free(buf);
    fclose(fh);
    return barcodeArray;
}

/*
 * return true if base is a noCall
 */
//#define isNoCall(c) (c=='N')
int isNoCall(char b)
{
    return b=='N' || b=='n' || b=='.';
}

/*
 * Count the number of noCalls in a sequence
 */
static int noCalls(char *s)
{
    int n=0;
    while (*s) {
        if (isNoCall(*s++)) n++;
    }
    return n;
}

/*
 * count number of mismatches between two sequences
 * (ignoring noCalls)
 */
static int countMismatches(char *tag, char *barcode, int maxval)
{
    int n = 0;
    for (int i=0; tag[i]; i++) {
        if ((tag[i] != barcode[i]) && (barcode[i] != 'N')) {
            n++;
            if (n>maxval) return n;     // exit early if we can
        }
    }
    return n;
}

/*
 * For a failed match, check is there is tag hopping to report
 */
static bc_details_t *check_tag_hopping(char *barcode, va_t *barcodeArray, HashTable *tagHopHash, opts_t *opts)
{
    bc_details_t *bcd=NULL, *best_match1, *best_match2;
    char *idx1, *idx2;
    int nmBest1 = opts->idx1_len + opts->idx2_len + 1;
    int nmBest2 = nmBest1;

    split_index(barcode, opts->dual_tag, &idx1, &idx2);

    // for each tag in barcodeArray
    for (int n=1; n < barcodeArray->end; n++) {
        bc_details_t *bcd = barcodeArray->entries[n];

        int nMismatches1 = countMismatches(bcd->idx1, idx1, nmBest1);
        int nMismatches2 = countMismatches(bcd->idx2, idx2, nmBest2);

        // match the first tag
        if (nMismatches1 < nmBest1) {
            nmBest1 = nMismatches1;
            best_match1 = bcd;
        }

        // match the second tag
        if (nMismatches2 < nmBest2) {
            nmBest2 = nMismatches2;
            best_match2 = bcd;
        }
    }

    free(idx2); free(idx1);

    bool matched_first = (nmBest1 == 0 );
    bool matched_second = (nmBest2 == 0 );

    if (matched_first && matched_second) {
        HashData hd;
        HashItem *hi;
        char *key = malloc(opts->idx1_len + opts->idx2_len + 2);
        strcpy(key, best_match1->idx1);
        strcat(key, INDEX_SEPARATOR);
        strcat(key, best_match2->idx2);
        hi = HashTableSearch(tagHopHash, key, 0);
        if (hi) {
            bcd = hi->data.p;
        } else {
            bcd = calloc(1, sizeof(bc_details_t)); //create a new entry with the two tags
            bcd->idx1 = strdup(best_match1->idx1);            
            bcd->idx2 = strdup(best_match2->idx2);            
            bcd->seq = strdup(key);
            bcd->name = strdup("0");
            bcd->lib = strdup("DUMMY_LIB");
            bcd->sample = strdup("DUMMY_SAMPLE");
            bcd->desc = NULL;
            hd.p = bcd;
            HashTableAdd(tagHopHash, key, 0, hd, NULL);
        }
        free(key);
    }

    return bcd;
}


/*
 * find the best match in the barcode (tag) file for a given barcode
 * return the tag, if a match found, else return NULL
 */
bc_details_t *findBestMatch(char *barcode, va_t *barcodeArray, HashTable *barcodeHash, opts_t *opts)
{
    int bcLen = opts->idx1_len + opts->idx2_len + 1;   // size of barcode sequence in barcode file
    bc_details_t *best_match = NULL;
    int nmBest = bcLen;             // number of mismatches (best)
    int nm2Best = bcLen;            // number of mismatches (second best)

    bool matched = false;

    // First look in the barcodeHash for an exact match
    // This optimisation only applies when mismatch_delta is 1 (the default)
    if (opts->min_mismatch_delta <= 1) {
        HashItem *hi;
        hi = HashTableSearch(barcodeHash, barcode, 0);
        if (hi) {
            return hi->data.p;
        }
    }

    // No exact match, so do it the hard way...
    for (int n=1; n < barcodeArray->end; n++) {
        bc_details_t *bcd = barcodeArray->entries[n];

        int nMismatches = countMismatches(bcd->seq, barcode, nm2Best);
        if (nMismatches < nmBest) {
            nm2Best = nmBest;
            nmBest = nMismatches;
            best_match = bcd;
        } else {
            if (nMismatches < nm2Best) nm2Best = nMismatches;
        }
    }

    matched = best_match && nmBest <= opts->max_mismatches && nm2Best - nmBest >= opts->min_mismatch_delta;

    if (!matched) {
        best_match = barcodeArray->entries[0];
    }

    return best_match;
}

/*
 * Update the metrics information
 */
static void updateMetrics(bc_details_t *bcd, char *seq, bool isPf)
{
    int n = 99;
    if (seq) n = countMismatches(bcd->seq, seq, 999);

    bcd->reads++;
    if (isPf) bcd->pf_reads++;

    if (n==0) {     // count perfect matches
        bcd->perfect++;
        if (isPf) bcd->pf_perfect++;
    }

    if (n==1) {     // count out-by-one matches
        bcd->one_mismatch++;
        if (isPf) bcd->pf_one_mismatch++;
    }
}

/*
 * find the best match in the barcode (tag) file, and return the corresponding barcode name
 * If no match found, check for tag hopping, and return dummy entry 0
 */
static char *findBarcodeName(char *barcode, va_t *barcodeArray, HashTable *barcodeHash, HashTable *tagHopHash, opts_t *opts, bool isPf, bool isUpdateMetrics)
{
    bc_details_t *bcd;
    if (noCalls(barcode) > opts->max_no_calls) {
        bcd = barcodeArray->entries[0];
        if (isUpdateMetrics) updateMetrics(bcd, barcode, isPf);
    } else {
        bcd = findBestMatch(barcode, barcodeArray, barcodeHash, opts);
        if (isUpdateMetrics) updateMetrics(bcd, barcode, isPf);
        if ((bcd == barcodeArray->entries[0]) && opts->idx2_len) {
            bc_details_t *tag_hop = check_tag_hopping(barcode, barcodeArray, tagHopHash, opts);
            if (isUpdateMetrics && tag_hop) updateMetrics(tag_hop, barcode, isPf);
        }
    }
    return bcd->name;
}

/*
 * make a new tag by appending #<name> to the old tag
 */
static char *makeNewTag(bam1_t *rec, char *tag, char *name)
{
    char *rg = "";
    uint8_t *p = bam_aux_get(rec,tag);
    if (p) rg = bam_aux2Z(p);
    char *newtag = malloc(strlen(rg) + strlen(name) + 16);
    strcpy(newtag, rg);
    strcat(newtag,"#");
    strcat(newtag, name);
    return newtag;
}

/*
 * Change the read name by adding "#<suffix>"
 */
static void add_suffix(bam1_t *rec, char *suffix)
{
    int oldqlen = strlen((char *)rec->data);
    int newlen = rec->l_data + strlen(suffix) + 1;

    if (newlen > rec->m_data) {
        rec->m_data = newlen;
        kroundup32(rec->m_data);
        rec->data = (uint8_t *)realloc(rec->data, rec->m_data);
    }
    memmove(rec->data + oldqlen + strlen(suffix) + 1,
            rec->data + oldqlen,
            rec->l_data - oldqlen);
    rec->data[oldqlen] = '#';
    memmove(rec->data + oldqlen + 1,
            suffix,
            strlen(suffix) + 1);
    rec->l_data = newlen;
    rec->core.l_qname += strlen(suffix) + 1;
}

/*
 * Add a new @RG line to the header
 */
static void addNewRG(SAM_hdr *sh, char *entry, char *bcname, char *lib, char *sample, char *desc)
{
    char *saveptr;
    char *p = strtok_r(entry,"\t",&saveptr);
    char *newtag = malloc(strlen(p)+1+strlen(bcname)+1);
    strcpy(newtag, p);
    strcat(newtag,"#");
    strcat(newtag, bcname);
    sam_hdr_add(sh, "RG", "ID", newtag, NULL, NULL);

    SAM_hdr_type *hdr = sam_hdr_find(sh, "RG", "ID", newtag);
    while (1) {
        char *pu = NULL;
        char *t = strtok_r(NULL, ":", &saveptr);
        if (!t) break;
        char *v = strtok_r(NULL, "\t", &saveptr);
        if (!v) break;

        // handle special cases
        if (strcmp(t,"PU") == 0) {
            // add #bcname
            pu = malloc(strlen(v) + 1 + strlen(bcname) + 1);
            strcpy(pu, v); strcat(pu,"#"); strcat(pu,bcname);
            v = pu;
        }
        if (strcmp(t,"LB") == 0) {
            if (lib) v = lib;        // use library name
        }
        if (strcmp(t,"DS") == 0) {
            if (desc) v = desc;       // use desc
        }
        if (strcmp(t,"SM") == 0) {
            if (sample) v = sample;     // use sample name
        }
        sam_hdr_update(sh, hdr, t, v, NULL);
        if (pu) free(pu);
    }
    free(newtag);
}

/*
 * for each "@RG ID:x" in the header, replace with
 * "@RG IDx#barcode" for each barcode
 *
 * And don't forget to add a @PG header
 */ 
static void changeHeader(va_t *barcodeArray, bam_hdr_t *h, char *argv_list)
{
    SAM_hdr *sh = sam_hdr_parse_(h->text, h->l_text);
    char **rgArray = malloc(sizeof(char*) * sh->nrg);
    int nrg = sh->nrg;
    int i, n;

    sam_hdr_add_PG(sh, "bambi", "VN", bambi_version(), "CL", argv_list, NULL);

    // store the RG names
    for (n=0; n < sh->nrg; n++) {
        // store the names and tags as a string <name>:<tag>:<val>:<tag>:<val>...
        // eg 1:PL:Illumina:PU:110608_HS19

        // first pass to determine size of string required
        int sz=strlen(sh->rg[n].name)+1;
        SAM_hdr_tag *rgtag = sh->rg[n].tag;
        while (rgtag) {
            if (strncmp(rgtag->str,"ID:",3)) {  // ignore name
                sz += 3 + strlen(rgtag->str) + 1;
            }
            rgtag = rgtag->next;
        }
        char *entry = malloc(sz+1);

        // second pass to create string
        strcpy(entry,sh->rg[n].name);
        rgtag = sh->rg[n].tag;
        while (rgtag) {
            if (strncmp(rgtag->str,"ID:",3)) {  // ignore name
                strcat(entry,"\t");
                strcat(entry,rgtag->str);
            }
            rgtag = rgtag->next;
        }
        rgArray[n] = entry;
    }

    // Remove the existing RG lines
    sh = sam_hdr_del(sh, "RG", NULL, NULL);

    // add the new RG lines
    for (n=0; n<nrg; n++) {
        char *entry = strdup(rgArray[n]);
        addNewRG(sh, entry, "0", NULL, NULL, NULL);
        free(entry);

        // for each tag in barcodeArray
        for (i=1; i < barcodeArray->end; i++) {
            bc_details_t *bcd = barcodeArray->entries[i];

            char *entry = strdup(rgArray[n]);
            addNewRG(sh, entry, bcd->name, bcd->lib, bcd->sample, bcd->desc);
            free(entry);
        }
    }

    for (n=0; n<nrg; n++) {
        free(rgArray[n]);
    }
    free(rgArray);

    free(h->text);
    sam_hdr_rebuild(sh);
    h->text = strdup(sam_hdr_str(sh));
    h->l_text = sam_hdr_length(sh);
    sam_hdr_free(sh);
}

/*
 * Process one template
 */
static int processTemplate(va_t *template, BAMit_t *bam_out, va_t *barcodeArray, HashTable *barcodeHash, HashTable *tagHopHash, opts_t *opts)
{
    char *name = NULL;
    char *bc_tag = NULL;
    char *qt_tag = NULL;
    short error_code = 0;

    // look for barcode tag
    for (int n=0; n < template->end; n++) {
        bam1_t *rec = template->entries[n];
        uint8_t *p = bam_aux_get(rec,opts->barcode_tag_name);
        if (p) {
            if (bc_tag) { // have we already found a tag?
                if (strcmp(bc_tag,bam_aux2Z(p)) != 0) {
                    fprintf(stderr,"Record %s has two different barcode tags: %s and %s\n",
                                   bam_get_qname(rec), bc_tag, bam_aux2Z(p));
                    return -1;
                }
            } else {
                bc_tag = strdup(bam_aux2Z(p));
                p = bam_aux_get(rec,opts->quality_tag_name);
                if (p) qt_tag = strdup(bam_aux2Z(p));
            }
        }
    }

    // if the convert_low_quality flag is set, then (potentially) change the tag
    char *newtag = NULL;
    if (bc_tag) {
        if (opts->convert_low_quality) {
            newtag = checkBarcodeQuality(bc_tag,qt_tag,opts);
        } else {
            newtag = strdup(bc_tag);
        }
        // truncate to barcode lengths if necessary
        char *idx1, *idx2;
        split_index(bc_tag, opts->dual_tag, &idx1, &idx2);
        if ( (strlen(idx1) > opts->idx1_len) || (strlen(idx2) > opts->idx2_len) ) {
            if (strlen(idx1) > opts->idx1_len) idx1[opts->idx1_len]=0;
            if (strlen(idx2) > opts->idx2_len) idx2[opts->idx2_len]=0;
            strcpy(newtag,idx1); 
            if (opts->idx2_len) strcat(newtag,INDEX_SEPARATOR);
            strcat(newtag,idx2);
        }
        free(idx2); free(idx1);
    }

    for (int n=0; n < template->end; n++) {
        bam1_t *rec = template->entries[n];
        if (newtag) {
            if (n==0) name = findBarcodeName(newtag,barcodeArray, barcodeHash, tagHopHash, opts,!(rec->core.flag & BAM_FQCFAIL), n==0);
            char *newrg = makeNewTag(rec,"RG",name);
            bam_aux_update_str(rec,"RG",strlen(newrg)+1, newrg);
            free(newrg);
            if (opts->change_read_name) add_suffix(rec, name);
        }
        int r = sam_write1(bam_out->f, bam_out->h, rec);
        if (r < 0) {
            fprintf(stderr, "Could not write sequence\n");
            error_code = -1;
            break;
        }
    }

    free(newtag);
    free(qt_tag);
    free(bc_tag);
    return error_code;
}

/*
 * Read records from a given iterator until the qname changes
 */
static va_t *loadTemplate(BAMit_t *bit, char *qname)
{
    va_t *recordSet = va_init(5,freeRecord);

    while (BAMit_hasnext(bit) && strcmp(bam_get_qname(BAMit_peek(bit)),qname) == 0) {
        bam1_t *rec = bam_init1();
        bam_copy1(rec,BAMit_next(bit));
        va_push(recordSet,rec);
    }

    return recordSet;
}


 
/*
 * Main code
 */
static int decode(opts_t* opts)
{
    int retcode = 1;
    BAMit_t *bam_in = NULL;
    BAMit_t *bam_out = NULL;
    va_t *barcodeArray = NULL;
    HashTable *tagHopHash = NULL;
    HashTable *barcodeHash = NULL;

    while (1) {
        /*
         * Read the barcode (tags) file 
         */
        barcodeArray = loadBarcodeFile(opts);
        if (!barcodeArray) break;

        // create hash from barcodeArray
        barcodeHash = HashTableCreate(0, HASH_DYNAMIC_SIZE | HASH_FUNC_JENKINS);
        for (int n=0; n < barcodeArray->end; n++) {
            bc_details_t *bcd = barcodeArray->entries[n];
            HashData hd;
            hd.p = bcd;
            HashTableAdd(barcodeHash, bcd->seq, 0, hd, NULL);
        }

        tagHopHash = HashTableCreate(0, HASH_DYNAMIC_SIZE | HASH_FUNC_JENKINS);

        /*
         * Open input fnd output BAM files
         */
        bam_in = BAMit_open(opts->input_name, 'r', opts->input_fmt, 0);
        if (!bam_in) break;
        bam_out = BAMit_open(opts->output_name, 'w', opts->output_fmt, opts->compression_level);
        if (!bam_out) break;
        // copy input to output header
        bam_hdr_destroy(bam_out->h); bam_out->h = bam_hdr_dup(bam_in->h);

        // Change header by adding PG and RG lines
        changeHeader(barcodeArray, bam_out->h, opts->argv_list);
        if (sam_hdr_write(bam_out->f, bam_out->h) != 0) {
            fprintf(stderr, "Could not write output file header\n");
            break;
        }

        // Read and process each template in the input BAM
        while (BAMit_hasnext(bam_in)) {
            bam1_t *rec = BAMit_peek(bam_in);
            char *qname = strdup(bam_get_qname(rec));
            va_t *template = loadTemplate(bam_in, qname);
            if (processTemplate(template, bam_out, barcodeArray, barcodeHash, tagHopHash, opts)) break;
            va_free(template);
            free(qname);
        }

        if (BAMit_hasnext(bam_in)) break;   // we must has exited the above loop early

        /*
         * And finally.....the metrics
         */
        if (opts->metrics_name) {
            if (writeMetrics(barcodeArray, tagHopHash, opts) != 0) break;
        }
                
        retcode = 0;
        break;
    }

    // tidy up after us
    va_free(barcodeArray);
    HashTableDestroy(barcodeHash, 0);
    HashTableDestroy(tagHopHash, 0);
    BAMit_free(bam_in);
    BAMit_free(bam_out);

    return retcode;
}

/*
 * called from bambi to perform index decoding
 *
 * Parse the command line arguments, then call the main decode function
 *
 * returns 0 on success, 1 if there was a problem
 */
int main_decode(int argc, char *argv[])
{
    int ret = 1;

    opts_t* opts = parse_args(argc, argv);
    if (opts) {
        ret = decode(opts);
    }
    free_opts(opts);
    return ret;
}
