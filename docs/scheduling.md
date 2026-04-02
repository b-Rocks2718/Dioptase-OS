# Scheduling

## Per-core and global ready queues with load-balancing
Lockless per-core ready queues are used to avoid the contention of a single locked global ready queue.
A global ready queue still exists to allow for load-balancing.

### Making a thread runnable
When a thread is ready to run, it is added to the global ready queue, unless it is pinned to a specific core. If the thread is pinned to a core, it gets added to that core's pinned queue. Each core's pinned queue is periodically emptied into the core's local ready queue by the idle thread. 

If the kernel needs to wake a thread from within an ISR, a different route must be taken to avoid acquiring any locks and keep the ISR O(1). Each core has a lockless interrupt-wake-queue. ISRs can add threads here, and the idle thread will periodically wake all the threads in it using the normal wake logic.

### Choosing the next thread to run
Threads are only chosen from the front of the local ready queue, and never the global ready queue. Because it is called from the idle thread, the code that chooses the next thread cannot block. In particular it cannot use the heap.  

### Load Balancing
To ensure the number of threads on each core is roughly balanced, the total number of threads in all ready queues is calculated. This is divided by the number of cores to find how many threads each core should have in an ideal scenario. If any core has too few or too many threads compared to the ideal number of threads, it moves some of its threads either to/from the global ready queue to try and achieve the ideal number of threads in its local ready queue. This balancing is done individually for each static/mlfq priority combination (priorities explained below). Note: threads pinned to a core will not be moved during the rebalance phase.

To prevent thread starvation for threads in the global queue, cores will occasionally move some threads from the global queue into their local ready queue, even if the ready queues are unbalanced. 

To see this working, run `EMU_VGA=yes make load_balance_test`

### Idle thread logic
As mentioned previously, the idle thread wakes all thread in the per-core interrupt wake queue, and it empties the core's pinned queue into its ready queue. Idle threads also wake sleeping threads once they have slept for the required amount of time. Idle threads rebalance the per-core queues, and periodically boost threads to the highest MLFQ priority.

## Combination of Static Priorities and MLFQ Scheduling

### Static Priorities
Each ready queue (the local and global queues) is divided into different queues for each static priority. There are currently three static priorities: `HIGH_PRIORITY`, `NORMAL_PRIORITY`, and `LOW_PRIORITY`. Each static priority gets an associated weight, and the ratio of these weights is equal to the ratios of the frequencies each priority of thread is scheduled. For example, high priority threads curreny have weight 4 and normal priority have weight 2, so high priority threads are chosen to be scheduled twice as often as normal priority. The exception is if one or more of the priority levels has no runnable threads; in this case the scheduler checks other priority levels until it finds a runnable thread. 

To see this working, run `EMU_VGA=yes make priorities_test`

### Multi-Level Feedback Queue (MLFQ)
The queue for each static priority level is divided into different queues for each MLFQ priority level. These priority levels are dynamic and based on thread behavior. The idea behind MLFQ is that threads that block often get higher priority than threads that run for a long time. When a thread needs to be chosen to run, MLFQ checks higher priority queues first, and only checks lower priority threads if higher queues are empty. Each queue has an associated "time quantum", which is the amount of time threads in that queue can run before they get preempted and demoted to a lower priority queue. To prevent all threads from eventually moving to the lowest priority queue, all threads are periodically boosted to the highest priority queue.

To see this working, run `EMU_VGA=yes make mlfq_test`

## Scheduler Parameters

I haven't spent much time tuning these, I will do more experiments once I have user mode.  

`MLFQ_LEVELS: 3` - `LEVEL_ZERO`, `LEVEL_ONE`, `LEVEL_TWO`  
`PRIORITY_LEVELS: 3` - `HIGH_PRIORITY`, `NORMAL_PRIORITY`, `LOW_PRIORITY`    

`GLOBAL_CHECK_INTERVAL: 2` - check shared runnable work on every other scheduling pass  
`MIN_LOCAL_RUNNABLE_THREADS: 1` - a core only needs one admitted peer before it can keep filling opportunistically from the shared bucketed queues  
`MAX_GLOBAL_ADMISSIONS_PER_PASS: 1` - one admission per pass avoids one core vacuuming a whole weighted batch before other cores can participate  

The ratios of these weights determine the relative frequency with which threads
of different priorities are scheduled  
`HIGH_PRIORITY_WEIGHT: 4`  
`MID_PRIORITY_WEIGHT: 2`  
`LOW_PRIORITY_WEIGHT: 1`  

`TIME_QUANTUM[MLFQ_LEVELS]: {2, 4, 8}` - (in PIT interrupts) how many times threads at each level can get get timer interrupted before they are preempted/demoted  
`MLFQ_BOOST_INTERVAL: 250` - (in PIT interrupts) boost queued work often enough to avoid indefinite starvation without constantly resetting CPU-bound threads  

`REBALANCE_INTERVAL: 32` - (in PIT interrupts) rebalance often enough that per-core priority/MLFQ skew does not persist for long on 4-core runs  
`MAX_REBALANCE_PERCENT: 130` - rebalance if we have >130% of our ideal number of threads  
`MIN_REBALANCE_PERCENT: 70` - rebalance if we have <70% of our ideal number of threads  
