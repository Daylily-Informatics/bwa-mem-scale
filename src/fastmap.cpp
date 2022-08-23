/*************************************************************************************
                           The MIT License

   BWA-MEM2  (Sequence alignment using Burrows-Wheeler Transform),
   Copyright (C) 2019  Intel Corporation, Heng Li.

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
   BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
   ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
   CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.

Authors: Vasimuddin Md <vasimuddin.md@intel.com>; Sanchit Misra <sanchit.misra@intel.com>;
         Heng Li <hli@jimmy.harvard.edu>
*****************************************************************************************/

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sstream>
#include "fastmap.h"
#include "FMI_search.h"
#include <errno.h>
#ifdef PERFECT_MATCH
#include "perfect.h"
#endif
#ifdef USE_SHM
#include <limits.h>
#include "bwa_shm.h"
#endif

#if AFF && (__linux__)
#include <sys/sysinfo.h>
int affy[256];
#endif

// --------------
extern uint64_t tprof[LIM_R][LIM_C];
// ---------------
#ifdef PERFECT_MATCH
extern uint64_t pprof[LIM_C][NUM_PPROF_ENTRY];
extern uint64_t pprof2[LIM_C][2];
int load_perfect_table(const char *prefix, int len, 
						uint8_t **reference, FMI_search *fmi);
#endif

#ifdef USE_SHM
int hint_readLen = READ_LEN;
extern int opt_bwa_shm_map_touch;
#else
#define hint_readLen READ_LEN
int opt_bwa_shm_map_touch; /* not used */
#endif

void __cpuid(unsigned int i, unsigned int cpuid[4]) {
#ifdef _WIN32
    __cpuid((int *) cpuid, (int)i);

#else
    asm volatile
        ("cpuid" : "=a" (cpuid[0]), "=b" (cpuid[1]), "=c" (cpuid[2]), "=d" (cpuid[3])
            : "0" (i), "2" (0));
#endif
}


int HTStatus()
{
    unsigned int cpuid[4];
    char platform_vendor[12];
    __cpuid(0, cpuid);
    ((unsigned int *)platform_vendor)[0] = cpuid[1]; // B
    ((unsigned int *)platform_vendor)[1] = cpuid[3]; // D
    ((unsigned int *)platform_vendor)[2] = cpuid[2]; // C
    std::string platform = std::string(platform_vendor, 12);

    __cpuid(1, cpuid);
    unsigned int platform_features = cpuid[3]; //D

    // __cpuid(1, cpuid);
    unsigned int num_logical_cpus = (cpuid[1] >> 16) & 0xFF; // B[23:16]
    // fprintf(stderr, "#logical cpus: ", num_logical_cpus);

    unsigned int num_cores = -1;
    if (platform == "GenuineIntel") {
        __cpuid(4, cpuid);
        num_cores = ((cpuid[0] >> 26) & 0x3f) + 1; //A[31:26] + 1
        fprintf(stderr, "Platform vendor: Intel.\n");
    } else  {
        fprintf(stderr, "Platform vendor unknown.\n");
    }

    // fprintf(stderr, "#physical cpus: ", num_cores);

    int ht = platform_features & (1 << 28) && num_cores < num_logical_cpus;
    if (ht)
        fprintf(stderr, "CPUs support hyperThreading !!\n");

    return ht;
}

void memoryAllocErt(ktp_aux_t *aux, worker_t &w, int32_t nreads, int32_t nthreads) {
    mem_opt_t *opt = aux->opt;
    int32_t memSize = nreads;
	int32_t readLen = hint_readLen;

    /* Mem allocation section for core kernels */
    w.regs = NULL; w.chain_ar = NULL; w.hits_ar = NULL; w.seedBuf = NULL;
    w.regs = (mem_alnreg_v *) calloc(memSize, sizeof(mem_alnreg_v));
    w.chain_ar = (mem_chain_v*) malloc (memSize * sizeof(mem_chain_v));
    w.seedBuf = (mem_seed_t *) calloc(memSize * AVG_SEEDS_PER_READ, sizeof(mem_seed_t));
    assert(w.seedBuf != NULL);
    w.seedBufSize = BATCH_SIZE * AVG_SEEDS_PER_READ;

    if (w.regs == NULL || w.chain_ar == NULL || w.seedBuf == NULL) {
        fprintf(stderr, "Memory not allocated!!\nExiting...\n");
        exit(0);
    }

    int64_t allocMem = memSize * sizeof(mem_alnreg_v) +
        memSize * sizeof(mem_chain_v) +
        sizeof(mem_seed_t) * memSize * AVG_SEEDS_PER_READ;
    fprintf(stderr, "------------------------------------------\n");
    fprintf(stderr, "1. Memory pre-allocation for chaining: %0.4lf MB\n", allocMem/1e6);

    /* SWA mem allocation */
    int64_t wsize = BATCH_SIZE * SEEDS_PER_READ;
    for(int l=0; l<nthreads; l++)
    {
        w.mmc.seqBufLeftRef[l*CACHE_LINE]  = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufLeftQer[l*CACHE_LINE]  = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufRightRef[l*CACHE_LINE] = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufRightQer[l*CACHE_LINE] = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN, 64);
        
        w.mmc.wsize_buf_ref[l*CACHE_LINE] = wsize * MAX_SEQ_LEN_REF;
        w.mmc.wsize_buf_qer[l*CACHE_LINE] = wsize * MAX_SEQ_LEN_QER;
        
        assert(w.mmc.seqBufLeftRef[l*CACHE_LINE]  != NULL);
        assert(w.mmc.seqBufLeftQer[l*CACHE_LINE]  != NULL);
        assert(w.mmc.seqBufRightRef[l*CACHE_LINE] != NULL);
        assert(w.mmc.seqBufRightQer[l*CACHE_LINE] != NULL);
    }
    
    for(int l=0; l<nthreads; l++) {
        w.mmc.seqPairArrayAux[l]      = (SeqPair *) malloc((wsize + MAX_LINE_LEN) * sizeof(SeqPair));
        w.mmc.seqPairArrayLeft128[l]  = (SeqPair *) malloc((wsize + MAX_LINE_LEN) * sizeof(SeqPair));
        w.mmc.seqPairArrayRight128[l] = (SeqPair *) malloc((wsize + MAX_LINE_LEN) * sizeof(SeqPair));
        w.mmc.wsize[l] = wsize;

        assert(w.mmc.seqPairArrayAux[l] != NULL);
        assert(w.mmc.seqPairArrayLeft128[l] != NULL);
        assert(w.mmc.seqPairArrayRight128[l] != NULL);
    }   


    allocMem = (wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN) * 2+
        (wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN) * 2 +       
        (wsize + MAX_LINE_LEN) * sizeof(SeqPair) * 3;   
    fprintf(stderr, "2. Memory pre-allocation for BSW: %0.4lf MB = %0.4lf MB * %d threads\n", 
				allocMem*nthreads/1e6, allocMem/1e6, nthreads);

    for (int l=0; l<nthreads; l++)
    {
        w.mmc.lim[l]           = (int32_t *) _mm_malloc((BATCH_SIZE + 32) * sizeof(int32_t), 64); // candidate not for reallocation, deferred for next round of changes.
    }

    allocMem = (BATCH_SIZE + 32) * sizeof(int32_t);
    fprintf(stderr, "3. Memory pre-allocation for BWT: %0.4lf MB = %0.4lf MB * %d threads\n", allocMem*nthreads/1e6, allocMem/1e6, nthreads);
    fprintf(stderr, "------------------------------------------\n");

    allocMem = ((MAX_LINE_LEN * sizeof(mem_v)) + (MAX_LINE_LEN * sizeof(u64v)))
				+ ((BATCH_MUL * READ_LEN * sizeof(mem_t)) + (MAX_HITS_PER_READ * sizeof(uint64_t)));
    w.smemBufSize = MAX_LINE_LEN * sizeof(mem_v);
    w.smems = (mem_v*) malloc(nthreads * w.smemBufSize);
    assert(w.smems != NULL);
    w.hitBufSize = MAX_LINE_LEN * sizeof(u64v);
    w.hits_ar = (u64v*) malloc(nthreads * w.hitBufSize);
    assert(w.hits_ar != NULL);
    for (int i = 0 ; i < nthreads; ++i) {
        kv_init_base(mem_t, w.smems[i * MAX_LINE_LEN], BATCH_MUL * READ_LEN);
        kv_init_base(uint64_t, w.hits_ar[i * MAX_LINE_LEN], MAX_HITS_PER_READ);
    }

    fprintf(stderr, "4. Memory pre-allocation for ERT: %0.4lf MB = %0.4lf MB * %d threads\n", allocMem*nthreads/1e6, allocMem/1e6, nthreads);
    fprintf(stderr, "------------------------------------------\n");

    w.useErt = 1;
}


/*** Memory pre-allocations ***/
void memoryAlloc(ktp_aux_t *aux, worker_t &w, int32_t nreads, int32_t nthreads)
{
    mem_opt_t *opt = aux->opt;  
    int32_t memSize = nreads;
    int32_t readLen = hint_readLen;

    /* Mem allocation section for core kernels */
    w.regs = NULL; w.chain_ar = NULL; w.seedBuf = NULL;

    w.regs = (mem_alnreg_v *) calloc(memSize, sizeof(mem_alnreg_v));
    w.chain_ar = (mem_chain_v*) malloc (memSize * sizeof(mem_chain_v));
    w.seedBuf = (mem_seed_t *) calloc(sizeof(mem_seed_t),  memSize * AVG_SEEDS_PER_READ);

    assert(w.seedBuf  != NULL);
    assert(w.regs     != NULL);
    assert(w.chain_ar != NULL);

    w.seedBufSize = BATCH_SIZE * AVG_SEEDS_PER_READ;

    /*** printing ***/
    int64_t allocMem = memSize * sizeof(mem_alnreg_v) +
        memSize * sizeof(mem_chain_v) +
        sizeof(mem_seed_t) * memSize * AVG_SEEDS_PER_READ;
    fprintf(stderr, "------------------------------------------\n");
    fprintf(stderr, "1. Memory pre-allocation for Chaining: %0.4lf MB\n", allocMem/1e6);

    
    /* SWA mem allocation */
    int64_t wsize = BATCH_SIZE * SEEDS_PER_READ;
    for(int l=0; l<nthreads; l++)
    {
        w.mmc.seqBufLeftRef[l*CACHE_LINE]  = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufLeftQer[l*CACHE_LINE]  = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufRightRef[l*CACHE_LINE] = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN, 64);
        w.mmc.seqBufRightQer[l*CACHE_LINE] = (uint8_t *)
            _mm_malloc(wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN, 64);
        
        w.mmc.wsize_buf_ref[l*CACHE_LINE] = wsize * MAX_SEQ_LEN_REF;
        w.mmc.wsize_buf_qer[l*CACHE_LINE] = wsize * MAX_SEQ_LEN_QER;
        
        assert(w.mmc.seqBufLeftRef[l*CACHE_LINE]  != NULL);
        assert(w.mmc.seqBufLeftQer[l*CACHE_LINE]  != NULL);
        assert(w.mmc.seqBufRightRef[l*CACHE_LINE] != NULL);
        assert(w.mmc.seqBufRightQer[l*CACHE_LINE] != NULL);
    }
    
    for(int l=0; l<nthreads; l++) {
        w.mmc.seqPairArrayAux[l]      = (SeqPair *) malloc((wsize + MAX_LINE_LEN) * sizeof(SeqPair));
        w.mmc.seqPairArrayLeft128[l]  = (SeqPair *) malloc((wsize + MAX_LINE_LEN) * sizeof(SeqPair));
        w.mmc.seqPairArrayRight128[l] = (SeqPair *) malloc((wsize + MAX_LINE_LEN) * sizeof(SeqPair));
        w.mmc.wsize[l] = wsize;

        assert(w.mmc.seqPairArrayAux[l] != NULL);
        assert(w.mmc.seqPairArrayLeft128[l] != NULL);
        assert(w.mmc.seqPairArrayRight128[l] != NULL);
    }   


    allocMem = (wsize * MAX_SEQ_LEN_REF * sizeof(int8_t) + MAX_LINE_LEN) * 2 +
        (wsize * MAX_SEQ_LEN_QER * sizeof(int8_t) + MAX_LINE_LEN)  * 2 +       
        (wsize + MAX_LINE_LEN) * sizeof(SeqPair) * 3;   
    fprintf(stderr, "2. Memory pre-allocation for BSW: %0.4lf MB = %0.4lf MB * %d threads\n", 
				allocMem*nthreads/1e6, allocMem/1e6, nthreads);

    for (int l=0; l<nthreads; l++)
    {
        w.mmc.wsize_mem[l]     = BATCH_MUL * BATCH_SIZE *               readLen;
        w.mmc.matchArray[l]    = (SMEM *) _mm_malloc(w.mmc.wsize_mem[l] * sizeof(SMEM), 64);
        w.mmc.min_intv_ar[l]   = (int32_t *) malloc(w.mmc.wsize_mem[l] * sizeof(int32_t));
        w.mmc.query_pos_ar[l]  = (int16_t *) malloc(w.mmc.wsize_mem[l] * sizeof(int16_t));
        w.mmc.enc_qdb[l]       = (uint8_t *) malloc(w.mmc.wsize_mem[l] * sizeof(uint8_t));
        w.mmc.rid[l]           = (int32_t *) malloc(w.mmc.wsize_mem[l] * sizeof(int32_t));
        w.mmc.lim[l]           = (int32_t *) _mm_malloc((BATCH_SIZE + 32) * sizeof(int32_t), 64); // candidate not for reallocation, deferred for next round of changes.
    }

    allocMem = BATCH_MUL * BATCH_SIZE * readLen * sizeof(SMEM) +
				BATCH_MUL * BATCH_SIZE * readLen *sizeof(int32_t) +
				BATCH_MUL * BATCH_SIZE * readLen *sizeof(int16_t) +
				BATCH_MUL * BATCH_SIZE * readLen *sizeof(int32_t) +
				(BATCH_SIZE + 32) * sizeof(int32_t);
    fprintf(stderr, "3. Memory pre-allocation for BWT: %0.4lf MB = %0.4lf MB * %d threads\n", allocMem*nthreads/1e6, allocMem/1e6, nthreads);
    fprintf(stderr, "------------------------------------------\n");
	w.useErt = 0;
}

ktp_data_t *kt_pipeline(void *shared, int step, void *data, mem_opt_t *opt, worker_t &w)
{
    ktp_aux_t *aux = (ktp_aux_t*) shared;
    ktp_data_t *ret = (ktp_data_t*) data;
    
	if (step == 0)
    {
        ktp_data_t *ret = (ktp_data_t *) calloc(1, sizeof(ktp_data_t));
        assert(ret != NULL);
        uint64_t tim = __rdtsc();
		double rtime = realtime();

        /* Read "reads" from input file (fread) */
        int64_t sz = 0;
        ret->seqs = bseq_read_orig(aux->task_size,
                                   &ret->n_seqs,
                                   aux->ks, aux->ks2,
                                   &sz);

        tprof[READ_IO][0] += __rdtsc() - tim;
        
        fprintf(stderr, "[0000] read_chunk: %ld, work_chunk_size: %ld, nseq: %d\n",
                aux->task_size, sz, ret->n_seqs);   

        if (ret->seqs == 0) {
            free(ret);
            return 0;
        }
        if (!aux->copy_comment){
            for (int i = 0; i < ret->n_seqs; ++i){
#ifndef OPT_RW
                free(ret->seqs[i].comment);
#endif
                ret->seqs[i].comment = 0;
            }
        }
#ifdef OPT_RW
            fprintf(stderr, "\t[0000][ M::%s] read %d sequences (%ld bp)... in %.3f real sec\n",
                    __func__, ret->n_seqs, (long)sz, realtime() - rtime);
#else
        {
            int64_t size = 0;
            for (int i = 0; i < ret->n_seqs; ++i) size += ret->seqs[i].l_seq;

            fprintf(stderr, "\t[0000][ M::%s] read %d sequences (%ld bp)... in %.3f real sec\n",
                    __func__, ret->n_seqs, (long)size, realtime() - rtime);
        }
#endif
                
        return ret;
    } // Step 0         
    else if (step == 1)  /* Step 2: Main processing-engine */
    {
        static int task = 0;
        if (w.nreads < ret->n_seqs)
        {
			int64_t allocMem = ret->n_seqs * sizeof(mem_alnreg_v) +
				ret->n_seqs * sizeof(mem_chain_v) +
				sizeof(mem_seed_t) * ret->n_seqs * AVG_SEEDS_PER_READ;
            fprintf(stderr, "[0000] Memory re-allocation for Chaining (%d => %d): %0.4lf MB\n", w.nreads, ret->n_seqs, allocMem/1e6);
            w.nreads = ret->n_seqs;
            free(w.regs); free(w.chain_ar); free(w.seedBuf);
            w.regs = (mem_alnreg_v *) calloc(w.nreads, sizeof(mem_alnreg_v));
            w.chain_ar = (mem_chain_v*) malloc (w.nreads * sizeof(mem_chain_v));
            w.seedBuf = (mem_seed_t *) calloc(sizeof(mem_seed_t), w.nreads * AVG_SEEDS_PER_READ);
            assert(w.regs != NULL); assert(w.chain_ar != NULL); assert(w.seedBuf != NULL);
        }       
                                
        fprintf(stderr, "[0000] Calling mem_process_seqs.., task: %d\n", task++);

        uint64_t tim = __rdtsc();
        if (opt->flag & MEM_F_SMARTPE)
        {
            bseq1_t *sep[2];
            int n_sep[2];
            mem_opt_t tmp_opt = *opt;

            bseq_classify(ret->n_seqs, ret->seqs, n_sep, sep);

            fprintf(stderr, "[M::%s] %d single-end sequences; %d paired-end sequences.....\n",
                    __func__, n_sep[0], n_sep[1]);
            
            if (n_sep[0]) {
                tmp_opt.flag &= ~MEM_F_PE;
                /* single-end sequences, in the mixture */
                mem_process_seqs(&tmp_opt,
                                 aux->n_processed,
                                 n_sep[0],
                                 sep[0],
                                 0,
                                 w);
                
                for (int i = 0; i < n_sep[0]; ++i)
                    ret->seqs[sep[0][i].id].sam = sep[0][i].sam;
            }
            if (n_sep[1]) {
                tmp_opt.flag |= MEM_F_PE;
                /* paired-end sequences, in the mixture */
                mem_process_seqs(&tmp_opt,
                                 aux->n_processed + n_sep[0],
                                 n_sep[1],
                                 sep[1],
                                 aux->pes0,
                                 w);
                                
                for (int i = 0; i < n_sep[1]; ++i)
                    ret->seqs[sep[1][i].id].sam = sep[1][i].sam;
            }
            free(sep[0]); free(sep[1]);
        }
        else {
            /* pure (single/paired-end), reads processing */
            mem_process_seqs(opt,
                             aux->n_processed,
                             ret->n_seqs,
                             ret->seqs,
                             aux->pes0,
                             w);
        }               
        tprof[MEM_PROCESS2][0] += __rdtsc() - tim;
                
        return ret;
    }           
    /* Step 3: Write output */
    else if (step == 2)
    {
		double rtime = realtime();
        aux->n_processed += ret->n_seqs;
        uint64_t tim = __rdtsc();
        
		for (int i = 0; i < ret->n_seqs; ++i)
        {
            if (ret->seqs[i].sam) {
                // err_fputs(ret->seqs[i].sam, stderr);
#ifdef OPT_RW
                fputs(ret->seqs[i].sam, aux->fp);
				free(ret->seqs[i].sam);
#else
                fputs(ret->seqs[i].sam, aux->fp);
#endif
            }
#ifdef OPT_RW
			if (ret->seqs[i].strbuf)
				free(ret->seqs[i].strbuf);
#else
            free(ret->seqs[i].name); free(ret->seqs[i].comment);
            free(ret->seqs[i].seq); free(ret->seqs[i].qual);
            free(ret->seqs[i].sam);
#endif
        }
        free(ret->seqs);
		fprintf(stderr, "\t[0000][ M::%s] write %d sequences... in %.3f real sec\n",
				__func__, ret->n_seqs, realtime() - rtime);
        free(ret);
        tprof[SAM_IO][0] += __rdtsc() - tim;
		

        return 0;
    } // step 2
    
    return 0;
}

static void *ktp_worker(void *data)
{   
    ktp_worker_t *w = (ktp_worker_t*) data;
    ktp_t *p = w->pl;
    
    while (w->step < p->n_steps) {
        // test whether we can kick off the job with this worker
        int pthread_ret = pthread_mutex_lock(&p->mutex);
        assert(pthread_ret == 0);
        for (;;) {
            int i;
            // test whether another worker is doing the same step
            for (i = 0; i < p->n_workers; ++i) {
                if (w == &p->workers[i]) continue; // ignore itself
                if (p->workers[i].step <= w->step && p->workers[i].index < w->index)
                    break;
            }
            if (i == p->n_workers) break; // no workers with smaller indices are doing w->step or the previous steps
            pthread_ret = pthread_cond_wait(&p->cv, &p->mutex);
            assert(pthread_ret == 0);
        }
        pthread_ret = pthread_mutex_unlock(&p->mutex);
        assert(pthread_ret == 0);

        // working on w->step
        w->data = kt_pipeline(p->shared, w->step, w->step? w->data : 0, w->opt, *(w->w)); // for the first step, input is NULL

        // update step and let other workers know
        pthread_ret = pthread_mutex_lock(&p->mutex);
        assert(pthread_ret == 0);
        w->step = w->step == p->n_steps - 1 || w->data? (w->step + 1) % p->n_steps : p->n_steps;

        if (w->step == 0) w->index = p->index++;
        pthread_ret = pthread_cond_broadcast(&p->cv);
        assert(pthread_ret == 0);
        pthread_ret = pthread_mutex_unlock(&p->mutex);
        assert(pthread_ret == 0);
    }
    pthread_exit(0);
}

/* TODO: change ert_idx_prefix to (int) useErt */
static int process(void *shared, gzFile gfp, gzFile gfp2, int pipe_threads)
{
    ktp_aux_t   *aux = (ktp_aux_t*) shared;
    worker_t     w;
    mem_opt_t   *opt = aux->opt;
    int32_t nthreads = opt->n_threads; // global variable for profiling!
    w.nthreads = opt->n_threads;
#if 0 
#if NUMA_ENABLED
    int  deno = 1;
    int tc = numa_num_task_cpus();
    int tn = numa_num_task_nodes();
    int tcc = numa_num_configured_cpus();
    fprintf(stderr, "num_cpus: %d, num_numas: %d, configured cpus: %d\n", tc, tn, tcc);
    int ht = HTStatus();
    if (ht) deno = 2;
    
    if (nthreads < tcc/tn/deno) {
        fprintf(stderr, "Enabling single numa domain...\n\n");
        // numa_set_preferred(0);
        // bitmask mask(0);
        struct bitmask *mask = numa_bitmask_alloc(numa_num_possible_nodes());
        numa_bitmask_clearall(mask);
        numa_bitmask_setbit(mask, 0);
        numa_bind(mask);
        numa_bitmask_free(mask);
    }
#endif
#if AFF && (__linux__)
    { // Affinity/HT stuff
        unsigned int cpuid[4];
        asm volatile
            ("cpuid" : "=a" (cpuid[0]), "=b" (cpuid[1]), "=c" (cpuid[2]), "=d" (cpuid[3])
             : "0" (0xB), "2" (1));
        int num_logical_cpus = cpuid[1] & 0xFFFF;

        asm volatile
            ("cpuid" : "=a" (cpuid[0]), "=b" (cpuid[1]), "=c" (cpuid[2]), "=d" (cpuid[3])
             : "0" (0xB), "2" (0));
        int num_ht = cpuid[1] & 0xFFFF;
        int num_total_logical_cpus = get_nprocs_conf();
        int num_sockets = num_total_logical_cpus / num_logical_cpus;
        fprintf(stderr, "#sockets: %d, #cores/socket: %d, #logical_cpus: %d, #ht/core: %d\n",
                num_sockets, num_logical_cpus/num_ht, num_total_logical_cpus, num_ht);
        
        for (int i=0; i<num_total_logical_cpus; i++) affy[i] = i;
        int slookup[256] = {-1};

        if (num_ht == 2 && num_sockets == 2)  // generalize it for n sockets
        {
            for (int i=0; i<num_total_logical_cpus; i++) {
                std::ostringstream ss;
                ss << i;
                std::string str = "/sys/devices/system/cpu/cpu"+ ss.str();
                str = str +"/topology/thread_siblings_list";
                // std::cout << str << std::endl;
                // std::string str = "cpu.txt";
                FILE *fp = fopen(str.c_str(), "r");
                if (fp == NULL) {
                    fprintf(stderr, "Error: Cant open the file..\n");
                    break;
                }
                else {
                    int a, b, v;
                    char ch[10] = {'\0'};
                    if (fgets(ch, 10, fp) == NULL) {
						fprintf(stderr, "Error: Can't read the file..\n");
						break;
					}
                    v = sscanf(ch, "%u,%u",&a,&b);
                    if (v == 1) v = sscanf(ch, "%u-%u",&a,&b);
                    if (v == 1) {
                        fprintf(stderr, "Mis-match between HT and threads_sibling_list...%s\n", ch);
                        fprintf(stderr, "Continuing with default affinity settings..\n");
                        break;
                    }
                    slookup[a] = 1;
                    slookup[b] = 2;
                    fclose(fp);
                }
            }
            int a = 0, b = num_total_logical_cpus / num_ht;
            for (int i=0; i<num_total_logical_cpus; i++) {
                if (slookup[i] == -1) {
                    fprintf(stderr, "Unseen cpu topology..\n");
                    break;
                }
                if (slookup[i] == 1) affy[a++] = i;
                else affy[b++] = i;
            }
        }
    }
#endif
#endif /* disable the original code for NUMA and AFFINITY */
#if AFF && (__linux__)
	for (int i = 0; i < opt->n_threads; ++i) {
		affy[i] = opt->start_core + i;
	}
#endif
    
    int32_t nreads = aux->actual_chunk_size / hint_readLen + 10;
    
	/* All memory allocation */
    if (aux->useErt) {
        memoryAllocErt(aux, w, nreads, nthreads);
    }
    else {
        memoryAlloc(aux, w, nreads, nthreads);
    }
    fprintf(stderr, "* Threads used (compute): %d\n", nthreads);
    
    /* pipeline using pthreads */
    ktp_t aux_;
    int p_nt = pipe_threads; // 2;
    int n_steps = 3;
   
    w.ref_string = aux->ref_string;
    w.fmi = aux->fmi;
    w.nreads  = nreads;
    // w.memSize = nreads;
    
    aux_.n_workers = p_nt;
    aux_.n_steps = n_steps;
    aux_.shared = aux;
    aux_.index = 0;
    int pthread_ret = pthread_mutex_init(&aux_.mutex, 0);
    assert(pthread_ret == 0);
    pthread_ret = pthread_cond_init(&aux_.cv, 0);
    assert(pthread_ret == 0);

    fprintf(stderr, "* No. of pipeline threads: %d\n\n", p_nt);
    aux_.workers = (ktp_worker_t*) malloc(p_nt * sizeof(ktp_worker_t));
    assert(aux_.workers != NULL);
    
    for (int i = 0; i < p_nt; ++i) {
        ktp_worker_t *wr = &aux_.workers[i];
        wr->step = 0; wr->pl = &aux_; wr->data = 0;
        wr->index = aux_.index++;
        wr->i = i;
        wr->opt = opt;
        wr->w = &w;
    }
    
    pthread_t *ptid = (pthread_t *) calloc(p_nt, sizeof(pthread_t));
    assert(ptid != NULL);
    
    for (int i = 0; i < p_nt; ++i)
        pthread_create(&ptid[i], 0, ktp_worker, (void*) &aux_.workers[i]);
    
    for (int i = 0; i < p_nt; ++i)
        pthread_join(ptid[i], 0);

    pthread_ret = pthread_mutex_destroy(&aux_.mutex);
    assert(pthread_ret == 0);
    pthread_ret = pthread_cond_destroy(&aux_.cv);
    assert(pthread_ret == 0);

    free(ptid);
    free(aux_.workers);
    /***** pipeline ends ******/
    
    fprintf(stderr, "[0000] Computation ends..\n");
    
    /* Dealloc memory allcoated in the header section */    
    free(w.chain_ar);
    free(w.regs);
    free(w.seedBuf);
    
    for(int l=0; l<nthreads; l++) {
        _mm_free(w.mmc.seqBufLeftRef[l*CACHE_LINE]);
        _mm_free(w.mmc.seqBufRightRef[l*CACHE_LINE]);
        _mm_free(w.mmc.seqBufLeftQer[l*CACHE_LINE]);
        _mm_free(w.mmc.seqBufRightQer[l*CACHE_LINE]);
    }

    for(int l=0; l<nthreads; l++) {
        free(w.mmc.seqPairArrayAux[l]);
        free(w.mmc.seqPairArrayLeft128[l]);
        free(w.mmc.seqPairArrayRight128[l]);
    }

    if (aux->useErt) {
        for (int i = 0 ; i < nthreads; ++i) {
            kv_destroy(w.smems[i * MAX_LINE_LEN]);
            kv_destroy(w.hits_ar[i * MAX_LINE_LEN]);
            _mm_free(w.mmc.lim[i]);
        }
        free(w.smems);
        free(w.hits_ar);
    }
    else {
        for(int l=0; l<nthreads; l++) {
            _mm_free(w.mmc.matchArray[l]);
            free(w.mmc.min_intv_ar[l]);
            free(w.mmc.query_pos_ar[l]);
            free(w.mmc.enc_qdb[l]);
            free(w.mmc.rid[l]);
            _mm_free(w.mmc.lim[l]);
        }
	}

    return 0;
}

static void update_a(mem_opt_t *opt, const mem_opt_t *opt0)
{
    if (opt0->a) { // matching score is changed
        if (!opt0->b) opt->b *= opt->a;
        if (!opt0->T) opt->T *= opt->a;
        if (!opt0->o_del) opt->o_del *= opt->a;
        if (!opt0->e_del) opt->e_del *= opt->a;
        if (!opt0->o_ins) opt->o_ins *= opt->a;
        if (!opt0->e_ins) opt->e_ins *= opt->a;
        if (!opt0->zdrop) opt->zdrop *= opt->a;
        if (!opt0->pen_clip5) opt->pen_clip5 *= opt->a;
        if (!opt0->pen_clip3) opt->pen_clip3 *= opt->a;
        if (!opt0->pen_unpaired) opt->pen_unpaired *= opt->a;
    }
}

void *__load_file(const char *prefix, const char *postfix, void *buf, size_t *size) {
	char path[PATH_MAX];
	FILE *fp;
	size_t flen;
	strcpy_s(path, PATH_MAX, prefix);
	strcat_s(path, PATH_MAX, postfix);

	fprintf(stderr, "INFO: load file: %s\n", path);
	fp = xopen(path, "rb");

	if (fp == NULL) {
		fprintf(stderr, "ERROR: can't open index file: %s\n", path);
		return NULL;
	}

	fseek(fp, 0L, SEEK_END);
	flen = ftell(fp);
	rewind(fp);

	if (buf == NULL) {
		if (size != NULL) *size = flen;
		buf = _mm_malloc(flen, 64);
		if (buf == NULL) {
			fprintf(stderr, "ERROR: can't allocation memory for loading %s. size: %ld\n",
										path, flen);
			return NULL;
		}
	} else if (*size < flen) {
		fprintf(stderr, "ERROR: buffer size (%ld) is less than the actual file size (%ld) for %s\n",
						*size, flen, path);
		return NULL;
	}

	err_fread_noeof(buf, 1, flen, fp);
	
	err_fclose(fp);

	return buf;
}

#ifdef USE_SHM
static uint8_t *__load_ref_string(const char *prefix, uint8_t *ref_string) {
    char binary_seq_file[PATH_MAX];
	FILE *fp;
    int64_t rlen = 0;
    
    strcpy_s(binary_seq_file, PATH_MAX, prefix);
    strcat_s(binary_seq_file, PATH_MAX, ".0123");
    
    fprintf(stderr, "* Binary seq file = %s\n", binary_seq_file);
    fp = fopen(binary_seq_file, "r");
    
    if (fp == NULL) {
        fprintf(stderr, "Error: can't open %s input file\n", binary_seq_file);
        exit(EXIT_FAILURE);
    }
    
    fseek(fp, 0, SEEK_END); 
    rlen = ftell(fp);
	if (ref_string == NULL)
    	ref_string = (uint8_t *) _mm_malloc(rlen, 64);

	if (ref_string == NULL) {
		printf("Error!! : [%s] ref_string is NULL!!\n", __func__);
		exit(EXIT_FAILURE);
	}
    rewind(fp);
    
    /* Reading ref. sequence */
    err_fread_noeof(ref_string, 1, rlen, fp);
    
	fclose(fp);
    fprintf(stderr, "* Reference genome size: %ld bp\n", rlen);
   
 	return ref_string;
}

int _load_ref_string(const char *prefix, uint8_t **ret_ptr) {
	return __bwa_shm_load_file(prefix, ".0123", BWA_SHM_REF, (void **) ret_ptr);
}

void load_ref_string(const char *prefix, uint8_t **ret_ptr) {
	if (_load_ref_string(prefix, ret_ptr)) {
		printf("ERROR: [%s] ref_string is NULL!!\n", __func__);
		exit(EXIT_FAILURE);
	}
	return;
}
#else
void load_ref_string(const char *prefix, uint8_t **ret_ptr) {
    char binary_seq_file[PATH_MAX];
	FILE *fp;
    int64_t rlen = 0;
	uint8_t *ref_string;
    
    strcpy_s(binary_seq_file, PATH_MAX, prefix);
    strcat_s(binary_seq_file, PATH_MAX, ".0123");
    
    fprintf(stderr, "* Binary seq file = %s\n", binary_seq_file);
    fp = fopen(binary_seq_file, "r");
    
    if (fp == NULL) {
        fprintf(stderr, "Error: can't open %s input file\n", binary_seq_file);
        exit(EXIT_FAILURE);
    }
    
    fseek(fp, 0, SEEK_END);
    rlen = ftell(fp);
    ref_string = (uint8_t*) _mm_malloc(rlen, 64);
	if (ref_string == NULL) {
		printf("Error!! : [%s] ref_string is NULL!!\n", __func__);
		exit(EXIT_FAILURE);
	}
    rewind(fp);
    
    /* Reading ref. sequence */
    err_fread_noeof(ref_string, 1, rlen, fp);
    
	fclose(fp);
    fprintf(stderr, "* Reference genome size: %ld bp\n", rlen);
 
 	*ret_ptr = ref_string;
	return;
}
#endif /* !USE_SHM */

static void usage(const mem_opt_t *opt)
{
    fprintf(stderr, "Usage: bwa-mem2 mem [options] <idxbase> <in1.fq> [in2.fq]\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  Algorithm options:\n");
    fprintf(stderr, "    -o STR        Output SAM file name\n");
    fprintf(stderr, "    -t INT        number of threads [%d]\n", opt->n_threads);
#ifdef PERFECT_MATCH
	fprintf(stderr, "    -l INT        use perfect table with the specified seed length. 0 for auto detection.\n");
#else
	fprintf(stderr, "    -l INT        hint for average sequence length\n");
#endif
    fprintf(stderr, "    -k INT        minimum seed length [%d]\n", opt->min_seed_len);
    fprintf(stderr, "    -w INT        band width for banded alignment [%d]\n", opt->w);
    fprintf(stderr, "    -d INT        off-diagonal X-dropoff [%d]\n", opt->zdrop);
    fprintf(stderr, "    -r FLOAT      look for internal seeds inside a seed longer than {-k} * FLOAT [%g]\n", opt->split_factor);
    fprintf(stderr, "    -y INT        seed occurrence for the 3rd round seeding [%ld]\n", (long)opt->max_mem_intv);
    fprintf(stderr, "    -c INT        skip seeds with more than INT occurrences [%d]\n", opt->max_occ);
    fprintf(stderr, "    -D FLOAT      drop chains shorter than FLOAT fraction of the longest overlapping chain [%.2f]\n", opt->drop_ratio);
    fprintf(stderr, "    -W INT        discard a chain if seeded bases shorter than INT [0]\n");
    fprintf(stderr, "    -m INT        perform at most INT rounds of mate rescues for each read [%d]\n", opt->max_matesw);
    fprintf(stderr, "    -S            skip mate rescue\n");
    fprintf(stderr, "    -P            skip pairing; mate rescue performed unless -S also in use\n");
    fprintf(stderr, "Scoring options:\n");
    fprintf(stderr, "   -A INT        score for a sequence match, which scales options -TdBOELU unless overridden [%d]\n", opt->a);
    fprintf(stderr, "   -B INT        penalty for a mismatch [%d]\n", opt->b);
    fprintf(stderr, "   -O INT[,INT]  gap open penalties for deletions and insertions [%d,%d]\n", opt->o_del, opt->o_ins);
    fprintf(stderr, "   -E INT[,INT]  gap extension penalty; a gap of size k cost '{-O} + {-E}*k' [%d,%d]\n", opt->e_del, opt->e_ins);
    fprintf(stderr, "   -L INT[,INT]  penalty for 5'- and 3'-end clipping [%d,%d]\n", opt->pen_clip5, opt->pen_clip3);
    fprintf(stderr, "   -U INT        penalty for an unpaired read pair [%d]\n", opt->pen_unpaired);
//  fprintf(stderr, "   -x STR        read type. Setting -x changes multiple parameters unless overriden [null]\n");
//  fprintf(stderr, "                 pacbio: -k17 -W40 -r10 -A1 -B1 -O1 -E1 -L0  (PacBio reads to ref)\n");
//  fprintf(stderr, "                 ont2d: -k14 -W20 -r10 -A1 -B1 -O1 -E1 -L0  (Oxford Nanopore 2D-reads to ref)\n");
//  fprintf(stderr, "                 intractg: -B9 -O16 -L5  (intra-species contigs to ref)\n");
    fprintf(stderr, "Input/output options:\n");
    fprintf(stderr, "   -p            smart pairing (ignoring in2.fq)\n");
    fprintf(stderr, "   -R STR        read group header line such as '@RG\\tID:foo\\tSM:bar' [null]\n");
    fprintf(stderr, "   -H STR/FILE   insert STR to header if it starts with @; or insert lines in FILE [null]\n");
    fprintf(stderr, "   -j            treat ALT contigs as part of the primary assembly (i.e. ignore <idxbase>.alt file)\n");
    fprintf(stderr, "   -5            for split alignment, take the alignment with the smallest coordinate as primary\n");
    fprintf(stderr, "   -q            don't modify mapQ of supplementary alignments\n");
    fprintf(stderr, "   -K INT        process INT input bases in each batch regardless of nThreads (for reproducibility) []\n");    
    fprintf(stderr, "   -v INT        verbose level: 1=error, 2=warning, 3=message, 4+=debugging [%d]\n", bwa_verbose);
    fprintf(stderr, "   -T INT        minimum score to output [%d]\n", opt->T);
    fprintf(stderr, "   -h INT[,INT]  if there are <INT hits with score >80%% of the max score, output all in XA [%d,%d]\n", opt->max_XA_hits, opt->max_XA_hits_alt);
    fprintf(stderr, "   -a            output all alignments for SE or unpaired PE\n");
    fprintf(stderr, "   -C            append FASTA/FASTQ comment to SAM output\n");
    fprintf(stderr, "   -V            output the reference FASTA header in the XR tag\n");
    fprintf(stderr, "   -Y            use soft clipping for supplementary alignments\n");
    fprintf(stderr, "   -M            mark shorter split hits as secondary\n");
    fprintf(stderr, "   -I FLOAT[,FLOAT[,INT[,INT]]]\n");
    fprintf(stderr, "                 specify the mean, standard deviation (10%% of the mean if absent), max\n");
    fprintf(stderr, "                 (4 sigma from the mean if absent) and min of the insert size distribution.\n");
    fprintf(stderr, "                 FR orientation only. [inferred]\n");
    fprintf(stderr, "   -Z            Use ERT index for seeding\n");
    fprintf(stderr, "Note: Please read the man page for detailed description of the command line and options.\n");
}

int main_mem(int argc, char *argv[])
{
    int          i, c, ignore_alt = 0, n_mt_io = 2;
    int          fixed_chunk_size          = -1;
    char        *p, *rg_line               = 0, *hdr_line = 0;
    const char  *mode                      = 0;
#ifdef USE_SHM
    int useErt = -1; /* if undefined, use bwa_shm's */
#else
    int useErt = DEFAULT_USE_ERT;
#endif
    
    mem_opt_t    *opt, opt0;
    gzFile        fp, fp2 = 0;
    void         *ko = 0, *ko2 = 0;
    int           fd, fd2;
    mem_pestat_t  pes[4];
    ktp_aux_t     aux;
    bool          is_o    = 0;
    uint8_t      *ref_string;
#ifdef PERFECT_MATCH
	int perfect_table_seed_len = PT_SEED_LEN_NO_TABLE;
#endif
	int retval = 0;
	uint64_t beg, end;

    memset_s(&aux, sizeof(ktp_aux_t), 0);
    memset_s(pes, 4 * sizeof(mem_pestat_t), 0);
    for (i = 0; i < 4; ++i) pes[i].failed = 1;
    
    // opterr = 0;
    aux.fp = stdout;
    aux.opt = opt = mem_opt_init();
    memset_s(&opt0, sizeof(mem_opt_t), 0);
    /* Parse input arguments */
    // comment: added option '5' in the list
    while ((c = getopt(argc, argv, "5i:qpaMCSPVYjk:c:v:s:r:t:R:A:B:O:E:U:w:L:d:T:Q:D:m:I:N:W:x:G:h:y:K:X:H:o:f:l:bZ:")) >= 0)
    {
        if (c == 'k') opt->min_seed_len = atoi(optarg), opt0.min_seed_len = 1;
		else if (c == 'b') opt_bwa_shm_map_touch = 1;
        else if (c == 'i') n_mt_io = atoi(optarg);
        else if (c == 'x') mode = optarg;
        else if (c == 'w') opt->w = atoi(optarg), opt0.w = 1;
        else if (c == 'A') opt->a = atoi(optarg), opt0.a = 1, assert(opt->a >= INT_MIN && opt->a <= INT_MAX);
        else if (c == 'B') opt->b = atoi(optarg), opt0.b = 1, assert(opt->b >= INT_MIN && opt->b <= INT_MAX);
        else if (c == 'T') opt->T = atoi(optarg), opt0.T = 1, assert(opt->T >= INT_MIN && opt->T <= INT_MAX);
        else if (c == 'U')
            opt->pen_unpaired = atoi(optarg), opt0.pen_unpaired = 1, assert(opt->pen_unpaired >= INT_MIN && opt->pen_unpaired <= INT_MAX);
        else if (c == 't')
            opt->n_threads = atoi(optarg), opt->n_threads = opt->n_threads > 1? opt->n_threads : 1, assert(opt->n_threads >= INT_MIN && opt->n_threads <= INT_MAX);
		else if (c == 'l')
		{
#if defined(PERFECT_MATCH) || defined(USE_SHM)
			int p = atoi(optarg);
			if (p > 0) {
#ifdef PERFECT_MATCH
				perfect_table_seed_len = p;
#endif
#ifdef USE_SHM
				hint_readLen = p;
#endif
			} 
#ifdef PERFECT_MATCH
			else if (p == 0) {
				perfect_table_seed_len = PT_SEED_LEN_AUTO_TABLE;
			}
#endif
#endif
		}
        else if (c == 'o' || c == 'f')
        {
            is_o = 1;
            aux.fp = fopen(optarg, "w");
            if (aux.fp == NULL) {
                fprintf(stderr, "Error: can't open %s input file\n", optarg);
                exit(EXIT_FAILURE);
            }
            /*fclose(aux.fp);*/
        }
        else if (c == 'P') opt->flag |= MEM_F_NOPAIRING;
        else if (c == 'a') opt->flag |= MEM_F_ALL;
        else if (c == 'p') opt->flag |= MEM_F_PE | MEM_F_SMARTPE;
        else if (c == 'M') opt->flag |= MEM_F_NO_MULTI;
        else if (c == 'S') opt->flag |= MEM_F_NO_RESCUE;
        else if (c == 'Y') opt->flag |= MEM_F_SOFTCLIP;
        else if (c == 'V') opt->flag |= MEM_F_REF_HDR;
        else if (c == '5') opt->flag |= MEM_F_PRIMARY5 | MEM_F_KEEP_SUPP_MAPQ; // always apply MEM_F_KEEP_SUPP_MAPQ with -5
        else if (c == 'q') opt->flag |= MEM_F_KEEP_SUPP_MAPQ;
        else if (c == 'c') opt->max_occ = atoi(optarg), opt0.max_occ = 1;
        else if (c == 'd') opt->zdrop = atoi(optarg), opt0.zdrop = 1;
        else if (c == 'v') bwa_verbose = atoi(optarg);
        else if (c == 'j') ignore_alt = 1;
        else if (c == 'r')
            opt->split_factor = atof(optarg), opt0.split_factor = 1.;
        else if (c == 'D') opt->drop_ratio = atof(optarg), opt0.drop_ratio = 1.;
        else if (c == 'm') opt->max_matesw = atoi(optarg), opt0.max_matesw = 1;
        else if (c == 's') opt->split_width = atoi(optarg), opt0.split_width = 1;
        else if (c == 'G')
            opt->max_chain_gap = atoi(optarg), opt0.max_chain_gap = 1;
        else if (c == 'N')
            opt->max_chain_extend = atoi(optarg), opt0.max_chain_extend = 1;
        else if (c == 'W')
            opt->min_chain_weight = atoi(optarg), opt0.min_chain_weight = 1;
        else if (c == 'y')
            opt->max_mem_intv = atol(optarg), opt0.max_mem_intv = 1;
        else if (c == 'C') aux.copy_comment = 1;
        else if (c == 'K') fixed_chunk_size = atoi(optarg);
        else if (c == 'X') opt->mask_level = atof(optarg);
        else if (c == 'h')
        {
            opt0.max_XA_hits = opt0.max_XA_hits_alt = 1;
            opt->max_XA_hits = opt->max_XA_hits_alt = strtol(optarg, &p, 10);
            if (*p != 0 && ispunct(*p) && isdigit(p[1]))
                opt->max_XA_hits_alt = strtol(p+1, &p, 10);
        }
        else if (c == 'Q')
        {
            opt0.mapQ_coef_len = 1;
            opt->mapQ_coef_len = atoi(optarg);
            opt->mapQ_coef_fac = opt->mapQ_coef_len > 0? log(opt->mapQ_coef_len) : 0;
        }
        else if (c == 'O')
        {
            opt0.o_del = opt0.o_ins = 1;
            opt->o_del = opt->o_ins = strtol(optarg, &p, 10);
            if (*p != 0 && ispunct(*p) && isdigit(p[1]))
                opt->o_ins = strtol(p+1, &p, 10);
        }
        else if (c == 'E')
        {
            opt0.e_del = opt0.e_ins = 1;
            opt->e_del = opt->e_ins = strtol(optarg, &p, 10);
            if (*p != 0 && ispunct(*p) && isdigit(p[1]))
                opt->e_ins = strtol(p+1, &p, 10);
        }
        else if (c == 'L')
        {
            opt0.pen_clip5 = opt0.pen_clip3 = 1;
            opt->pen_clip5 = opt->pen_clip3 = strtol(optarg, &p, 10);
            if (*p != 0 && ispunct(*p) && isdigit(p[1]))
                opt->pen_clip3 = strtol(p+1, &p, 10);
        }
        else if (c == 'R')
        {
            if ((rg_line = bwa_set_rg(optarg)) == 0) {
                free(opt);
                if (is_o)
                    fclose(aux.fp);
                return 1;
            }
        }
        else if (c == 'H')
        {
            if (optarg[0] != '@')
            {
                FILE *fp;
                if ((fp = fopen(optarg, "r")) != 0)
                {
                    char *buf;
                    buf = (char *) calloc(1, 0x10000);
                    assert(buf != NULL);
                    while (fgets(buf, 0xffff, fp))
                    {
                        i = strlen(buf);
                        assert(buf[i-1] == '\n');
                        buf[i-1] = 0;
                        hdr_line = bwa_insert_header(buf, hdr_line);
                    }
                    free(buf);
                    fclose(fp);
                }
            } else hdr_line = bwa_insert_header(optarg, hdr_line);
        }
        else if (c == 'I')
        {
            aux.pes0 = pes;
            pes[1].failed = 0;
            pes[1].avg = strtod(optarg, &p);
            pes[1].std = pes[1].avg * .1;
            if (*p != 0 && ispunct(*p) && isdigit(p[1]))
                pes[1].std = strtod(p+1, &p);
            pes[1].high = (int)(pes[1].avg + 4. * pes[1].std + .499);
            pes[1].low  = (int)(pes[1].avg - 4. * pes[1].std + .499);
            if (pes[1].low < 1) pes[1].low = 1;
            if (*p != 0 && ispunct(*p) && isdigit(p[1]))
                pes[1].high = (int)(strtod(p+1, &p) + .499);
            if (*p != 0 && ispunct(*p) && isdigit(p[1]))
                pes[1].low  = (int)(strtod(p+1, &p) + .499);
        }
        else if (c == 'Z') {
            useErt = atoi(optarg) ? 1 : 0;
        }
        else {
			retval = EXIT_FAILURE;
			goto out;
        }
    }
    
    /* Check output file name */
    if (rg_line)
    {
        hdr_line = bwa_insert_header(rg_line, hdr_line);
        free(rg_line);
    }

    if (opt->n_threads < 1) opt->n_threads = 1;
    if (optind + 2 != argc && optind + 3 != argc) {
        usage(opt);
		retval = EXIT_FAILURE;
		goto out;
    }

    /* Further input parsing */
    if (mode)
    {
        fprintf(stderr, "WARNING: bwa-mem2 doesn't work well with long reads or contigs; please use minimap2 instead.\n");
        if (strcmp(mode, "intractg") == 0)
        {
            if (!opt0.o_del) opt->o_del = 16;
            if (!opt0.o_ins) opt->o_ins = 16;
            if (!opt0.b) opt->b = 9;
            if (!opt0.pen_clip5) opt->pen_clip5 = 5;
            if (!opt0.pen_clip3) opt->pen_clip3 = 5;
        }
        else if (strcmp(mode, "pacbio") == 0 || strcmp(mode, "pbref") == 0 || strcmp(mode, "ont2d") == 0)
        {
            if (!opt0.o_del) opt->o_del = 1;
            if (!opt0.e_del) opt->e_del = 1;
            if (!opt0.o_ins) opt->o_ins = 1;
            if (!opt0.e_ins) opt->e_ins = 1;
            if (!opt0.b) opt->b = 1;
            if (opt0.split_factor == 0.) opt->split_factor = 10.;
            if (strcmp(mode, "ont2d") == 0)
            {
                if (!opt0.min_chain_weight) opt->min_chain_weight = 20;
                if (!opt0.min_seed_len) opt->min_seed_len = 14;
                if (!opt0.pen_clip5) opt->pen_clip5 = 0;
                if (!opt0.pen_clip3) opt->pen_clip3 = 0;
            }
            else
            {
                if (!opt0.min_chain_weight) opt->min_chain_weight = 40;
                if (!opt0.min_seed_len) opt->min_seed_len = 17;
                if (!opt0.pen_clip5) opt->pen_clip5 = 0;
                if (!opt0.pen_clip3) opt->pen_clip3 = 0;
            }
        }
        else
        {
            fprintf(stderr, "[E::%s] unknown read type '%s'\n", __func__, mode);
			retval = EXIT_FAILURE;
			goto out;
        }
    } else update_a(opt, &opt0);

#if defined(PERFECT_MATCH) && (__AVX512BW__)
	if (perfect_table_seed_len != PT_SEED_LEN_NO_TABLE && (opt->flag & MEM_F_PE || optind + 2 < argc)) {
		fprintf(stderr, "[ERROR] when arch=avx512, perfect match cannot be used with paired-end input yet.\n");
		retval = EXIT_FAILURE;
		goto out;
	}
#endif
#ifdef USE_SHM
	bwa_shm_init(argv[optind], &useErt, BWA_SHM_INIT_READ);
#endif
    
    /* Matrix for SWA */
    bwa_fill_scmat(opt->a, opt->b, opt->mat);
    
    /* Load bwt2/FMI index */
    beg = __rdtsc();
    
    fprintf(stderr, "* Ref file: %s\n", argv[optind]);
	aux.useErt = useErt;
    if (!useErt) {
        aux.fmi = new FMI_search(argv[optind]);
        aux.fmi->load_index();
    }
    else {
        aux.fmi = new FMI_search(argv[optind]);
        aux.fmi->load_index_other_elements(BWA_IDX_BNS | BWA_IDX_PAC);
		aux.fmi->load_ert_index();
    }
	aux.fmi->useErt = useErt;
	
	end = __rdtsc();
    tprof[FMI][0] += end - beg;
    
    // reading ref string from the file
    beg = __rdtsc();
    fprintf(stderr, "* Reading reference genome..\n");
	load_ref_string(argv[optind], &ref_string); 
   	aux.ref_string = ref_string;

    end  = __rdtsc();
    tprof[REF_IO][0] += end - beg;
    
	fprintf(stderr, "* Done reading reference genome !!\n\n");
    
    if (ignore_alt) {
        for (i = 0; i < aux.fmi->idx->bns->n_seqs; ++i)
            aux.fmi->idx->bns->anns[i].is_alt = 0;
	}
		
#ifdef PERFECT_MATCH
	memset(pprof, 0, sizeof(uint64_t) * LIM_C * NUM_PPROF_ENTRY);
	memset(pprof2, 0, sizeof(uint64_t) * LIM_C * 2);
	if (perfect_table_seed_len != PT_SEED_LEN_NO_TABLE) {
		beg = __rdtsc();
		uint8_t **reference;
		reference = &ref_string;
		load_perfect_table(argv[optind], perfect_table_seed_len, reference, aux.fmi);
		end = __rdtsc();
		tprof[PERFECT_TABLE_READ][0] = end - beg;
	}
#endif
#ifdef USE_SHM
	bwa_shm_complete(BWA_SHM_INIT_READ);
#endif

    /* READS file operations */
    ko = kopen(argv[optind + 1], &fd);
	if (ko == 0) {
		fprintf(stderr, "[E::%s] fail to open file `%s'.\n", __func__, argv[optind + 1]);
		retval = EXIT_FAILURE;
		goto out;
    }
    // fp = gzopen(argv[optind + 1], "r");
    fp = gzdopen(fd, "r");
    aux.ks = kseq_init(fp);
    
    // PAIRED_END
    /* Handling Paired-end reads */
    aux.ks2 = 0;
    if (optind + 2 < argc) {
        if (opt->flag & MEM_F_PE) {
            fprintf(stderr, "[W::%s] when '-p' is in use, the second query file is ignored.\n",
                    __func__);
        }
        else
        {
            ko2 = kopen(argv[optind + 2], &fd2);
            if (ko2 == 0) {
                fprintf(stderr, "[E::%s] failed to open file `%s'.\n", __func__, argv[optind + 2]);
                retval = EXIT_FAILURE;
				goto out;
            }            
            // fp2 = gzopen(argv[optind + 2], "r");
            fp2 = gzdopen(fd2, "r");
            aux.ks2 = kseq_init(fp2);
            opt->flag |= MEM_F_PE;
            assert(aux.ks2 != 0);
        }
    }

    bwa_print_sam_hdr(aux.fmi->idx->bns, hdr_line, aux.fp);

    if (fixed_chunk_size > 0)
        aux.task_size = fixed_chunk_size;
    else {
        //aux.task_size = 10000000 * opt->n_threads; //aux.actual_chunk_size;
        aux.task_size = opt->chunk_size * opt->n_threads; //aux.actual_chunk_size;
    }
    tprof[MISC][1] = opt->chunk_size = aux.actual_chunk_size = aux.task_size;

    beg = __rdtsc();

    /* Relay process function */
    process(&aux, fp, fp2, n_mt_io);
   
   	end = __rdtsc();
    tprof[PROCESS][0] += end - beg;

    // free memory
#ifdef USE_SHM
	if (bwa_shm_unmap(BWA_SHM_REF))
#endif
    	_mm_free(ref_string);

out:
	if (hdr_line) free(hdr_line);
    if (opt) free(opt);
    if (aux.ks) kseq_destroy(aux.ks);   
    if (fp) err_gzclose(fp); 
	if (ko) kclose(ko);

    // PAIRED_END
    if (aux.ks2) kseq_destroy(aux.ks2);
    if (fp2) err_gzclose(fp2); 
	if (ko2) kclose(ko2);
    
    if (is_o && aux.fp) fclose(aux.fp);

    // new bwt/FMI
    if (aux.fmi) delete(aux.fmi); 

#ifdef PERFECT_MATCH
	free_perfect_table();
#endif
#ifdef USE_SHM
	bwa_shm_final(BWA_SHM_INIT_READ);
#endif

    /* Display runtime profiling stats */
    tprof[MEM][0] = __rdtsc() - tprof[MEM][0];
    display_stats(aux.opt->n_threads);
    
    return retval;
}

