/*
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_ktrace.c	8.2 (Berkeley) 9/23/93
 * $FreeBSD$
 */

#include "opt_ktrace.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysproto.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/fcntl.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/ktrace.h>
#include <sys/malloc.h>
#include <sys/sx.h>
#include <sys/syslog.h>
#include <sys/jail.h>

static MALLOC_DEFINE(M_KTRACE, "KTRACE", "KTRACE");

#ifdef KTRACE
static struct ktr_header *ktrgetheader(int type);
static void ktrwrite(struct vnode *, struct ktr_header *, struct uio *);
static int ktrcanset(struct thread *, struct proc *);
static int ktrsetchildren(struct thread *, struct proc *, int, int, struct vnode *);
static int ktrops(struct thread *, struct proc *, int, int, struct vnode *);


static struct ktr_header *
ktrgetheader(type)
	int type;
{
	register struct ktr_header *kth;
	struct proc *p = curproc;	/* XXX */

	MALLOC(kth, struct ktr_header *, sizeof (struct ktr_header),
		M_KTRACE, M_WAITOK);
	kth->ktr_type = type;
	microtime(&kth->ktr_time);
	kth->ktr_pid = p->p_pid;
	bcopy(p->p_comm, kth->ktr_comm, MAXCOMLEN + 1);
	return (kth);
}

/*
 * MPSAFE
 */
void
ktrsyscall(vp, code, narg, args)
	struct vnode *vp;
	int code, narg;
	register_t args[];
{
	struct	ktr_header *kth;
	struct	ktr_syscall *ktp;
	register int len = offsetof(struct ktr_syscall, ktr_args) +
	    (narg * sizeof(register_t));
	struct proc *p = curproc;	/* XXX */
	register_t *argp;
	int i;

	mtx_lock(&Giant);
	p->p_traceflag |= KTRFAC_ACTIVE;
	kth = ktrgetheader(KTR_SYSCALL);
	MALLOC(ktp, struct ktr_syscall *, len, M_KTRACE, M_WAITOK);
	ktp->ktr_code = code;
	ktp->ktr_narg = narg;
	argp = &ktp->ktr_args[0];
	for (i = 0; i < narg; i++)
		*argp++ = args[i];
	kth->ktr_buffer = (caddr_t)ktp;
	kth->ktr_len = len;
	ktrwrite(vp, kth, NULL);
	FREE(ktp, M_KTRACE);
	FREE(kth, M_KTRACE);
	p->p_traceflag &= ~KTRFAC_ACTIVE;
	mtx_unlock(&Giant);
}

/*
 * MPSAFE
 */
void
ktrsysret(vp, code, error, retval)
	struct vnode *vp;
	int code, error;
	register_t retval;
{
	struct ktr_header *kth;
	struct ktr_sysret ktp;
	struct proc *p = curproc;	/* XXX */

	mtx_lock(&Giant);
	p->p_traceflag |= KTRFAC_ACTIVE;
	kth = ktrgetheader(KTR_SYSRET);
	ktp.ktr_code = code;
	ktp.ktr_error = error;
	ktp.ktr_retval = retval;		/* what about val2 ? */

	kth->ktr_buffer = (caddr_t)&ktp;
	kth->ktr_len = sizeof(struct ktr_sysret);

	ktrwrite(vp, kth, NULL);
	FREE(kth, M_KTRACE);
	p->p_traceflag &= ~KTRFAC_ACTIVE;
	mtx_unlock(&Giant);
}

void
ktrnamei(vp, path)
	struct vnode *vp;
	char *path;
{
	struct ktr_header *kth;
	struct proc *p = curproc;	/* XXX */

	/*
	 * don't let p_tracep get ripped out from under us
	 */
	if (vp)
		VREF(vp);
	p->p_traceflag |= KTRFAC_ACTIVE;
	kth = ktrgetheader(KTR_NAMEI);
	kth->ktr_len = strlen(path);
	kth->ktr_buffer = path;

	ktrwrite(vp, kth, NULL);
	if (vp)
		vrele(vp);
	FREE(kth, M_KTRACE);
	p->p_traceflag &= ~KTRFAC_ACTIVE;
}

void
ktrgenio(vp, fd, rw, uio, error)
	struct vnode *vp;
	int fd;
	enum uio_rw rw;
	struct uio *uio;
	int error;
{
	struct ktr_header *kth;
	struct ktr_genio ktg;
	struct proc *p = curproc;	/* XXX */

	if (error)
		return;

	mtx_lock(&Giant);
	/*
	 * don't let p_tracep get ripped out from under us
	 */
	if (vp)
		VREF(vp);
	p->p_traceflag |= KTRFAC_ACTIVE;
	kth = ktrgetheader(KTR_GENIO);
	ktg.ktr_fd = fd;
	ktg.ktr_rw = rw;
	kth->ktr_buffer = (caddr_t)&ktg;
	kth->ktr_len = sizeof(struct ktr_genio);
	uio->uio_offset = 0;
	uio->uio_rw = UIO_WRITE;

	ktrwrite(vp, kth, uio);
	if (vp)
		vrele(vp);
	FREE(kth, M_KTRACE);
	p->p_traceflag &= ~KTRFAC_ACTIVE;
	mtx_unlock(&Giant);
}

void
ktrpsig(vp, sig, action, mask, code)
	struct vnode *vp;
	int sig;
	sig_t action;
	sigset_t *mask;
	int code;
{
	struct ktr_header *kth;
	struct ktr_psig	kp;
	struct proc *p = curproc;	/* XXX */

	/*
	 * don't let vp get ripped out from under us
	 */
	if (vp)
		VREF(vp);
	p->p_traceflag |= KTRFAC_ACTIVE;
	kth = ktrgetheader(KTR_PSIG);
	kp.signo = (char)sig;
	kp.action = action;
	kp.mask = *mask;
	kp.code = code;
	kth->ktr_buffer = (caddr_t)&kp;
	kth->ktr_len = sizeof (struct ktr_psig);

	ktrwrite(vp, kth, NULL);
	if (vp)
		vrele(vp);
	FREE(kth, M_KTRACE);
	p->p_traceflag &= ~KTRFAC_ACTIVE;
}

void
ktrcsw(vp, out, user)
	struct vnode *vp;
	int out, user;
{
	struct ktr_header *kth;
	struct	ktr_csw kc;
	struct proc *p = curproc;	/* XXX */

	/*
	 * don't let vp get ripped out from under us
	 */
	if (vp)
		VREF(vp);
	p->p_traceflag |= KTRFAC_ACTIVE;
	kth = ktrgetheader(KTR_CSW);
	kc.out = out;
	kc.user = user;
	kth->ktr_buffer = (caddr_t)&kc;
	kth->ktr_len = sizeof (struct ktr_csw);

	ktrwrite(vp, kth, NULL);
	if (vp)
		vrele(vp);
	FREE(kth, M_KTRACE);
	p->p_traceflag &= ~KTRFAC_ACTIVE;
}
#endif

/* Interface and common routines */

/*
 * ktrace system call
 */
#ifndef _SYS_SYSPROTO_H_
struct ktrace_args {
	char	*fname;
	int	ops;
	int	facs;
	int	pid;
};
#endif
/* ARGSUSED */
int
ktrace(td, uap)
	struct thread *td;
	register struct ktrace_args *uap;
{
#ifdef KTRACE
	struct proc *curp = td->td_proc;
	register struct vnode *vp = NULL;
	register struct proc *p;
	struct pgrp *pg;
	int facs = uap->facs & ~KTRFAC_ROOT;
	int ops = KTROP(uap->ops);
	int descend = uap->ops & KTRFLAG_DESCEND;
	int ret = 0;
	int flags, error = 0;
	struct nameidata nd;

	curp->p_traceflag |= KTRFAC_ACTIVE;
	if (ops != KTROP_CLEAR) {
		/*
		 * an operation which requires a file argument.
		 */
		NDINIT(&nd, LOOKUP, NOFOLLOW, UIO_USERSPACE, uap->fname, td);
		flags = FREAD | FWRITE | O_NOFOLLOW;
		error = vn_open(&nd, &flags, 0);
		if (error) {
			curp->p_traceflag &= ~KTRFAC_ACTIVE;
			return (error);
		}
		NDFREE(&nd, NDF_ONLY_PNBUF);
		vp = nd.ni_vp;
		VOP_UNLOCK(vp, 0, td);
		if (vp->v_type != VREG) {
			(void) vn_close(vp, FREAD|FWRITE, td->td_ucred, td);
			curp->p_traceflag &= ~KTRFAC_ACTIVE;
			return (EACCES);
		}
	}
	/*
	 * Clear all uses of the tracefile.
	 */
	if (ops == KTROP_CLEARFILE) {
		sx_slock(&allproc_lock);
		LIST_FOREACH(p, &allproc, p_list) {
			PROC_LOCK(p);
			if (p->p_tracep == vp) {
				if (ktrcanset(td, p) && p->p_tracep == vp) {
					p->p_tracep = NULL;
					p->p_traceflag = 0;
					PROC_UNLOCK(p);
					(void) vn_close(vp, FREAD|FWRITE,
						td->td_ucred, td);
				} else {
					PROC_UNLOCK(p);
					error = EPERM;
				}
			} else
				PROC_UNLOCK(p);
		}
		sx_sunlock(&allproc_lock);
		goto done;
	}
	/*
	 * need something to (un)trace (XXX - why is this here?)
	 */
	if (!facs) {
		error = EINVAL;
		goto done;
	}
	/*
	 * do it
	 */
	if (uap->pid < 0) {
		/*
		 * by process group
		 */
		sx_slock(&proctree_lock);
		pg = pgfind(-uap->pid);
		if (pg == NULL) {
			sx_sunlock(&proctree_lock);
			error = ESRCH;
			goto done;
		}
		/*
		 * ktrops() may call vrele(). Lock pg_members
		 * by the proctree_lock rather than pg_mtx.
		 */
		PGRP_UNLOCK(pg);
		LIST_FOREACH(p, &pg->pg_members, p_pglist)
			if (descend)
				ret |= ktrsetchildren(td, p, ops, facs, vp);
			else
				ret |= ktrops(td, p, ops, facs, vp);
		sx_sunlock(&proctree_lock);
	} else {
		/*
		 * by pid
		 */
		p = pfind(uap->pid);
		if (p == NULL) {
			error = ESRCH;
			goto done;
		}
		PROC_UNLOCK(p);
		/* XXX: UNLOCK above has a race */
		if (descend)
			ret |= ktrsetchildren(td, p, ops, facs, vp);
		else
			ret |= ktrops(td, p, ops, facs, vp);
	}
	if (!ret)
		error = EPERM;
done:
	if (vp != NULL)
		(void) vn_close(vp, FWRITE, td->td_ucred, td);
	curp->p_traceflag &= ~KTRFAC_ACTIVE;
	return (error);
#else
	return ENOSYS;
#endif
}

/*
 * utrace system call
 */
/* ARGSUSED */
int
utrace(td, uap)
	struct thread *td;
	register struct utrace_args *uap;
{

#ifdef KTRACE
	struct ktr_header *kth;
	struct proc *p = curproc;	/* XXX */
	struct vnode *vp;
	register caddr_t cp;

	if (!KTRPOINT(p, KTR_USER))
		return (0);
	if (uap->len > KTR_USER_MAXLEN)
		return (EINVAL);
	p->p_traceflag |= KTRFAC_ACTIVE;
	if ((vp = p->p_tracep) != NULL)
		VREF(vp);
	kth = ktrgetheader(KTR_USER);
	MALLOC(cp, caddr_t, uap->len, M_KTRACE, M_WAITOK);
	if (!copyin(uap->addr, cp, uap->len)) {
		kth->ktr_buffer = cp;
		kth->ktr_len = uap->len;
		ktrwrite(vp, kth, NULL);
	}
	if (vp)
		vrele(vp);
	FREE(kth, M_KTRACE);
	FREE(cp, M_KTRACE);
	p->p_traceflag &= ~KTRFAC_ACTIVE;

	return (0);
#else
	return (ENOSYS);
#endif
}

#ifdef KTRACE
static int
ktrops(td, p, ops, facs, vp)
	struct thread *td;
	struct proc *p;
	int ops, facs;
	struct vnode *vp;
{
	struct vnode *vtmp = NULL, *newvp = NULL;

	PROC_LOCK(p);
	if (!ktrcanset(td, p)) {
		PROC_UNLOCK(p);
		return (0);
	}
	if (ops == KTROP_SET) {
		if (p->p_tracep != vp) {
			struct vnode *vtmp;

			/*
			 * if trace file already in use, relinquish below
			 */
			newvp = vp;
			vtmp = p->p_tracep;
			p->p_tracep = NULL;
		}
		p->p_traceflag |= facs;
		if (td->td_ucred->cr_uid == 0)
			p->p_traceflag |= KTRFAC_ROOT;
	} else {
		/* KTROP_CLEAR */
		if (((p->p_traceflag &= ~facs) & KTRFAC_MASK) == 0) {
			struct vnode *vtmp;

			/* no more tracing */
			p->p_traceflag = 0;
			vtmp = p->p_tracep;
			p->p_tracep = NULL;
		}
	}
	PROC_UNLOCK(p);

	/* Release old trace file if requested. */
	if (vtmp != NULL)
		vrele(vtmp);

	/* Setup new trace file if requested. */
	/*
	 * XXX: Doing this before the PROC_UNLOCK above would result in
	 * fewer lock operations but would break old behavior where the
	 * above vrele() would not be traced when changing trace files.
	 */
	if (newvp != NULL) {
		VREF(newvp);
		PROC_LOCK(p);
		p->p_tracep = newvp;
		PROC_UNLOCK(p);
	}
			
	return (1);
}

static int
ktrsetchildren(td, top, ops, facs, vp)
	struct thread *td;
	struct proc *top;
	int ops, facs;
	struct vnode *vp;
{
	register struct proc *p;
	register int ret = 0;

	p = top;
	sx_slock(&proctree_lock);
	for (;;) {
		ret |= ktrops(td, p, ops, facs, vp);
		/*
		 * If this process has children, descend to them next,
		 * otherwise do any siblings, and if done with this level,
		 * follow back up the tree (but not past top).
		 */
		if (!LIST_EMPTY(&p->p_children))
			p = LIST_FIRST(&p->p_children);
		else for (;;) {
			if (p == top) {
				sx_sunlock(&proctree_lock);
				return (ret);
			}
			if (LIST_NEXT(p, p_sibling)) {
				p = LIST_NEXT(p, p_sibling);
				break;
			}
			p = p->p_pptr;
		}
	}
	/*NOTREACHED*/
}

static void
ktrwrite(vp, kth, uio)
	struct vnode *vp;
	register struct ktr_header *kth;
	struct uio *uio;
{
	struct uio auio;
	struct iovec aiov[2];
	struct thread *td = curthread;	/* XXX */
	struct proc *p = td->td_proc;	/* XXX */
	struct mount *mp;
	int error;

	if (vp == NULL)
		return;
	auio.uio_iov = &aiov[0];
	auio.uio_offset = 0;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_rw = UIO_WRITE;
	aiov[0].iov_base = (caddr_t)kth;
	aiov[0].iov_len = sizeof(struct ktr_header);
	auio.uio_resid = sizeof(struct ktr_header);
	auio.uio_iovcnt = 1;
	auio.uio_td = curthread;
	if (kth->ktr_len > 0) {
		auio.uio_iovcnt++;
		aiov[1].iov_base = kth->ktr_buffer;
		aiov[1].iov_len = kth->ktr_len;
		auio.uio_resid += kth->ktr_len;
		if (uio != NULL)
			kth->ktr_len += uio->uio_resid;
	}
	vn_start_write(vp, &mp, V_WAIT);
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY, td);
	(void)VOP_LEASE(vp, td, td->td_ucred, LEASE_WRITE);
	error = VOP_WRITE(vp, &auio, IO_UNIT | IO_APPEND, td->td_ucred);
	if (error == 0 && uio != NULL) {
		(void)VOP_LEASE(vp, td, td->td_ucred, LEASE_WRITE);
		error = VOP_WRITE(vp, uio, IO_UNIT | IO_APPEND, td->td_ucred);
	}
	VOP_UNLOCK(vp, 0, td);
	vn_finished_write(mp);
	if (!error)
		return;
	/*
	 * If error encountered, give up tracing on this vnode.  XXX what
	 * happens to the loop if vrele() blocks?
	 */
	log(LOG_NOTICE, "ktrace write failed, errno %d, tracing stopped\n",
	    error);
	sx_slock(&allproc_lock);
	LIST_FOREACH(p, &allproc, p_list) {
		if (p->p_tracep == vp) {
			p->p_tracep = NULL;
			p->p_traceflag = 0;
			vrele(vp);
		}
	}
	sx_sunlock(&allproc_lock);
}

/*
 * Return true if caller has permission to set the ktracing state
 * of target.  Essentially, the target can't possess any
 * more permissions than the caller.  KTRFAC_ROOT signifies that
 * root previously set the tracing status on the target process, and
 * so, only root may further change it.
 */
static int
ktrcanset(td, targetp)
	struct thread *td;
	struct proc *targetp;
{

	PROC_LOCK_ASSERT(targetp, MA_OWNED);
	if (targetp->p_traceflag & KTRFAC_ROOT &&
	    suser_cred(td->td_ucred, PRISON_ROOT))
		return (0);

	if (p_candebug(td, targetp) != 0)
		return (0);

	return (1);
}

#endif /* KTRACE */
