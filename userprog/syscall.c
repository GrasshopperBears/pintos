#include "userprog/syscall.h"
#include "include/lib/user/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "filesys/filesys.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool compare_file_elem (const struct list_elem *e1, const struct list_elem *e2);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

void
halt(void) {
	power_off();
}

void
exit(int status) {
	thread_current()->exit_status = status;
	thread_exit();
}

int
exec (const char *cmd_line) {
	int result;
	char* fn_copy = palloc_get_page(0);
	if (fn_copy == NULL) {
		exit(-1);
	}
	
	strlcpy(fn_copy, cmd_line, PGSIZE);
	result = process_exec(fn_copy);
	return result;
}

bool
create (const char *file, unsigned initial_size) {
	bool ret;

	if (file == NULL) {
		exit(-1);
	}
	ret = filesys_create(file, initial_size);
	return ret;
}

int
open (const char *file) {
	struct thread* curr = thread_current();
	struct file* opened_file;
	struct file_elem* new_f_el = palloc_get_page(PAL_ZERO);
	int new_fd = 2;
	struct list_elem* el;
	struct file_elem* f_el;

	if (file == NULL) {
		exit(-1);
	}
	lock_acquire(curr->filesys_lock);
	opened_file = filesys_open(file);
	lock_release(curr->filesys_lock);

	if (opened_file == NULL) {
		return -1;
	}
	if (!list_empty(&curr->files_list)) {
		el = list_front(&curr->files_list);
		while (el != list_end(&curr->files_list)) {
			f_el = list_entry(el, struct file_elem, elem);
			el = el->next;
			if (new_fd < f_el->fd)
				break;
			new_fd++;
		}
	}
	new_f_el->file = opened_file;
	new_f_el->fd = new_fd;
	list_insert_ordered(&curr->files_list, &new_f_el->elem, compare_file_elem, NULL);
	return new_fd;
}

int
write (int fd, const void *buffer, unsigned size) {
	int written_size;

	if (fd == 1) {
		putbuf(buffer, size);
	} 
	return written_size;
}

void
is_valid_user_ptr(void* ptr) {
	if (is_kernel_vaddr(ptr) || !pml4_get_page(thread_current()->pml4, ptr)) {
		exit(-1);
	}
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	// printf ("system call at %s TYPE: %d\n", thread_current()->name, f->R.rax);
	switch (f->R.rax)
	{
	case SYS_HALT:	// 0
		halt();
		break;
	case SYS_EXIT:	// 1
		exit(f->R.rdi);
		break;
	case SYS_FORK:	// 2
		is_valid_user_ptr(f->R.rdi);
		// f->R.rax = fork(f->R.rdi);
		break;
	case SYS_EXEC:	// 3
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = exec(f->R.rdi);
		break;
	case SYS_WAIT:	// 4
		// f->R.rax = wait(f->R.rdi);
		break;
	case SYS_CREATE:	// 5
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:	// 6
		is_valid_user_ptr(f->R.rdi);
		// f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:	// 7
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE:	// 8
		// f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ:	// 9
		is_valid_user_ptr(f->R.rsi);
		// f->R.rax = read (f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:	// 10
		is_valid_user_ptr(f->R.rsi);
		f->R.rax = write (f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:	// 11
		// seek (f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:	// 12
		// f->R.rax = tell (f->R.rdi);
		break;
	case SYS_CLOSE:	// 13
		// close (f->R.rdi);
		break;
	default:
		// thread_exit ();
		break;
	}
}

bool
compare_file_elem (const struct list_elem *e1, const struct list_elem *e2) {
	struct file_elem *t1 = list_entry(e1, struct file_elem, elem);
	struct file_elem *t2 = list_entry(e2, struct file_elem, elem);
	
	int decsriptor1 = t1->fd;
	int decsriptor2 = t2->fd;
	if (decsriptor1 < decsriptor2)
		return true;
	else
		return false;
}
