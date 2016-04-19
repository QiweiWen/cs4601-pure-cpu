#ifndef CONFIG_H
#define CONFIG_H

#define USE_DOUBLE
//#define DEBUG
#define ANIM
#define TIMESTEP "0y10d0s"

#define OTREE_NODE_CAP 50
#define CYCLES_PER_GARBAGE_COLLECT 10
#define CYCLES_PER_WRITE 1

//maximum size of a node (not neccessarily leaf)
//for all particles under it to share the 
//same interactio list
#define GROUP_SIZE (OTREE_NODE_CAP * 5) 
#define BH_THETA 0.2
#define MIN_MASS 10

#ifdef DEBUG
#define dbprintf(...) printf(__VA_ARGS__)
#else
#define dbprintf(...)
#endif
#endif

