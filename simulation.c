#include "point_mass.h"
#include "octtree.h"
#include "force_calc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include "simulation.h"
#include "assert.h"
#ifdef HWACCL
#include "hwaccl.h"
#include <semaphore.h>
#include <sys/sem.h>
#include <pthread.h>
#endif

extern uint64_t direct_sum_times;
extern uint64_t direct_sum_total_len;

extern uint64_t group_times;
extern uint64_t group_sum_total_len;

extern uint64_t sum_ilist_count;
static otree_t* the_tree;
#ifdef HWACCL
pthread_t ilist_threads [NUM_PROCESSORS];
pthread_t summation_threads [NUM_PROCESSORS];
pthread_spinlock_t tree_biglock;
#ifdef ANIM
pthread_spinlock_t ofile_lock;
#endif
//thread waits on "control" to start execution
//main thread waits on "result" for end of computation
sem_t     ilist_thread_control[NUM_PROCESSORS];
sem_t     ilist_thread_result [NUM_PROCESSORS];
sem_t     summation_thread_control[NUM_PROCESSORS];
sem_t     summation_thread_result [NUM_PROCESSORS];


static void force_calc_threads_start (void){
	for (int i = 0; i < NUM_PROCESSORS; ++i){
		sem_post (&ilist_thread_control [i]);
	}

	for (int i = 0; i < NUM_PROCESSORS; ++i){
		sem_wait (&ilist_thread_result [i]);
	}
}

static void* ilist_thread_entry (void* ptr_our_tid){
	uint16_t our_tid = (uint16_t)(long)ptr_our_tid;
	assert (our_tid < NUM_PROCESSORS);
	sem_t*   control = &ilist_thread_control [our_tid];
	sem_t*   result = &ilist_thread_result [our_tid];
	for (;;){
		//wait for signal from main thread
		sem_wait (control);	
		//perform computation
		hwaccl_calculate_force (our_tid, the_tree);
		//report to main thread
		sem_post (result);
	}
	//not reached
	return NULL;
}

typedef struct {
	int years;
	int days;
	int seconds;
	int dump;
	FILE* ofile;
	uint16_t tid;
}sum_thread_ctrl_t;

static int integrate (otree_t* node, int years, int days, int seconds, int dump, FILE* ofile);
	
static void hwaccl_integrate (sum_thread_ctrl_t* args){	
	int num = 8 / NUM_PROCESSORS;	
	int start = args->tid* num;

	for (int i = start; i < num; ++i){
		integrate (the_tree->children[i], 
				   args->years, args->days, 
				   args->seconds, args->dump, 
				   args->ofile);
	}
}

static void* summation_thread_entry (void* arg){
	sum_thread_ctrl_t* threadargs = (sum_thread_ctrl_t*)arg;
	uint16_t our_tid = (uint16_t)threadargs->tid;

	sem_t*   control = &summation_thread_control [our_tid];
	sem_t*   result = &summation_thread_result [our_tid];
	for (;;){
		//wait for signal from main thread
		sem_wait (control);
		//perform computation
		hwaccl_integrate (arg);
		//report to main thread
		sem_post (result);
	}
	//not reached
	return NULL;
}

static void thread_init (int ts_years, int ts_days, int ts_secs, int anim, FILE* ofile){
	for (int i = 0; i < NUM_PROCESSORS; ++i){
		sem_init (&ilist_thread_control [i], 1, 0);
		sem_init (&summation_thread_control [i], 1, 0);
		pthread_spin_init (&tree_biglock, 1);
		pthread_create (&ilist_threads [i], NULL, ilist_thread_entry, (void*)(long)i);
		sum_thread_ctrl_t* thread_args = malloc (sizeof (sum_thread_ctrl_t));
		*thread_args = (sum_thread_ctrl_t)
		{
			.years = ts_years,
			.days  = ts_days,
			.seconds = ts_secs,
			.dump = anim,
			.ofile = ofile,
			.tid = (uint16_t)i
		};

		pthread_create (&summation_threads [i], NULL, 
				summation_thread_entry, (void*)thread_args);
#ifdef ANIM
		pthread_spin_init (&ofile_lock, 1);
#endif
	}		
}
#endif

static inline int cmp_int (int a, int b){
	if (a < b) return -1;
	if (a > b) return 1;
	return 0;
}

static int done (int tgt_yr, int tgt_d, int  tgt_s, 
						int curr_yr, int curr_d, int curr_sec)
{
	//lexicographic comparison
	int yr_result = cmp_int (tgt_yr, curr_yr),
		d_result  = cmp_int (tgt_d, curr_d),
		s_result  = cmp_int (tgt_s, curr_sec);
	if (yr_result == -1) return 1;
	if (yr_result == 1) return 0;
	if (d_result == -1) return 1;
	if (d_result == 1) return 0;
	if (s_result == 1) return 0;
	return 1;
}

static inline void do_integration (pmass_t* particle, uint64_t total_seconds){
	point_t added = particle->acc;
	point_t old_vel = particle->vel;
	//scale the acceleration to add
	added . x *= total_seconds;
	added . y *= total_seconds;
	added . z *= total_seconds;

	vector_add (&particle->vel, &added);
	
	added.x = (particle->vel.x + old_vel.x)/2;	
	added.y = (particle->vel.y + old_vel.y)/2;
	added.z = (particle->vel.z + old_vel.z)/2;
	
	added . x *= total_seconds;
	added . y *= total_seconds;
	added . z *= total_seconds;
	
	vector_add (&particle->pos, &added);
}

//mass in kg, distance in metres, velocity in m/s
//force in Newtons
static int integrate (otree_t* node, int years, int days, int seconds, int dump, FILE* ofile){
	pmass_t* the_particle;
	uint64_t total_seconds = (uint64_t)days * SECS_IN_DAY +
							 (uint64_t)years * SECS_IN_DAY * DAYS_IN_YEAR +
							 (uint64_t)seconds;
	int printcount = 0;
	if (node -> children[0] == NULL){
		dlnode_t* curr = node->particles->first,	
				* next;
		pmass_t old, new;
		while (curr != NULL){
			the_particle = (pmass_t*)curr->key;
			next = curr->next;
			old = *the_particle;
			do_integration (the_particle, total_seconds);
			new = *the_particle;
#ifdef HWACCL
			pthread_spin_lock (&tree_biglock);
#endif
			otree_t* new_leaf = otree_relocate (node, curr);	
			otree_fix_com (node, new_leaf, &old, &new);
#ifdef HWACCL
			pthread_spin_unlock (&tree_biglock);
#endif	
			curr = next;
		}
		curr = node->particles->first;
		while (dump && curr){				
#ifdef HWACCL
#ifdef ANIM
			pthread_spin_lock (&ofile_lock);
#endif
#endif	
			the_particle = (pmass_t*)curr->key;
			++printcount;
			fprintf (ofile, "(%.16lf, %.16lf, %.16lf)\n", 
						the_particle->pos.x, the_particle->pos.y, the_particle->pos.z);

#ifdef HWACCL
#ifdef ANIM
			pthread_spin_unlock (&ofile_lock);
#endif
#endif
			curr = curr->next;
		}
	}else{
		for (int i = 0; i < 8; ++i){
			printcount += integrate (node->children[i], years, days, seconds, dump, ofile);
		}
	}
	return printcount;
}

static void run_simulation (int years, int days, int seconds, otree_t* root, int anim){
	int ts_years, ts_days, ts_secs;
	sscanf (TIMESTEP, "%dy%dd%ds", &ts_years, &ts_days, &ts_secs);
	assert (ts_secs < SECS_IN_DAY && ts_days < DAYS_IN_YEAR);

	FILE* ofile = NULL;
#ifdef ANIM
	if (anim){
		ofile = fopen ("simfile", "w");
		fprintf (ofile, "%lf\n", root->side_len);
	}	
#endif

#ifdef HWACCL
	int e_ilist_len = root->total_particles * 0.3;
	uint16_t writes_per_flush = e_ilist_len / 10;	
	hwaccl_init (writes_per_flush);
	thread_init(ts_years, ts_days, ts_secs, anim, ofile);
#endif

	int curr_years = 0, curr_days = 0, curr_secs = 0;
	int cycles = 0;
	while (!done(years, days, seconds, curr_years, curr_days, curr_secs)){
#ifndef HWACCL
		calculate_force (root, root);
#else
		force_calc_threads_start();
#endif
#ifdef ANIM	
		if (anim && !(cycles % CYCLES_PER_WRITE)){
			fprintf(ofile, "====\n");
			fprintf(ofile, "time %dy%dd%ds\n", curr_years, curr_days, curr_secs);
			int printcount = integrate (root, ts_years, ts_days, ts_secs, 1, ofile);
		}else
#endif
		integrate (root, ts_years, ts_days, ts_secs, 0, NULL);
		//add the time step to the current time	
		curr_secs += ts_secs;
		if (curr_secs >= SECS_IN_DAY){
			curr_secs -= SECS_IN_DAY;
			curr_days += 1;
		}
		curr_days += ts_days;
		if (curr_days >= DAYS_IN_YEAR){	
			curr_years += 1;
			curr_days -= DAYS_IN_YEAR;
		}
		curr_years += ts_years;
		if (!(cycles % CYCLES_PER_GARBAGE_COLLECT)){
			otree_garbage_collect (root);
		}
		//printf("%d years, %d days and %d secs have passed\n",curr_years,curr_days,curr_secs);
		++cycles;
	}	
}

void simulation (int years, int days, int seconds,FILE* infile, int anim){
	
	//get the input
	char* heapbuf = malloc(sizeof(char) * 200);
	size_t len = 200;
	int charread = 0;
	int linenum = 0;

	floating_point universe_size = 0;	
	pmass_t* particle;

	while ((charread = getline (&heapbuf, &len, infile)) != -1){
		if (linenum == 0){
			sscanf (heapbuf,FFMT,&universe_size);
			the_tree = otree_new (universe_size);
			dbprintf("universe size %lf\n", universe_size);
		}else{
			//the format string for the float type
			//doesnt matter for output, but does for input
			particle = malloc (sizeof(pmass_t));
			sscanf (heapbuf, "("FFMT"," FFMT"," FFMT"," FFMT")",
					&particle->pos.x, &particle->pos.y,
					&particle->pos.z, &particle->mass);	
			assert (particle->mass >= MIN_MASS);
			print_pmass (particle);	
			otree_insert (the_tree,NULL, particle, 1);	
		}
		++linenum;
	}
	free (heapbuf);
	run_simulation (years, days, seconds, the_tree, anim);
	printf("direct sum: %llu, %llu\ngroup: %llu, %llu\nsum_ilst: %llu\n",
			direct_sum_total_len, direct_sum_times, group_sum_total_len, group_times, sum_ilist_count);
}

