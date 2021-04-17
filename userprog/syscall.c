#include "userprog/syscall.h"
#include "include/lib/user/syscall.h"
#include "threads/synch.h"
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

struct lock filesys_lock;

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
	lock_init(&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

struct file_elem*
file_elem_by_fd(int fd) {
	struct thread* curr = thread_current();
	struct list_elem* el;
	struct file_elem* f_el;

	if (fd == NULL || fd < 0 || list_empty(&curr->files_list)) {
		exit(-1);
	}
	if (fd <= 1) {
		return NULL;
	}
	el = list_begin(&curr->files_list);
	while (el != list_end(&curr->files_list)) {
		f_el = list_entry(el, struct file_elem, elem);
		if (f_el->fd == fd)
			return f_el;
		el = el->next;
	}
	exit(-1);
}

void
halt(void) {
	power_off();
}

void
close_all_files(void) {
	struct thread* curr = thread_current();
	struct list_elem* el;
	struct file_elem *f_el;

	if (list_empty(&curr->files_list))
		return;
	el = list_begin(&curr->files_list);
	while (el != list_end(&curr->files_list)) {
		f_el = list_entry(el, struct file_elem, elem);
		el = el->next;
		file_close(f_el->file);
		list_remove(&f_el->elem);
		free(f_el);
	}
}

void
exit(int status) {
	struct thread* curr = thread_current();

	curr->exit_status = status;
	// if (curr->is_process)
	// 	printf ("%s: exit(%d)\n", curr->name, curr->exit_status);
	close_all_files();
	thread_exit();
}

pid_t
fork (const char *thread_name) {
	struct thread* parent = thread_current();
	int parent_lock = 0;
	pid_t child_pid;
	enum intr_level old_level;

	if (parent->depth > 20)
		return TID_ERROR;

	child_pid = process_fork(thread_name, &parent->tf, &parent_lock);
	while (parent_lock == 0) {
		old_level = intr_disable();
		thread_block();
		intr_set_level(old_level);
	}
	if (child_pid == TID_ERROR)
		return TID_ERROR;
	return running_thread()->status == THREAD_RUNNING ? (parent_lock > 0 ? child_pid : TID_ERROR) : 0;
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
wait (pid_t pid) {
	int result = process_wait(pid);

	return result;
}

bool
remove (const char *file) {
	// TODO: 열려 있는 파일의 remove 처리
	bool ret;

	ret = filesys_remove(file);
	return ret;
}

int
open (const char *file) {
	struct thread* curr = thread_current();
	struct file* opened_file;
	struct file_elem* new_f_el = malloc(sizeof(struct file_elem));
	int new_fd = 2;
	struct list_elem* el;
	struct file_elem* f_el;

	if (file == NULL || new_f_el == NULL) {
		exit(-1);
	}
	lock_acquire(&filesys_lock);
	opened_file = filesys_open(file);

	if (opened_file == NULL) {
		lock_release(&filesys_lock);
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
	lock_release(&filesys_lock);
	return new_fd;
}

int
filesize (int fd) {
	int size;
	struct file_elem* f_el = file_elem_by_fd(fd);

	size = file_length(f_el->file);
	return size;
}

int
read (int fd, void *buffer, unsigned size) {
	struct thread* curr = thread_current();
	struct file_elem* f_el;
	int read_size;

	lock_acquire(&filesys_lock);
	if(fd == 0) {
		read_size = input_getc();
	} else if(fd == 1) {
		exit(-1);
	}	else {
		f_el = file_elem_by_fd(fd);
		read_size = file_read(f_el->file, buffer, size);
	}
	lock_release(&filesys_lock);
	return read_size;
}

int
write (int fd, const void *buffer, unsigned size) {
	struct thread* curr = thread_current();
	struct file_elem* f_el;
	int written_size;

	lock_acquire(&filesys_lock);
	if (fd == 1) {
		putbuf(buffer, size);
	} else if (fd == 0) {
		exit(-1);
	} else {
		f_el = file_elem_by_fd(fd);
		written_size = file_write(f_el->file, buffer, size);
	}
	lock_release(&filesys_lock);
	return written_size;
}

void
seek (int fd, unsigned position) {
	struct file_elem* f_el = file_elem_by_fd(fd);

	lock_acquire(&filesys_lock);
	file_seek(f_el->file, position);
	lock_release(&filesys_lock);
}

unsigned
tell (int fd) {
	struct file_elem* f_el = file_elem_by_fd(fd);

	return f_el->file->pos;
}

void
close (int fd) {
	struct list_elem* el;
	struct file_elem* f_el = file_elem_by_fd(fd);

	file_close(f_el->file);
	list_remove(&f_el->elem);
	free(f_el);
}

void
is_valid_user_ptr(void* ptr) {
	if (is_kernel_vaddr(ptr) || !pml4_get_page(thread_current()->pml4, ptr) || ptr == NULL) {
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
		f->R.rax = fork(f->R.rdi);
		break;
	case SYS_EXEC:	// 3
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = exec(f->R.rdi);
		break;
	case SYS_WAIT:	// 4
		f->R.rax = wait(f->R.rdi);
		break;
	case SYS_CREATE:	// 5
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = create(f->R.rdi, f->R.rsi);
		break;
	case SYS_REMOVE:	// 6
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = remove(f->R.rdi);
		break;
	case SYS_OPEN:	// 7
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = open(f->R.rdi);
		break;
	case SYS_FILESIZE:	// 8
		f->R.rax = filesize(f->R.rdi);
		break;
	case SYS_READ:	// 9
		is_valid_user_ptr(f->R.rsi);
		f->R.rax = read (f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_WRITE:	// 10
		is_valid_user_ptr(f->R.rsi);
		f->R.rax = write (f->R.rdi, f->R.rsi, f->R.rdx);
		break;
	case SYS_SEEK:	// 11
		seek (f->R.rdi, f->R.rsi);
		break;
	case SYS_TELL:	// 12
		f->R.rax = tell (f->R.rdi);
		break;
	case SYS_CLOSE:	// 13
		close (f->R.rdi);
		break;
	default:
		thread_exit ();
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
