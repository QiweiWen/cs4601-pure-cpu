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
#endif
#include <semaphore.h>
#include <sys/sem.h>
#include <pthread.h>
#include <sys/types.h>
#include <sched.h>
#include <unistd.h>
#include <errno.h>


static otree_t* the_tree;
#if NUM_PROCESSORS > 1
pthread_t ilist_threads [NUM_PROCESSORS];
//thread waits on "control" to start execution
//main thread waits on "result" for end of computation
sem_t     ilist_thread_control[NUM_PROCESSORS];
sem_t     ilist_thread_result[NUM_PROCESSORS];

static int stick_this_thread_to_core(int core_id) {
   int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
   if (core_id < 0 || core_id >= num_cores)
      return EINVAL;

   cpu_set_t cpuset;
   CPU_ZERO(&cpuset);
   CPU_SET(core_id, &cpuset);

   pthread_t current_thread = pthread_self();    
   return pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

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
	stick_this_thread_to_core ((int)our_tid);
	assert (our_tid < NUM_PROCESSORS);
	sem_t*   control = &ilist_thread_control [our_tid];
	sem_t*   result = &ilist_thread_result [our_tid];
	for (;;){
		//wait for signal from main thread
		sem_wait (control);	
		//perform computation
		//bogus	
		multithread_calculate_force (our_tid, the_tree);
		//report to main thread
		sem_post (result);
	}
	//not reached
	return NULL;
}


static void thread_init (int ts_years, int ts_days, int ts_secs, int anim, FILE* ofile){
	for (int i = 0; i < NUM_PROCESSORS; ++i){
		sem_init (&ilist_thread_control [i], 1, 0);	
		pthread_create (&ilist_threads [i], NULL, ilist_thread_entry, (void*)(long)i);
	}		
}
#endif //multiprocessors

#ifdef HWACCL
size_t ilist_stats [NUM_PROCESSORS];
size_t ilist_stats_len [NUM_PROCESSORS];
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
	//print_vector (&particle->acc);
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
static void integrate (otree_t* node, int years, int days, int seconds, int dump, FILE* ofile){
	pmass_t* the_particle;
	uint64_t total_seconds = (uint64_t)days * SECS_IN_DAY +
							 (uint64_t)years * SECS_IN_DAY * DAYS_IN_YEAR +
							 (uint64_t)seconds;	
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
			otree_t* new_leaf = otree_relocate (node, curr);	
			otree_fix_com (node, new_leaf, &old, &new);
			curr = next;
		}
		curr = node->particles->first;
		while (dump && curr){				
			the_particle = (pmass_t*)curr->key;
			fprintf (ofile, "(%.10f, %.10f, %.10f)\n", 
						the_particle->pos.x, the_particle->pos.y, the_particle->pos.z);

			curr = curr->next;
		}
	}else{
		for (int i = 0; i < 8; ++i){
			integrate (node->children[i], years, days, seconds, dump, ofile);
		}
	}
}

static void run_simulation (int years, int days, int seconds, otree_t* root, int anim, int log){
	int ts_years, ts_days, ts_secs;
	sscanf (TIMESTEP, "%dy%dd%ds", &ts_years, &ts_days, &ts_secs);
	assert (ts_secs < SECS_IN_DAY && ts_days < DAYS_IN_YEAR);

	FILE* ofile = NULL;
	int doprint = 0;
#ifdef ANIM
	if (anim){
		ofile = fopen ("simfile", "w");
		fprintf (ofile, "%.10f\n", root->side_len);
	}	
#endif
#ifdef HWACCL
	hwaccl_init ();
#endif

#if NUM_PROCESSORS > 1
	thread_init(ts_years, ts_days, ts_secs, anim, ofile);
#endif

	int curr_years = 0, curr_days = 0, curr_secs = 0;
	int cycles = 0;
	while (!done(years, days, seconds, curr_years, curr_days, curr_secs)){
#if NUM_PROCESSORS < 2
	#ifndef HWACCL
		calculate_force (root, root);
	#else
		hwaccl_calculate_force (0, root, root);
	#endif
#else
		force_calc_threads_start();
#endif
#ifdef HWACCL
		uint32_t ilistlen_sum = 0;
		for (int i = 0; i < NUM_PROCESSORS; ++i){	
			ilistlen_sum += ilist_stats [i];
			ilist_stats [i] = 0;
			ilist_stats_len [i] = 0;
		}
		ilistlen_sum /= NUM_PROCESSORS;
		update_ilist_len (ilistlen_sum);

#endif
#ifdef ANIM	
		if (anim && !(cycles % CYCLES_PER_WRITE)){
			doprint = 1;
			fprintf(ofile, "====\n");
			fprintf(ofile, "time %dy%dd%ds\n", curr_years, curr_days, curr_secs);		
		}
#endif
		if (log && !(cycles % CYCLES_PER_WRITE)){
			fprintf(stdout, "time %dy%dd%ds\n", curr_years, curr_days, curr_secs);
		}	
		integrate (root, ts_years, ts_days, ts_secs, doprint, ofile);
		if (doprint) doprint = 0;
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

void simulation (int years, int days, int seconds,FILE* infile, int anim, int log){
	
	//get the input
	char* heapbuf = malloc(sizeof(char) * 200);
	size_t len = 200;
	int charread = 0;
	int linenum = 0;

	float universe_size = 0;	
	pmass_t* particle;

	while ((charread = getline (&heapbuf, &len, infile)) != -1){
		if (linenum == 0){
			sscanf (heapbuf,"%f",&universe_size);
			the_tree = otree_new (universe_size);
			
		}else{		
			particle = malloc (sizeof(pmass_t));
			
			sscanf (heapbuf, "(""%f""," "%f""," "%f""," "%f"")",
					&particle->pos.x, &particle->pos.y,
					&particle->pos.z, &particle->mass);	
			assert (particle->mass >= MIN_MASS);	
			otree_insert (the_tree,NULL, particle, 1);	
		}
		++linenum;
	}
	free (heapbuf);
	run_simulation (years, days, seconds, the_tree, anim, log);
}

