/* Implement entry point to select system call.
 *
 * The entry points into this file are
 *   do_select:	       perform the SELECT system call
 *   select_callback:  notify select system of possible fd operation 
 *   select_notified:  low-level entry for device notifying select
 */


 /* TODO: check if close (pipe?) / exit works; 
  * some printf()s are serious errors;
  * check combinations of cases listen in open group select
  * spec (various NULLs and behaviours);
  * pty support in tty
  * make select cancel disappearing fp's
  */

#define DEBUG_SELECT 1

#include "fs.h"
#include "select.h"
#include "file.h"
#include "inode.h"
#include "fs_timers.h"

#include <sys/time.h>
#include <sys/select.h>
#include <minix/com.h>
#include <string.h>

/* max. number of simultaneously pending select() calls */
#define MAXSELECTS 25

PRIVATE struct selectentry {
	struct fproc *requestor;	/* slot is free iff this is NULL */
	int req_procnr;
	fd_set readfds, writefds, errorfds;
	fd_set ready_readfds, ready_writefds, ready_errorfds;
	fd_set *vir_readfds, *vir_writefds, *vir_errorfds;
	struct filp *filps[FD_SETSIZE];
	int type[FD_SETSIZE];
	int nfds, nreadyfds;
	clock_t expiry;
	timer_t timer;	/* if expiry > 0 */
} selecttab[MAXSELECTS];

#define SELFD_FILE	0
#define SELFD_PIPE	1
#define SELFD_TTY	2
#define SELFD_INET	3
#define SEL_FDS		4

FORWARD _PROTOTYPE(int select_reevaluate, (struct filp *fp));

FORWARD _PROTOTYPE(int select_request_file, (struct filp *f, int *ops, int block));
FORWARD _PROTOTYPE(int select_match_file, (struct filp *f));

FORWARD _PROTOTYPE(int select_request_tty, (struct filp *f, int *ops, int block));
FORWARD _PROTOTYPE(int select_match_tty, (struct filp *f));

FORWARD _PROTOTYPE(int select_request_inet, (struct filp *f, int *ops, int block));
FORWARD _PROTOTYPE(int select_match_inet, (struct filp *f));

FORWARD _PROTOTYPE(void select_cancel_all, (struct selectentry *e));
FORWARD _PROTOTYPE(int select_wakeup, (struct selectentry *e));

/* The Open Group:
 * "The pselect() and select() functions shall support
 * regular files, terminal and pseudo-terminal devices,
 * STREAMS-based files, FIFOs, pipes, and sockets."
 */

PRIVATE struct fdtype {
	int (*select_request)(struct filp *, int *ops, int block);	
	int (*select_match)(struct filp *);
} fdtypes[SEL_FDS] = {
		/* SELFD_FILE */
	{ select_request_file, select_match_file },
		/* SELFD_TTY (also PTY) */
	{ select_request_tty, select_match_tty },
		/* SELFD_INET */
	{ select_request_inet, select_match_inet },
		/* SELFD_PIPE (pipe(2) pipes and FS FIFOs) */
	{ select_request_pipe, select_match_pipe },
};

/* Open Group:
 * "File descriptors associated with regular files shall always select true
 * for ready to read, ready to write, and error conditions."
 */

/*===========================================================================*
 *				select_request_file			     *
 *===========================================================================*/
PRIVATE int select_request_file(struct filp *f, int *ops, int block)
{
	/* output *ops is input *ops */
	return SEL_OK;
}

/*===========================================================================*
 *				select_match_file			     *
 *===========================================================================*/
PRIVATE int select_match_file(struct filp *file)
{
	if(file && file->filp_ino && (file->filp_ino->i_mode & I_REGULAR))
		return 1;
	return 0;
}

/*===========================================================================*
 *				select_request_tty			     *
 *===========================================================================*/
PRIVATE int select_request_tty(struct filp *f, int *ops, int block)
{
	int r, rops;
	rops = *ops;
	if(block) rops |= SEL_NOTIFY;
	*ops = dev_io(DEV_SELECT, f->filp_ino->i_zone[0], rops, NULL, 0, 0, 0);
	if(*ops < 0)
		return SEL_ERR;
	return SEL_OK;
}

/*===========================================================================*
 *				select_match_tty			     *
 *===========================================================================*/
PRIVATE int select_match_tty(struct filp *file)
{
	int major;
	if(!(file && file->filp_ino &&
		(file->filp_ino->i_mode & I_TYPE) == I_CHAR_SPECIAL))
		return 0;
	major = (file->filp_ino->i_zone[0] >> MAJOR) & BYTE;
	if(major == TTY_MAJOR || major == CTTY_MAJOR)
		return 1;
	return 0;
}

/*===========================================================================*
 *				select_request_inet			     *
 *===========================================================================*/
PRIVATE int select_request_inet(struct filp *f, int *ops, int block)
{
	int r, rops;
	rops = *ops;
	if(block) rops |= SEL_NOTIFY;
	*ops = dev_io(DEV_SELECT, f->filp_ino->i_zone[0], rops, NULL, 0, 0, 0);
	if(*ops < 0)
		return SEL_ERR;
	return SEL_OK;
}

/*===========================================================================*
 *				select_match_inet			     *
 *===========================================================================*/
PRIVATE int select_match_inet(struct filp *file)
{
	int major;
	if(!(file && file->filp_ino &&
		(file->filp_ino->i_mode & I_TYPE) == I_CHAR_SPECIAL))
		return 0;
	major = (file->filp_ino->i_zone[0] >> MAJOR) & BYTE;
	if(major == INET_MAJOR)
		printf("inet minor: %d\n", 
			(file->filp_ino->i_zone[0] & BYTE));
		/* return 1; */
	return 0;
}


PRIVATE int tab2ops(int fd, struct selectentry *e)
{
	return (FD_ISSET(fd, &e->readfds) ? SEL_RD : 0) |
		(FD_ISSET(fd, &e->writefds) ? SEL_WR : 0) |
		(FD_ISSET(fd, &e->errorfds) ? SEL_ERR : 0);
}

PRIVATE void ops2tab(int ops, int fd, struct selectentry *e)
{
	if((ops & SEL_RD) && e->vir_readfds && FD_ISSET(fd, &e->readfds)
		&& !FD_ISSET(fd, &e->ready_readfds)) {
		FD_SET(fd, &e->ready_readfds);
		e->nreadyfds++;
	}
	if((ops & SEL_WR) && e->vir_writefds && FD_ISSET(fd, &e->writefds) 
		&& !FD_ISSET(fd, &e->ready_writefds)) {
		FD_SET(fd, &e->ready_writefds);
		e->nreadyfds++;
	}
	if((ops & SEL_ERR) && e->vir_errorfds && FD_ISSET(fd, &e->errorfds)
		&& !FD_ISSET(fd, &e->ready_errorfds)) {
		FD_SET(fd, &e->ready_errorfds);
		e->nreadyfds++;
	}

	return;
}

PRIVATE void copy_fdsets(struct selectentry *e)
{
	if(e->vir_readfds)
		sys_vircopy(SELF, D, (vir_bytes) &e->ready_readfds,
		e->req_procnr, D, (vir_bytes) e->vir_readfds, sizeof(fd_set));
	if(e->vir_writefds)
		sys_vircopy(SELF, D, (vir_bytes) &e->ready_writefds,
		e->req_procnr, D, (vir_bytes) e->vir_writefds, sizeof(fd_set));
	if(e->vir_errorfds)
		sys_vircopy(SELF, D, (vir_bytes) &e->ready_errorfds,
		e->req_procnr, D, (vir_bytes) e->vir_errorfds, sizeof(fd_set));

	return;
}

/*===========================================================================*
 *				do_select					     *
 *===========================================================================*/
PUBLIC int do_select(void)
{
	int r, nfds, is_timeout = 1, nonzero_timeout = 0,
		fd, s, block = 0;
	struct timeval timeout;
	nfds = m_in.SEL_NFDS;

	if(nfds < 0 || nfds > FD_SETSIZE)
		return EINVAL;

	for(s = 0; s < MAXSELECTS; s++)
		if(!selecttab[s].requestor)
			break;

	if(s >= MAXSELECTS)
		return ENOSPC;

	selecttab[s].req_procnr = who;
	selecttab[s].nfds = 0;
	selecttab[s].nreadyfds = 0;
	memset(selecttab[s].filps, 0, sizeof(selecttab[s].filps));

	/* defaults */
	FD_ZERO(&selecttab[s].readfds);
	FD_ZERO(&selecttab[s].writefds);
	FD_ZERO(&selecttab[s].errorfds);
	FD_ZERO(&selecttab[s].ready_readfds);
	FD_ZERO(&selecttab[s].ready_writefds);
	FD_ZERO(&selecttab[s].ready_errorfds);

	selecttab[s].vir_readfds = (fd_set *) m_in.SEL_READFDS;
	selecttab[s].vir_writefds = (fd_set *) m_in.SEL_WRITEFDS;
	selecttab[s].vir_errorfds = (fd_set *) m_in.SEL_ERRORFDS;

	/* copy args */
	if(selecttab[s].vir_readfds && (r=sys_vircopy(who, D, (vir_bytes) m_in.SEL_READFDS,
		SELF, D, (vir_bytes) &selecttab[s].readfds, sizeof(fd_set))) != OK)
		return r;

	if(selecttab[s].vir_writefds && (r=sys_vircopy(who, D, (vir_bytes) m_in.SEL_WRITEFDS,
		SELF, D, (vir_bytes) &selecttab[s].writefds, sizeof(fd_set))) != OK)
		return r;

	if(selecttab[s].vir_errorfds && (r=sys_vircopy(who, D, (vir_bytes) m_in.SEL_ERRORFDS,
		SELF, D, (vir_bytes) &selecttab[s].errorfds, sizeof(fd_set))) != OK)
		return r;

	if(!m_in.SEL_TIMEOUT)
		is_timeout = nonzero_timeout = 0;
	else
		if((r=sys_vircopy(who, D, (vir_bytes) m_in.SEL_TIMEOUT,
			SELF, D, (vir_bytes) &timeout, sizeof(timeout))) != OK)
			return r;

	/* No nonsense in the timeval please. */
	if(is_timeout && (timeout.tv_sec < 0 || timeout.tv_usec < 0))
		return EINVAL;

	/* if is_timeout if 0, we block forever. otherwise, if nonzero_timeout
	 * is 0, we do a poll (don't block). otherwise, we block up to the
	 * specified time interval.
	 */
	if(is_timeout && (timeout.tv_sec > 0 || timeout.tv_usec > 0))
		nonzero_timeout = 1;

	if(nonzero_timeout || !is_timeout)
		block = 1;
	else
		block = 0; /* timeout set as (0,0) - this effects a poll */

	/* no timeout set (yet) */
	selecttab[s].expiry = 0;

	for(fd = 0; fd < nfds; fd++) {
		int orig_ops, ops, t, type = -1, r;
		struct filp *filp;
	
		if(!(orig_ops = ops = tab2ops(fd, &selecttab[s])))
			continue;
		if(!(filp = selecttab[s].filps[fd] = get_filp(fd))) {
			select_cancel_all(&selecttab[s]);
			return EBADF;
		}

		for(t = 0; t < SEL_FDS; t++) {
			if(fdtypes[t].select_match(filp)) {
#if DEBUG_SELECT
				printf("select: fd %d is type %d ", fd, t);
#endif
				if(type != -1)
					printf("select: double match\n");
				type = t;
			}
		}

		/* Open Group:
		 * "The pselect() and select() functions shall support
		 * regular files, terminal and pseudo-terminal devices,
		 * STREAMS-based files, FIFOs, pipes, and sockets. The
		 * behavior of pselect() and select() on file descriptors
		 * that refer to other types of file is unspecified."
		 *
		 * If all types are implemented, then this is another
		 * type of file and we get to do whatever we want.
		 */
		if(type == -1)
			return EBADF;

		selecttab[s].type[fd] = type;

		if((selecttab[s].filps[fd]->filp_select_ops & ops) != ops) {
			int wantops;
			/* Request the select on this fd.  */
#if DEBUG_SELECT
			printf("%p requesting ops %d -> ",
				selecttab[s].filps[fd],
				selecttab[s].filps[fd]->filp_select_ops);
#endif
			wantops = (selecttab[s].filps[fd]->filp_select_ops |= ops);
#if DEBUG_SELECT
			printf("%d\n", selecttab[s].filps[fd]->filp_select_ops);
#endif
			if((r = fdtypes[type].select_request(filp,
				&wantops, block)) != SEL_OK) {
				/* error or bogus return code.. backpaddle */
				select_cancel_all(&selecttab[s]);
				printf("select: select_request returned error\n");
				return EINVAL;
			}
			if(wantops) {
				if(wantops & ops) {
					/* operations that were just requested
					 * are ready to go right away
					 */
					ops2tab(wantops, fd, &selecttab[s]);
				}
				/* if there are any other select()s blocking
				 * on these operations of this fp, they can
				 * be awoken too
				 */
				select_callback(filp, ops);
			}
#if DEBUG_SELECT
			printf("select request ok; ops returned %d\n", wantops);
#endif
		} else {
#if DEBUG_SELECT
			printf("select already happening on that filp\n");
#endif
		}

		selecttab[s].nfds = fd+1;
		selecttab[s].filps[fd]->filp_selectors++;

#if DEBUG_SELECT
		printf("[fd %d ops: %d] ", fd, ops);
#endif
	}

	if(selecttab[s].nreadyfds > 0 || !block) {
		/* fd's were found that were ready to go right away, and/or
		 * we were instructed not to block at all. Must return
		 * immediately.
		 */
		copy_fdsets(&selecttab[s]);
		select_cancel_all(&selecttab[s]);
		selecttab[s].requestor = NULL;

		/* Open Group:
		 * "Upon successful completion, the pselect() and select()
		 * functions shall return the total number of bits
		 * set in the bit masks."
		 */

		return selecttab[s].nreadyfds;
	}
 
	/* Convert timeval to ticks and set the timer. If it fails, undo
	 * all, return error.
	 */
	if(is_timeout) {
		int ticks;
		/* Open Group:
		 * "If the requested timeout interval requires a finer
		 * granularity than the implementation supports, the
		 * actual timeout interval shall be rounded up to the next
		 * supported value."
		 */
#define USECPERSEC 1000000
		while(timeout.tv_usec >= USECPERSEC) {
			/* this is to avoid overflow with *HZ below */
			timeout.tv_usec -= USECPERSEC;
			timeout.tv_sec++;
		}
		ticks = timeout.tv_sec * HZ +
			(timeout.tv_usec * HZ + USECPERSEC-1) / USECPERSEC;
		selecttab[s].expiry = ticks;
		fs_set_timer(&selecttab[s].timer, ticks, select_timeout_check, s);
#if DEBUG_SELECT
		printf("%d: blocking %d ticks\n", s, ticks);
#endif
	}

	/* if we're blocking, the table entry is now valid. */
	selecttab[s].requestor = fp;

	/* process now blocked */
	suspend(XSELECT);
	return SUSPEND;
}

PRIVATE void select_cancel_all(struct selectentry *e)
{
	int fd;

	for(fd = 0; fd < e->nfds; fd++) {
		struct filp *fp;
		fp = e->filps[fd];
		if(!fp) {
#if DEBUG_SELECT
			printf("[ fd %d/%d NULL ] ", fd, e->nfds);
#endif
			continue;
		}
		if(fp->filp_selectors < 1) {
#if DEBUG_SELECT
			printf("select: %d selectors?!\n", fp->filp_selectors);
#endif
			continue;
		}
		fp->filp_selectors--;
		e->filps[fd] = NULL;
		select_reevaluate(fp);
	}

	if(e->expiry > 0) {
#if DEBUG_SELECT
		printf("cancelling timer %d\n", e - selecttab);
#endif
		fs_cancel_timer(&e->timer); 
		e->expiry = 0;
	}

	return;
}

PRIVATE int select_wakeup(struct selectentry *e)
{
	/* Open Group:
	 * "Upon successful completion, the pselect() and select()
	 * functions shall return the total number of bits
	 * set in the bit masks."
	 */
	revive(e->req_procnr, e->nreadyfds);
	return;
}

PRIVATE int select_reevaluate(struct filp *fp)
{
	int r;
	int s, remain_ops = 0, fd, type = -1;
	int want_ops;

	if(!fp) {
		printf("fs: select: reevalute NULL fp\n");
		return 0;
	}

	for(s = 0; s < MAXSELECTS; s++) {
		if(!selecttab[s].requestor)
			continue;
		for(fd = 0; fd < selecttab[s].nfds; fd++)
			if(fp == selecttab[s].filps[fd]) {
				remain_ops |= tab2ops(fd, &selecttab[s]);
				type = selecttab[s].type[fd];
			}
	}

	/* If there are any select()s open that want any operations on
	 * this fd that haven't been satisfied by this callback, then we're
	 * still in the market for it.
	 */
	fp->filp_select_ops = remain_ops;
#if DEBUG_SELECT
	printf("remaining operations on fp are %d\n", fp->filp_select_ops);
#endif

	return remain_ops;
}

/*===========================================================================*
 *				int select_callback			     *
 *===========================================================================*/
PUBLIC int select_callback(struct filp *fp, int ops)
{
	int s, f, fd, want_ops, remain_ops, type;

	/* We are being notified that file pointer fp is available for
	 * operations 'ops'. We must re-register the select for
	 * operations that we are still interested in, if any.
	 */

restart_callback:
	want_ops = 0;
	type = -1;
	for(s = 0; s < MAXSELECTS; s++) {
		int wakehim = 0;
		if(!selecttab[s].requestor)
			continue;
		for(fd = 0; fd < selecttab[s].nfds; fd++) {
			if(!selecttab[s].filps[fd])
				continue;
			if(selecttab[s].filps[fd] == fp) {
				int this_want_ops;
				this_want_ops = tab2ops(fd, &selecttab[s]);
				want_ops |= this_want_ops;
				if(this_want_ops & ops) {
					/* this select() has been satisfied. */
					ops2tab(ops, fd, &selecttab[s]);
					wakehim = 1;
				}
				type = selecttab[s].type[fd];
			}
		}
		if(wakehim) {
			select_cancel_all(&selecttab[s]);
			copy_fdsets(&selecttab[s]);
			selecttab[s].requestor = NULL;
			select_wakeup(&selecttab[s]);
		}
	}

	return 0;
}

/*===========================================================================*
 *				int select_notified			     *
 *===========================================================================*/
PUBLIC int select_notified(message *m)
{
	int s, f;

	switch(m->m_source) {
		case TTY:
#if DEBUG_SELECT
			printf("fs: select: tty notification\n");
#endif
			for(s = 0; s < MAXSELECTS; s++) {
				int line, ops;
				if(!selecttab[s].requestor)
					continue;
				for(f = 0; f < selecttab[s].nfds; f++) {
					if(!selecttab[s].filps[f] ||
					   !select_match_tty(selecttab[s].filps[f]))
					   	continue;
					ops = tab2ops(f, &selecttab[s]);
					line = selecttab[s].filps[f]->filp_ino->i_zone[0] & BYTE;
					if((line == m->NOTIFY_ARG) &&
						(m->NOTIFY_FLAGS & ops)) {
#if DEBUG_SELECT
						printf("fs: select: tty notification matched\n");
#endif
						select_callback(selecttab[s].filps[f], ops);
					}
				}
			}
			break;
		default:
			printf("fs: select: unrecognized select reply\n");
	}
	return OK;
}

/*===========================================================================*
 *				int select_forget			     *
 *===========================================================================*/
PUBLIC void select_forget(int proc)
{
	/* something has happened (e.g. signal delivered that interrupts
	 * select()). totally forget about the select().
	 */
	int s;

	for(s = 0; s < MAXSELECTS; s++) {
		if(selecttab[s].requestor &&
			selecttab[s].req_procnr == proc) {
			break;
		}

	}

	if(s >= MAXSELECTS) {
		printf("select: cancelled select() not found");
		return;
	}

	select_cancel_all(&selecttab[s]);
	selecttab[s].requestor = NULL;

	return;
}

/*===========================================================================*
 *				int select_timeout_check	  	     *
 *===========================================================================*/
PUBLIC void select_timeout_check(timer_t *timer)
{
	int s, r;
	clock_t now;

	s = tmr_arg(timer)->ta_int;

	if(s < 0 || s >= MAXSELECTS) {
		printf("select: bogus slot arg to watchdog %d\n", s);
		return;
	}

	if(!selecttab[s].requestor) {
		printf("select: no requestor in watchdog\n");
		return;
	}

	if(selecttab[s].expiry <= 0) {
		printf("select: strange expiry value in watchdog\n", s);
		return;
	}

	selecttab[s].expiry = 0;
	copy_fdsets(&selecttab[s]);
	select_cancel_all(&selecttab[s]);
	selecttab[s].requestor = NULL;
	select_wakeup(&selecttab[s]);

	return;
}
