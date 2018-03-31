/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

//#define _DEBUG_
/* MOREFIX: */
extern void ret_from_fork();

long last_pid=0;

void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}

int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f);
	data_limit=get_limit(0x17);
	old_code_base = get_base(current->ldt[1]);
	old_data_base = get_base(current->ldt[2]);
	if (old_data_base != old_code_base)
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
	new_data_base = new_code_base = nr * 0x4000000;
	p->start_code = new_code_base;
	/* 修改属于这个进程的ldt */
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		printk("free_page_tables: from copy_mem\n");
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct volatile *p;
	long volatile * ksp;
	int i;
	struct file *f;
	p = (struct task_struct *) get_free_page();
	if (!p)
		return -EAGAIN;
	task[nr] = p;
	__asm__("cld"::);
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */
//	memcpy(p, current, sizeof(struct task_struct));
	/**
	 * 关键的问题终于找到了 这个函数默认reps来传送 但是DF是置1了
	 * 所以地址是会goes downwards 所以出错了。
	 */

#ifdef _DEBUG__
	printk("current_pid: %d, state: %d\n", current->pid, current->state);
	printk("counter: %d, priority: %d\n", current->counter, current->priority);
#endif
	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = last_pid;
	p->father = current->pid;
	p->priority = current->priority;
	p->counter = p->priority;
	/* p->counter = p->priority */
#ifdef __DEBUG__
	printk("pid: %d, state: %d\n", last_pid, (p)->state);
	printk("counter: %d, priority: %d\n", (p)->counter, (p)->priority);
	printk("father: %d \n", p->father);
#endif
	p->signal = 0;
	p->alarm = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;
/*
MOREFIX: 早期linux fork之后 子进程一开始直接回到int 0x80的下一句

	p->tss.back_link = 0;
	p->tss.esp0 = PAGE_SIZE + (long) p;
	p->tss.ss0 = 0x10;
	p->tss.eip = eip;
	p->tss.eflags = eflags;
	p->tss.eax = 0;
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);
	p->tss.trace_bitmap = 0x80000000;
*/

	/* FILL  the kernel stack! heart of the fork! */
	ksp = (void *)((long)p + PAGE_SIZE);

	ksp--; /* KEY: FUCK! 注意， esp指向这里OK 但是指针就会有问题， esp是默认先-4才处理的 */

	/* for iret */
	*(ksp--) = ss & 0xffff;
	*(ksp--) = esp;
	*(ksp--) = eflags;
	*(ksp--) = cs & 0xffff;
	*(ksp--) = eip;
	/* for system call */
	*(ksp--) = ds & 0xffff;
	*(ksp--) = es & 0xffff;
	*(ksp--) = fs & 0xffff;
	*(ksp--) = gs & 0xffff;
	*(ksp--) = ebp;
	*(ksp--) = edi;
	*(ksp--) = esi;
	*(ksp--) = edx;
	*(ksp--) = ecx;
	*(ksp--) = ebx;
	*(ksp) = 0;  /* eax magic! */
	p->th.esp0 = ksp; 	/* KEY: get a reference */
	p->th.eip = (long)ret_from_fork;
	p->th.ldt = _LDT(nr);
	p->th.gs = gs;
	p->th.trace_bitmap = 0x80000000;

	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (init_tss.i387)); /* MOREFIX: p->tss.i387*/
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	for (i=0; i<NR_OPEN;i++)
		if ((f=p->filp[i]))
			f->f_count++;
	if (current->pwd)
		current->pwd->i_count++;
	if (current->root)
		current->root->i_count++;
	if (current->executable)
		current->executable->i_count++;
	/* MOREFIX:  这两句是为了让描述符知道数据结构所在基址 */
//	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
//	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	set_ldt_desc(gdt+ nr +FIRST_LDT_ENTRY,&(p->ldt));
	p->state = TASK_RUNNING;	/* do this last, just in case 其实在cs=0x10的时候 不会 schedule  */
#ifdef _DEBUG_
	printk("done copy process\n new process pid: %d\n", last_pid);
	printk(
	"esp0 = 0x%08x		ss0 = 0x10;\n"
	"eip = 0x%08x	eflags = 0x%08x;\n"
	"eax = 0		ecx = 0x%08x;\n"
	"edx = 0x%08x	ebx = 0x%08x;\n"
	"esp = 0x%08x	ebp = 0x%08x;\n"
	"esi = 0x%08x	edi =0x%08x;\n"
	"es = 0x%08x	cs = 0x%08x;\n"
	"ss = 0x%08x 	ds = 0x%08x;\n"
	"fs = 0x%08x  	gs = 0x%08x;\n"
	"ldt = 0x%08x	trace_bitmap = 0x80000000\n",  PAGE_SIZE + (long) p, eip, eflags, ecx, edx, ebx,esp, \
		ebp,esi,edi,es ,cs , ss , ds , fs, \
		gs, _LDT(nr) );
#endif
	return last_pid;
}

int find_empty_process(void)
{
	int i;

	repeat:
		if ((++last_pid)<0) last_pid=1;
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
