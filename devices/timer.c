#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "list.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* See [8254] for hardware details of the 8254 timer chip. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

// 세마포어 기능 추가
// 세마포어 카운트
struct semaphore timer_sema;

// sleep_node 구조체
struct sleep_node{
	int64_t tick;
	struct thread *thread;
	struct list_elem elem;
};
// struct list 구현
struct list sl_list;
struct list_elem *e;


/* Number of timer ticks since OS booted. */
static int64_t ticks;

/* Number of loops per timer tick.
   Initialized by timer_calibrate(). */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);


/* Sets up the 8254 Programmable Interval Timer (PIT) to
   interrupt PIT_FREQ times per second, and registers the
   corresponding interrupt. */
void
timer_init (void) {
	/* 8254 input frequency divided by TIMER_FREQ, rounded to
	   nearest. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* CW: counter 0, LSB then MSB, mode 2, binary. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");


	sema_init(&timer_sema, 0);
	list_init(&sl_list);
}

/* Calibrates loops_per_tick, used to implement brief delays. */
void
timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* Approximate loops_per_tick as the largest power-of-two
	   still less than one timer tick. */
	loops_per_tick = 1u << 10;
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* Refine the next 8 bits of loops_per_tick. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* Returns the number of timer ticks since the OS booted. */
int64_t
timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* Returns the number of timer ticks elapsed since THEN, which
   should be a value once returned by timer_ticks(). */
int64_t
timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}


void custom_sleep(struct sleep_node *sl){
	list_push_back(&sl_list, &sl->elem);
	printf("i'm die\n");
	sema_down(&timer_sema);
	printf("restart here? thread wakeup?\n");
}

/// @brief  자고일어나면 락해야해요~~ lock_acquire release
/// @param start 
/// @param sl 
void custom_wakeup(int64_t start, struct sleep_node *sl){
	
	// printf("sleeplist 현황 확인\n");
	// for (e = list_begin(&sl_list); e != list_end(&sl_list); e = list_next(e)){
	for (e = list_begin(&sl_list); e != list_end(&sl_list); e = list_next(e)){
		if (e == sl->thread ) {
			continue;
		}
		// 무한 루프 X 나중에 for문 올려도 될듯 
		if( (e == list_end(&sl_list)) ){
			break;
			// e = list_begin(&sl_list);
		}

		struct sleep_node  *now_sl = list_entry(e, struct sleep_node, elem);
		printf("thread_tid: %d\n", now_sl->thread->tid);
		printf("start : %d tick : %d\n", timer_elapsed (start), now_sl->tick);
		
		// 시간 확인 후 반환
		if(timer_elapsed (start) >= now_sl->tick){
			if(now_sl->thread->tid == 1){
				break;
			}
			// printf("will wake up %d\n", now_sl->thread->tid);
			list_remove(e);
			// printf("before_unblock t_id : %d,status :%d \n",now_sl->thread->tid, now_sl->thread->status);
			thread_unblock(now_sl->thread);

			timer_sema.value++;
			// printf("sema_value: %d\n", timer_sema.value);
			// printf("after_unblock t_id : %d,status :%d \n",now_sl->thread->tid, now_sl->thread->status);
			printf("i'm revive\n");
			break;
		}
		// printf("check %d, %d\n",timer_elapsed (start),  sl->tick);
		// // 무한 루프
		// if( (timer_elapsed (start) < (sl->tick)) && (e == list_end(&sl_list)) ){
		// 	e = list_begin(&sl_list);
		// }


	}

	

}

/* Suspends execution for approximately TICKS timer ticks. */
void
timer_sleep (int64_t ticks) {
	/*
	원문 코드

	int64_t start = timer_ticks ();

	ASSERT (intr_get_level () == INTR_ON);
	
	while (timer_elapsed (start) < ticks)
		thread_yield ();
	*/	
	
	int64_t start = timer_ticks ();
	ASSERT (intr_get_level () == INTR_ON);


	struct thread *td = thread_current();

	struct sleep_node *sl = (struct sleep_node*) malloc(sizeof(struct sleep_node));
	printf("input_tick: %d\n", ticks);
	sl->thread = td;
	sl->tick= start + ticks;
	printf("now_tick: %d\n", sl->tick);
	// struct sleep_node *sl_p;
	// sl_p->thread = td;
	// sl_p->tick= ticks;
	// *sl = *sl_p;

	
	// printf("struct test\n");
	// printf("out td : %d\n", td->tid);
	// printf("out tick : %d\n", ticks);
	// printf("in td : %d\n", sl->thread->tid);
	// printf("in tick : %d\n", sl->tick);

	// thread_current()->status = THREAD_BLOCKED;
	// printf("sema value : %d", timer_sema.value);
	printf("thread_name : %s\n", td->name);

	// main 스레드가 아닌 경우
	if (thread_tid()!=1){
		custom_sleep(sl);
	}
	// 메인스레드 인 경우
	else{
		list_push_back(&sl_list, &sl->elem);
		while (timer_elapsed (start) < ticks){
			custom_wakeup(start, sl);
			thread_yield();

			// sema_count 0일때까지
			while(timer_sema.value >0){
				printf("timer_sema_count:%d", timer_sema.value);
				thread_yield(); 
			}
			
		}	
	}

	// // struct thread *td = thread_current();
	// while (timer_elapsed (start) < ticks)
	// 	thread_yield ();
}


/* Suspends execution for approximately MS milliseconds. */
void
timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* Suspends execution for approximately US microseconds. */
void
timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* Suspends execution for approximately NS nanoseconds. */
void
timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* Prints timer statistics. */
void
timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* Timer interrupt handler. */
static void
timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();
}

/* Returns true if LOOPS iterations waits for more than one timer
   tick, otherwise false. */
static bool
too_many_loops (unsigned loops) {
	/* Wait for a timer tick. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* Run LOOPS loops. */
	start = ticks;
	busy_wait (loops);

	/* If the tick count changed, we iterated too long. */
	barrier ();
	return start != ticks;
}

/* Iterates through a simple loop LOOPS times, for implementing
   brief delays.

   Marked NO_INLINE because code alignment can significantly
   affect timings, so that if this function was inlined
   differently in different places the results would be difficult
   to predict. */
static void NO_INLINE
busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* Sleep for approximately NUM/DENOM seconds. */
static void
real_time_sleep (int64_t num, int32_t denom) {
	/* Convert NUM/DENOM seconds into timer ticks, rounding down.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* We're waiting for at least one full timer tick.  Use
		   timer_sleep() because it will yield the CPU to other
		   processes. */
		timer_sleep (ticks);
	} else {
		/* Otherwise, use a busy-wait loop for more accurate
		   sub-tick timing.  We scale the numerator and denominator
		   down by 1000 to avoid the possibility of overflow. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
