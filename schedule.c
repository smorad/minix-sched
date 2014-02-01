/* This file contains the scheduling policy for SCHED
 *
 * The entry points are:
 *   do_noquantum:        Called on behalf of process' that run out of quantum
 *   do_start_scheduling  Request to start scheduling a proc
 *   do_stop_scheduling   Request to stop scheduling a proc
 *   do_nice		  Request to change the nice level on a proc
 *   init_scheduling      Called from main.c to set up/prepare scheduling
 */
#include "sched.h"
#include "schedproc.h"
#include <assert.h>
#include <minix/com.h>
#include <machine/archtypes.h>
#include "kernel/proc.h" /* for queue constants */
#include <stdio.h>

PRIVATE timer_t sched_timer;
PRIVATE unsigned balance_timeout;
PRIVATE FILE *debug;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

FORWARD _PROTOTYPE( int schedule_process, (struct schedproc * rmp)	);
FORWARD _PROTOTYPE( void balance_queues, (struct timer *tp)		);

#define DEFAULT_USER_TIME_SLICE 200
#define DEBUG 			/*debug print statements*/
/*#define DYN_PRIO*/		/*dynamic priority adjustment enable */
unsigned max_tickets = 0;



unsigned is_user_process(rmp){
	return ((rmp->priority >= MAX_USER_Q) && (rmp->priority <= MIN_USER_Q));
	
}
/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

PUBLIC int do_noquantum(message *m_ptr)
{
	#ifdef DEBUG
		fprintf(debug,"reached do_no quantum\n");
		fflush(NULL);
	#endif
	register struct schedproc *rmp;
	int rv, proc_nr_n, winning_proc;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
		m_ptr->m_source);
		return EBADEPT;
	}
+
	rmp = &schedproc[proc_nr_n];
/*	if (rmp->priority < MIN_USER_Q) { */
/*		rmp->priority += 1;*/ /* lower priority */
/*	} */
#ifdef DYN_PRIO
	if(!rmp->num_tickets<1){
		--rmp->num_tickets;	/*take away a ticket*/
		--max_tickets;	
	}
#endif
	if ((rv = schedule_process(rmp)) != OK) {
		return rv;
	}
	winning_proc = play_lottery();
	if(winning_proc != OK && is_user_process) return winning_proc;
	return OK;
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
PUBLIC int do_stop_scheduling(message *m_ptr)
{
	#ifdef DEBUG
		fprintf(debug, "reached do_stop_sched\n");
		fflush(NULL);
	#endif
	register struct schedproc *rmp;
	int rv, proc_nr_n, winning_proc;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	rmp->flags = 0; /*&= ~IN_USE;*/
	max_tickets -= rmp->num_tickets; /*when process is done, remove tickets from circulation*/
	
/*	winning_proc = play_lottery();*/
/*	if(winning_proc != OK) return winning_proc;*/

	return OK;
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
PUBLIC int do_start_scheduling(message *m_ptr)
{
	#ifdef DEBUG
		fprintf(debug, "reached do_start_sched\n");
		fflush(NULL);
	#endif
	register struct schedproc *rmp;
	int rv, proc_nr_n, parent_nr_n, nice;
	
	/* we can handle two kinds of messages here */
	assert(m_ptr->m_type == SCHEDULING_START || 
		m_ptr->m_type == SCHEDULING_INHERIT);

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	/* Resolve endpoint to proc slot. */
	if ((rv = sched_isemtyendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n))
			!= OK) {
		return rv;
	}
	rmp = &schedproc[proc_nr_n];

	/* Populate process slot */
	rmp->endpoint     = m_ptr->SCHEDULING_ENDPOINT;
	rmp->parent       = m_ptr->SCHEDULING_PARENT;
	rmp->max_priority = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	rmp->num_tickets = 5;		/*process starts with 5 tickets*/
	rmp->priority = LOSER_Q;	/*initialize as a loser*/
	max_tickets += 5;		/*add more tickets to our pool*/
	if (rmp->max_priority >= NR_SCHED_QUEUES) {
		return EINVAL;
	}
	
	switch (m_ptr->m_type) {

	case SCHEDULING_START:
		/* We have a special case here for system processes, for which
		 * quanum and priority are set explicitly rather than inherited 
		 * from the parent */
		rmp->priority   = rmp->max_priority;
		rmp->time_slice = (unsigned) m_ptr->SCHEDULING_QUANTUM;
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
				&parent_nr_n)) != OK)
			return rv;

		rmp->priority = schedproc[parent_nr_n].priority;
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0)) != OK) {
		printf("Sched: Error taking over scheduling for %d, kernel said %d\n",
			rmp->endpoint, rv);
		return rv;
	}
	rmp->flags = IN_USE;

	/* Schedule the process, giving it some quantum */
	if ((rv = schedule_process(rmp)) != OK) {
		printf("Sched: Error while scheduling process, kernel replied %d\n",
			rv);
		return rv;
	}

	/* Mark ourselves as the new scheduler.
	 * By default, processes are scheduled by the parents scheduler. In case
	 * this scheduler would want to delegate scheduling to another
	 * scheduler, it could do so and then write the endpoint of that
	 * scheduler into SCHEDULING_SCHEDULER
	 */

	m_ptr->SCHEDULING_SCHEDULER = SCHED_PROC_NR;

	return OK;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
PUBLIC int do_nice(message *m_ptr)
{
	#ifdef DEBUG
		fprintf(debug, "nice\n");
		fflush(NULL);
	#endif
	struct schedproc *rmp;
	int rv;
	int proc_nr_n;
	unsigned new_q, old_q, old_max_q;

	/* check who can send you requests */
	if (!accept_message(m_ptr))
		return EPERM;

	if (sched_isokendpt(m_ptr->SCHEDULING_ENDPOINT, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg "
		"%ld\n", m_ptr->SCHEDULING_ENDPOINT);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	new_q = (unsigned) m_ptr->SCHEDULING_MAXPRIO;
	if (new_q >= NR_SCHED_QUEUES) {
		return EINVAL;
	}

	/* Store old values, in case we need to roll back the changes */
	old_q     = rmp->priority;
	old_max_q = rmp->max_priority;

	/* Update the proc entry and reschedule the process */
	rmp->max_priority = rmp->priority = new_q;

	if ((rv = schedule_process(rmp)) != OK) {
		/* Something went wrong when rescheduling the process, roll
		 * back the changes to proc struct */
		rmp->priority     = old_q;
		rmp->max_priority = old_max_q;
	}

	return rv;
}

/*===========================================================================*
 *				schedule_process			     *
 *===========================================================================*/
PRIVATE int schedule_process(struct schedproc * rmp)
{
	int rv;

	if ((rv = sys_schedule(rmp->endpoint, rmp->priority,
			rmp->time_slice)) != OK) {
		printf("SCHED: An error occurred when trying to schedule %d: %d\n",
		rmp->endpoint, rv);
	}

	return rv;
}


/*===========================================================================*
 *				start_scheduling			     *
 *===========================================================================*/

PUBLIC void init_scheduling(void)
{	#ifdef DEBUG
		fprintf(debug, "init_schedule\n");
		fflush(NULL);
	#endif
	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	init_timer(&sched_timer);
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
	srand(time(NULL)); 	/*seed our lottery*/
	#ifdef DEBUG
	debug =	fopen("/log", "w");
	#endif
}

/*===========================================================================*
 *				balance_queues				     *
 *===========================================================================*/

/* This function in called every 100 ticks to rebalance the queues. The current
 * scheduler bumps processes down one priority when ever they run out of
 * quantum. This function will find all proccesses that have been bumped down,
 * and pulls them back up. This default policy will soon be changed.
 */
PRIVATE void balance_queues(struct timer *tp)
{
	#ifdef DEBUG
		fprintf(debug, "balance\n");
		fflush(NULL);
	#endif
	struct schedproc *rmp;
	int proc_nr;
	int rv;

	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
		if (rmp->flags & IN_USE) {
/*			if (rmp->priority > rmp->max_priority) {*/
/*				rmp->priority -= 1; */ /* increase priority */
			#ifdef DYN_PRIO
				if(rmp->tickets < rmp->proc_max_tickets && is_user_process(rmp))
					++rmp->tickets;
					schedule_process(rmp);
				}
			#else 
				schedule_process(rmp);
			
			#endif
			
			
		}
	}

	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}


/*===========================================================================*
 *				play_lottery				     *
 *===========================================================================*/
 
 /* This function will have a process 'play the lottery'. If it wins, it gets
 quantum. This function will return the process number of the winner*/
 
 PRIVATE int play_lottery()
 {
 	#ifdef DEBUG
 		fprintf(debug, "lottery\n");
 		fflush(NULL);
 	#endif
 	struct schedproc *rmp;
 	int proc_nr;
 	int rv;
 	unsigned winning_ticket = rand() % (max_tickets - 1);
 	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++, winning_ticket -= rmp->num_tickets){
 			if(winning_ticket<=0){	/*we've found our winner!*/
 				rmp->priority = WINNER_Q;
 				rv = proc_nr;
 				return rv;
 			}
 			else {			/*set losers to min priority*/
 				rmp->priority = LOSER_Q;
 			}
 		}
 		fprintf(debug, "Ticket underflow, something went wrong");
 		fflush(NULL);
 		rv = -1;
 		return rv;
  }
