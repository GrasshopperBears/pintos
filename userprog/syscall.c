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
#include "filesys/inode.h"
#include "intrinsic.h"
#ifdef VM
#include "vm/vm.h"
#endif

void syscall_entry (void);
void syscall_handler (struct intr_frame *);
bool compare_file_elem (const struct list_elem *e1, const struct list_elem *e2);

struct lock filesys_lock;

struct inode_disk {
	disk_sector_t start;                /* First data sector. */
	off_t length;                       /* File size in bytes. */
	unsigned magic;                     /* Magic number. */
	uint32_t unused[125];               /* Not used. */
};

struct inode {
	struct list_elem elem;              /* Element in inode list. */
	disk_sector_t sector;               /* Sector number of disk location. */
	int open_cnt;                       /* Number of openers. */
	bool removed;                       /* True if deleted, false otherwise. */
	int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
	struct inode_disk data;             /* Inode content. */
};

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

	if (fd < 0 || list_empty(&curr->files_list)) {
		exit(-1);
	}
	el = list_begin(&curr->files_list);
	while (el != list_end(&curr->files_list)) {
		f_el = list_entry(el, struct file_elem, elem);
		if (f_el->fd == fd)
			return f_el;
		el = el->next;
	}
	return NULL;
}

void
halt(void) {
	power_off();
}

bool
is_closed(struct file** closed_files, struct file* find, int size) {
	for (int i = 0; i < size; i++) {
		if (closed_files[i] = find)
			return true;
	}
	return false;
}

void
close_all_files(void) {
	struct thread* curr = thread_current();
	struct list_elem* el;
	struct file_elem *f_el;
	struct file** closed_files;
	int size, idx = 0;

	if (list_empty(&curr->files_list))
		return;
	size = list_size(&curr->files_list);
	closed_files = malloc(sizeof(struct file_elem *) * size);
	el = list_begin(&curr->files_list);
	while (el != list_end(&curr->files_list)) {
		f_el = list_entry(el, struct file_elem, elem);
		el = el->next;
		if (f_el->fd > 1 && f_el->file != NULL) {
			if (!is_closed(closed_files, f_el->file, idx)) {
				closed_files[idx] = f_el->file;
				lock_acquire(&filesys_lock);
				file_close(f_el->file);
				lock_release(&filesys_lock);
				idx++;
			}
		}
		list_remove(&f_el->elem);
		free(f_el);
	}
	free(closed_files);
}

void
exit(int status) {
	struct thread* curr = thread_current();

	curr->exit_status = status;
	if (lock_held_by_current_thread(&filesys_lock))
		lock_release(&filesys_lock);
	close_all_files();
	thread_exit();
}

pid_t
fork (const char *thread_name) {
	struct thread* parent = thread_current();
	int parent_lock = 0;
	pid_t child_pid;
	enum intr_level old_level;

	if (parent->depth > 40)
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
	lock_acquire(&filesys_lock);
	ret = filesys_create(file, initial_size);
	lock_release(&filesys_lock);
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
	
	lock_acquire(&filesys_lock);
	ret = filesys_remove(file);
	lock_release(&filesys_lock);
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
	new_f_el->reference = -1;
	list_insert_ordered(&curr->files_list, &new_f_el->elem, compare_file_elem, NULL);
	lock_release(&filesys_lock);
	return new_fd;
}

int
filesize (int fd) {
	int size;
	struct file_elem* f_el = file_elem_by_fd(fd);

	if (f_el == NULL)
		exit(-1);
	lock_acquire(&filesys_lock);
	size = file_length(f_el->file);
	lock_release(&filesys_lock);
	return size;
}

int
read (int fd, void *buffer, unsigned size) {
	struct thread* curr = thread_current();
	struct file_elem* f_el = file_elem_by_fd(fd);
	int read_size;
	if (f_el == NULL)
		exit(-1);

	if(fd == 0 || f_el->reference == 1) {
		if (f_el->open) {
			lock_acquire(&filesys_lock);
			read_size = input_getc();
			lock_acquire(&filesys_lock);
		}	else
			read_size = 0;
	} else if(fd == 1) {
		exit(-1);
	}	else {
#ifdef VM
		if (!spt_find_page (&thread_current()->spt, pg_round_down(buffer))->writable) {
			exit(-1);
		}
#endif
		lock_acquire(&filesys_lock);
		read_size = file_read(f_el->file, buffer, size);
		lock_release(&filesys_lock);
	}
	return read_size;
}

int
write (int fd, const void *buffer, unsigned size) {
	struct thread* curr = thread_current();
	struct file_elem* f_el = file_elem_by_fd(fd);
	int written_size;

	if (f_el == NULL)
		exit(-1);

	if (fd == 1 || f_el->reference == 1) {
		if (f_el->open) {
			lock_acquire(&filesys_lock);
			putbuf(buffer, size);
			lock_release(&filesys_lock);
		}
		else
			written_size = 0;
	} else if (fd == 0) {
		exit(-1);
	} else {
		lock_acquire(&filesys_lock);
		written_size = file_write(f_el->file, buffer, size);
		lock_release(&filesys_lock);
	}
	return written_size;
}

void
seek (int fd, unsigned position) {
	struct file_elem* f_el = file_elem_by_fd(fd);

	if ((f_el == NULL && position == 0) || (fd == 0 || fd == 1 || f_el->reference == 0 || f_el->reference == 1))
		return;
	if (f_el == NULL || f_el->file == NULL)
		exit(-1);

	lock_acquire(&filesys_lock);
	file_seek(f_el->file, position);
	lock_release(&filesys_lock);
}

unsigned
tell (int fd) {
	struct file_elem* f_el = file_elem_by_fd(fd);

	if (f_el == NULL)
		exit(-1);

	return f_el->file->pos;
}

bool
other_fd_open(struct file_elem* find_f_el) {
	struct thread* curr = thread_current();
	struct file_elem* f_el;
	struct list_elem* el;

	el = list_begin(&curr->files_list);
	while (el != list_end(&curr->files_list)) {
		f_el = list_entry(el, struct file_elem, elem);
		if (f_el->fd != find_f_el->fd && f_el->file == find_f_el->file)
			return true;
		el = el->next;
	}
	return false;
}

void
close_all_std(int fd) {
	struct thread* curr = thread_current();
	struct list_elem* el;
	struct file_elem* f_el;

	el = list_begin(&curr->files_list);
	while (el != list_end(&curr->files_list)) {
		f_el = list_entry(el, struct file_elem, elem);
		if (f_el->fd == fd || f_el->reference == fd)
			f_el->open = false;
		el = el->next;
	}
}

void
close (int fd) {
	struct list_elem* el;
	struct file_elem* f_el = file_elem_by_fd(fd);

	if (f_el == NULL)
		exit(-1);
	if (fd == 0 || fd == 1 || f_el->reference == 0 || f_el->reference == 1) {
		close_all_std((fd == 0 || fd == 1) ? fd : f_el->reference);
	} else {
		if (!other_fd_open(f_el)) {
			lock_acquire(&filesys_lock);
			file_close(f_el->file);
			lock_release(&filesys_lock);
		}
		list_remove(&f_el->elem);
		free(f_el);
	}
}

int
dup2(int oldfd, int newfd) {
	struct thread* curr = thread_current();
	struct file_elem *old_f_el, *new_f_el;

	if (oldfd == NULL || newfd == NULL || oldfd < 0 || newfd < 0)
		return -1;
	if (oldfd == newfd)
		return newfd;

	old_f_el = file_elem_by_fd(oldfd);
	new_f_el = file_elem_by_fd(newfd);
	if (old_f_el == NULL)
		return NULL;
	if (new_f_el != NULL) {
		if (!other_fd_open(new_f_el)) {
			lock_acquire(&filesys_lock);
			file_close(new_f_el->file);
			lock_release(&filesys_lock);
		}
		new_f_el->file = old_f_el->file;
	}	else {
		new_f_el = malloc(sizeof(struct file_elem));
		if (new_f_el == NULL)
			return -1;
		new_f_el->fd = newfd;
		new_f_el->file = old_f_el->file;
		list_insert_ordered(&curr->files_list, &new_f_el->elem, compare_file_elem, NULL);
	}
	if (oldfd == 0 || oldfd == 1 || old_f_el->reference == 0 || old_f_el->reference == 1) {
		new_f_el->reference = (oldfd == 0 || oldfd == 1) ? oldfd : old_f_el->reference;
	} else {
		old_f_el->reference = newfd;
		new_f_el->reference = oldfd;
	}
	new_f_el->open = old_f_el->open;
	return newfd;
}

void *
mmap (void *addr, size_t length, int writable, int fd, off_t offset) {
	struct file_elem* f_el = file_elem_by_fd(fd);

	if (f_el == NULL || fd == 0 || fd == 1 || addr == NULL || is_kernel_vaddr(addr) || 
			length == 0 || !file_length(f_el->file) || offset % PGSIZE != 0)
		return NULL;
	if (pg_round_down(addr) != addr)	// addr is not alligned
		return NULL;
	return do_mmap(addr, length, writable, f_el->file, offset);
}

void
munmap (void *addr) {
	do_munmap(pg_round_down(addr));
}

void
is_valid_user_ptr(void* ptr) {
	if (is_kernel_vaddr(ptr) || ptr == NULL) {
		exit(-1);
	}
}

bool
chdir(const char *dir) {
	bool success = false;

	return success;
}

bool
mkdir(const char *dir) {
	bool success = false;

	return success;
}

bool
readdir(int fd, char *name) {
	bool success = false;

	return success;
}

bool
isdir(int fd) {
	bool success = false;

	return success;
}

bool
inumber(int fd) {
	bool success = false;

	return success;
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	// printf ("system call at %s TYPE: %d\n", thread_current()->name, f->R.rax);
	// thread_current()->recent_rsp = f->rsp;
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
	case SYS_DUP2:
		f->R.rax = dup2(f->R.rdi, f->R.rsi);
		break;
	case SYS_MMAP:
		f->R.rax = mmap (f->R.rdi, f->R.rsi, f->R.rdx, f->R.r10, f->R.r8);
		break;
	case SYS_MUNMAP:
		is_valid_user_ptr(f->R.rdi);
		munmap (f->R.rdi);
		break;
	case SYS_CHDIR:
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = chdir(f->R.rdi);
		break;
	case SYS_MKDIR:
		is_valid_user_ptr(f->R.rdi);
		f->R.rax = mkdir(f->R.rdi);
		break;
	case SYS_READDIR:
		is_valid_user_ptr(f->R.rsi);
		f->R.rax = readdir(f->R.rdi, f->R.rsi);
		break;
	case SYS_ISDIR:
		f->R.rax = isdir(f->R.rdi);
		break;
	case SYS_INUMBER:
		f->R.rax = inumber(f->R.rdi);
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
