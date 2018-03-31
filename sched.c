/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

#define __DEBUG__

#define _S(nr) (1<<((nr)-1))
/* can  WAKE UP */
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];
};

static union task_union init_task = {INIT_TASK,};
long volatile jiffies=0;
long startup_time=0;
struct task_struct *current = &(init_task.task); /* 一开始默认就是0进程！ */
struct task_struct *last_task_used_math = NULL;
/* MOREFIX:: */
struct tss_struct init_tss = MAJOR_TSS;

struct task_struct * task[NR_TASKS] = {&(init_task.task), };

long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) { /*MOREFIX:  last_task_used_math->tss.i387*/
		__asm__("fnsave %0"::"m" (init_tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {	/* current->tss.i387 */
		__asm__("frstor %0"::"m" (init_tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

//void switch_to(struct task_struct * prev, struct task_struct * next )
//{
//
//	if( prev == next)
//		return;
//#ifdef __DEBUG__
//	printk("BEFORE SWITCH\n"
//			"next %d info:\n esp: 0x%08x eip: 0x%08x \n",
//			next->pid, next->th.esp0, next->th.eip);
//#endif
//	/* update tss */
//	init_tss.esp0 = ( long )next + PAGE_SIZE; /* !!令esp0 = th.esp是错的！！ */
//	init_tss.ldt = next->th.ldt;
//	init_tss.trace_bitmap = next->th.trace_bitmap;
//__asm__(
//	"pushfl\n\t"
//	"pushl %%ebp\n\t"
//	"movl %%esp, (%%ebx)\n\t" /* save esp */
//	"movl $1f, %%ecx\n\t"
//	"movl %%ecx, 4(%%ebx)\n\t" /* save eip*/
//	"movl (%%edx), %%esp\n\t" /* load esp*/
//	"movl  %%edx, %%ebx\n\t"
//	"movl $0xfffff000, %%ecx\n\t"
//	"andl  %%ecx, %%ebx\n\t"
//	"movl  %%ebx, current\n\t" /* updates CURRENT */
//	"lldt %%ax\n\t" /* load ldt*/
//	"movl $0x17, %%ecx\n\t"
//	"movw  %%cx, %%fs\n\t"
//	"pushl 4(%%edx)\n\t"
//	"ret\n\t"
//"1:"
//	"cmpl %%ebx,last_task_used_math\n\t" \
//	"jne 2f\n\t"
//	"clts\n\t"
//"2:"
//	"popl %%ebp\n\t"
//	"popfl\n"
//	::"a"(next->th.ldt),"b"(&prev->th),"d"(&next->th)
//	 );
///* 反汇编就可以看出  取地址传进去其实很浪费指令，直接用偏移加上去
// * 就可以最大程度优化，所以现代linux也是这么做的
// *
//movl %ds:752(%edx), %eax
//movl %ss:8(%esp), %ecx
//leal %ds:744(%ecx), %ebx
//addl $0x000002e8, %edx
// *
// *
// */
//#ifdef __DEBUG__
//	printk("AFTER SWITCH and come back!\n"
//			"next %d info:\n esp: 0x%08x eip: 0x%08x \n",
//			next->pid, next->th.esp0, next->th.eip);
//#endif
//}


/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */

void switch_to(struct task_struct *);
void schedule(void)
{
	int i,c;
 /* MOREFIX： next = address of next PCB  */
	struct task_struct ** p, *next;

/* check alarm, wake up any interruptible tasks that have got a signal */

	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		if (*p) {
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper: */

	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {
			if (!*--p)
				continue;
#ifdef _DEBUG_
			printk("pid: %d, state: %d\n", i, (*p)->state);
			printk("counter: %d, priority: %d\n", (*p)->counter, (*p)->priority);
#endif
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c) {
				c = (*p)->counter, next = *p ; /* max counter will be choosen */
			}
		}
		if (c) break;
		/*
		 TELLME: c = 0, no more task get slice
		 fork.c:  p->counter = p->priority;
		 why counter need to be divided 2?
		 are all counter equal to 0?

		 TELLYOU：current->counter != 0,

		 FIXME：个人认为这里要关中断？
		*/
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
#ifdef __DEBUG_
	printk("Switch: %d -> %d\n", current->pid, next->pid);
	printk("current thread: esp: 0x%08x eip: 0x%08x\n", current->th.esp0, current->th.eip);
	printk("next thread: esp: 0x%08x eip: 0x%08x \n", next->th.esp0, next->th.eip);
#endif
	switch_to(next);
}

int sys_pause(void)
{
	current->state = TASK_INTERRUPTIBLE;
	schedule();
	return 0;
}

void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp = *p;
	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;
	schedule();
	if (tmp)
		tmp->state=0; /* 原来也有进程在等待，一并唤醒 */
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;
	void (*fn)();
	struct timer_list * next;
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

void do_timer(long cpl)
{
	extern int beepcount;
	extern void sysbeepstop(void);

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	if (cpl)
		current->utime++;
	else
		current->stime++;

	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();
		}
	}
	if (current_DOR & 0xf0)
		do_floppy_timer();
	if ((--current->counter)>0) return;
	current->counter=0;
	if (!cpl) return;
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	/* 根据 tss的基址来设置 描述符  现在 ts 不再需要tss了 */
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&init_tss);
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	p = gdt+2+FIRST_TSS_ENTRY;
	/* 清空gpt table  */
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");
	ltr(0);
	lldt(0);
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */
	set_intr_gate(0x20,&timer_interrupt);
	outb(inb_p(0x21)&~0x01,0x21);
	set_system_gate(0x80,&system_call);
}
