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

PRIVATE timer_t sched_timer;
PRIVATE unsigned balance_timeout;

#define BALANCE_TIMEOUT	5 /* how often to balance queues in seconds */

FORWARD _PROTOTYPE( int schedule_process, (struct schedproc * rmp)	);
FORWARD _PROTOTYPE( void balance_queues, (struct timer *tp)		);

#define DEFAULT_USER_TIME_SLICE 200
/* 
 * If this is defined the scheduler will run dynamic
 * assignment of tickets for priority raising or lowering.
 * SHOULD NOT BE USED IN CONJUNCTION WITH EXPR_PRIORITY
 */
#define DYNAMIC_PRIORITY
/* 
 * If this is defined the scheduler will run our own
 * personal ticket assignment. (Which could be hilariously bad)
 * Idea: Double tickets to those who don't get chosen
 * in the initial lottery (version below)
 * And set tickets to 1 when you use your quantum
 * SHOULD NOT BE USED IN CONJUNCTION WITH DYNAMIC_PRIORITY
 */
/*#define EXPR_PRIORITY*/
#define DEBUG

PRIVATE int is_user_proc(int prio){
	return (prio > WINNER_Q);
}

unsigned max_tickets;
unsigned total_block_count = 0;
/*===========================================================================*
 *				do_noquantum				     *
 *===========================================================================*/

PUBLIC int do_noquantum(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;

	if (sched_isokendpt(m_ptr->m_source, &proc_nr_n) != OK) {
		printf("SCHED: WARNING: got an invalid endpoint in OOQ msg %u.\n",
		m_ptr->m_source);
		return EBADEPT;
	}

	rmp = &schedproc[proc_nr_n];
	#ifdef DYNAMIC_PRIORITY
	
		if(m_ptr->SCHEDULING_ACNT_IPC_SYNC > total_block_count){
			total_block_count = m_ptr->SCHEDULING_ACNT_IPC_SYNC;
			allot_tickets(rmp, +1);	/*process blocked, increase tickets*/
		}
		else{
		/* 
		 * Received full quantum, reduce tickets by 1 to lower its
		 * priority forthe next lottery
		 */
		 
			allot_tickets(rmp, -1);
		}
	#endif
	#ifdef EXPR_PRIORITY
	/* 
	 * Received full quantum, set tickets to 1 to start
	 * at lowest priority again!
	 */
	       allot_tickets(rmp, rmp->num_tickets - rmp->num_tickets+1);
	#endif

	if ((rv = schedule_process(rmp)) != OK) {
		return rv;
	}
	play_lottery();
	
	return OK;
	
}

/*===========================================================================*
 *				do_stop_scheduling			     *
 *===========================================================================*/
PUBLIC int do_stop_scheduling(message *m_ptr)
{
	register struct schedproc *rmp;
	int rv, proc_nr_n;

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
	max_tickets-=rmp->num_tickets;

	return OK;
}

/*===========================================================================*
 *				do_start_scheduling			     *
 *===========================================================================*/
PUBLIC int do_start_scheduling(message *m_ptr)
{
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
		allot_tickets(rmp, 10);
		rmp->max_tickets = 20;
		
		break;
		
	case SCHEDULING_INHERIT:
		/* Inherit current priority and time slice from parent. Since there
		 * is currently only one scheduler scheduling the whole system, this
		 * value is local and we assert that the parent endpoint is valid */
		if ((rv = sched_isokendpt(m_ptr->SCHEDULING_PARENT,
				&parent_nr_n)) != OK)
			return rv;

		rmp->priority = LOSER_Q;
		rmp->time_slice = schedproc[parent_nr_n].time_slice;
		allot_tickets(rmp, 5);
		rmp->max_tickets = 20;
		
		break;
		
	default: 
		/* not reachable */
		assert(0);
	}

	/* Take over scheduling the process. The kernel reply message populates
	 * the processes current priority and its time slice */
	if ((rv = sys_schedctl(0, rmp->endpoint, 0, 0, 0)) != OK) {
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
 *				allot_tickets				     *
 *===========================================================================*/
 /* Will add given number of tickets to selected process */
PRIVATE	int allot_tickets(struct schedproc * rmp, int num_tickets){
	/*rmp = &schedproc[proc_nr_n];*/
	int rv;
	if(!is_user_proc(rmp->priority)) return -1;
	if((rmp->num_tickets + num_tickets > rmp->max_tickets) && (rmp->num_tickets + num_tickets < 1)){
		rmp->num_tickets += num_tickets;
		max_tickets +=rmp->num_tickets;
		rv = OK;
	}
	else{
		#ifdef DEBUG
		if(rmp->num_tickets + num_tickets > rmp->max_tickets)
			printf("could not allot tickets, would be > proc_max_tickets");
		else
			printf("could not allot tickets, would be <1 ticket");
		#endif
		rv = -1;
		
	}
	return rv;
}

/*===========================================================================*
 *				do_nice					     *
 *===========================================================================*/
PUBLIC int do_nice(message *m_ptr)
{
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
			rmp->time_slice, 0)) != OK) {
		#ifdef DEBUG
			printf("SCHED: An error occurred when trying to schedule %d: %d\n",
			rmp->endpoint, rv);
		#endif
	}

	return rv;
}


/*===========================================================================*
 *				start_scheduling			     *
 *===========================================================================*/

PUBLIC void init_scheduling(void)
{
	balance_timeout = BALANCE_TIMEOUT * sys_hz();
	init_timer(&sched_timer);
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
	max_tickets = 0;
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
	struct schedproc *rmp;
	int proc_nr;
	int rv;
	
	/* 
	 * Code to balancegoes here:
	 * Should try to keep to a minimum since this interrupt gets called
	 * fairly frequently
	 */

        /* Reset the timer */
	set_timer(&sched_timer, balance_timeout, balance_queues, 0);
}

/* 
 * This function will get the max lottery number. This is a seperate function from
 * play_lottery due to stupid compiler 'declare var after block' rules 
 */
PRIVATE int get_range(){
	int max_winning_num = 0;
	struct schedproc *rmp;
	int proc_nr;
	int rv;
	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++){
		if(is_user_proc(rmp->priority) && (rmp->flags& IN_USE)){
			max_winning_num += rmp->num_tickets;
		}
	}
	/* Subtracting one because rand() function and ticket count include 0 */
	return (max_winning_num - 1);
}

/*===========================================================================*
 *				play_lottery				     *
 *===========================================================================*/
 
 PRIVATE void play_lottery(){
 	struct schedproc *rmp;
	int proc_nr;
	int rv;
	int winning_num;
	int winner = 0;
	int winner_tickets = 0;
	int is_winner = 0;
	/* Do we have to seed every time? */
	srand(time(NULL));
	winning_num = rand() % get_range();
	#ifdef DEBUG
		printf("winning num: %d\n", winning_num);
		printf("LOSERS: ");
	#endif

	for (proc_nr=0, rmp=schedproc; proc_nr < NR_PROCS; proc_nr++, rmp++) {
			winning_num -= rmp->num_tickets;
			if(is_user_proc(rmp->priority) && (rmp->flags& IN_USE)){
				if(winning_num <= 0 && !is_winner){
					rmp->priority = WINNER_Q;	/* Winner! */
					is_winner=1;
					#ifdef DEBUG
						winner = proc_nr;
						winner_tickets = rmp->num_tickets;
					#endif
					
				}
				else{	
					#ifdef DYNAMIC_PRIORITY
					        /* 
					         * The process was not chosen. Increase its
					         * ticket count by one to incerase its priority
					         * for next lottery.
					         */
						if(rmp->num_tickets < rmp->max_tickets){
							allot_tickets(rmp, 1);
						}
					#endif
					#ifdef EXPR_PRIORITY
					        /* 
					         * The process was not chosen. Double its
					         * ticket count for fun!
					         */
						if(rmp->num_tickets < rmp->max_tickets){
							allot_tickets(rmp, rmp->num_tickets*2);
						}
					#endif
					
					rmp->priority = LOSER_Q;
					#ifdef DEBUG
						/* Prints out the losers */
						printf("%d[%d],   ", proc_nr, rmp->num_tickets);
					#endif
				}
			schedule_process(rmp);
			}
	}
	#ifdef DEBUG
		printf("**WINNER: %d[%d]**\n", winner, winner_tickets);
	#endif
 }
