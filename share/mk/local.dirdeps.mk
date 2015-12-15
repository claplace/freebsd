# $FreeBSD$
.if !target(_DIRDEP_USE)
# we are the 1st makefile

.if !defined(MK_CLANG)
.include "${SRCTOP}/share/mk/src.opts.mk"
.endif

# DEP_MACHINE is set before we get here, this may not be.
DEP_RELDIR ?= ${RELDIR}

.if ${.TARGETS:Uall:M*/*} && empty(DIRDEPS)
# This little trick let's us do
#
# mk -f dirdeps.mk some/dir.i386,bsd
#
DIRDEPS := ${.TARGETS:M*/*}
${.TARGETS:Nall}: all
.endif

# making universe is special
.if defined(UNIVERSE_GUARD)
# these should be done by now
DIRDEPS_FILTER+= N*.host
.endif

# pseudo machines get no qualification
.for m in host common
M_dep_qual_fixes += C;($m),[^/.,]*$$;\1;
.endfor
#.info M_dep_qual_fixes=${M_dep_qual_fixes}

# Cheat for including src.libnames.mk
__<bsd.init.mk>__:
# Pull in _INTERNALLIBS
.include <src.libnames.mk>

# Host libraries should mostly be excluded from the build so the
# host version in /usr/lib is used.  Internal libraries need to be
# allowed to be built though since they are never installed.
_need_host_libs=
.for lib in ${_INTERNALLIBS}
_need_host_libs+= ${LIB${lib:tu}DIR:S,^${OBJTOP}/,,}
.endfor

N_host_libs:= ${cd ${SRCTOP} && echo lib/lib*:L:sh:${_need_host_libs:${M_ListToSkip}}:${M_ListToSkip}}
DIRDEPS_FILTER.host = \
	${N_host_libs} \
	Ninclude* \
	Nlib/csu* \
	Nlib/[mn]* \
	Ngnu/lib/csu* \
	Ngnu/lib/lib[a-r]* \
	Nusr.bin/xinstall* \


DIRDEPS_FILTER+= \
	Nbin/cat.host \
	${DIRDEPS_FILTER.xtras:U}
.endif

# reset this each time
DIRDEPS_FILTER.xtras=
.if ${DEP_MACHINE:Npkgs*} != ""
DIRDEPS_FILTER.xtras+= Nusr.bin/clang/clang.host
.endif

.if ${DEP_MACHINE} != "host"

# this is how we can handle optional dependencies
.if ${DEP_RELDIR} == "lib/libc"
DIRDEPS += lib/libc_nonshared
.if ${MK_SSP:Uno} != "no" 
DIRDEPS += gnu/lib/libssp/libssp_nonshared
.endif
.else
DIRDEPS_FILTER.xtras+= Nlib/libc_nonshared
.endif

# some optional things
.if ${MK_CTF} == "yes" && ${DEP_RELDIR:Mcddl/usr.bin/ctf*} == ""
DIRDEPS += \
	cddl/usr.bin/ctfconvert.host \
	cddl/usr.bin/ctfmerge.host
.endif

# Bootstrap support.  Give hints to DIRDEPS if there is no Makefile.depend*
# generated yet.  This can be based on things such as SRC files and LIBADD.
# These hints will not factor into the final Makefile.depend as only what is
# used will be added in and handled via [local.]gendirdeps.mk.  This is not
# done for MACHINE=host builds.
# XXX: Include this in local.autodep.mk as well for gendirdeps without filemon.
.if ${RELDIR} == ${DEP_RELDIR} # Only do this for main build target
.for _depfile in ${.MAKE.DEPENDFILE_PREFERENCE:T}
.if !defined(_have_depfile) && exists(${.CURDIR}/${_depfile})
_have_depfile=
.endif
.endfor
.if !defined(_have_depfile)
# KMOD does not use any stdlibs.
.if !defined(KMOD)
# Has C files. The C_DIRDEPS are shared with C++ files as well.
C_DIRDEPS= \
	gnu/lib/csu \
	gnu/lib/libgcc \
	include \
	include/xlocale \
	lib/${CSU_DIR} \
	lib/libc \
	lib/libcompiler_rt \

.if !empty(SRCS:M*.c)
DIRDEPS+= ${C_DIRDEPS}
.endif
# Has C++ files
.if !empty(SRCS:M*.cc) || !empty(SRCS:M*.C) || !empty(SRCS:M*.cpp) || \
    !empty(SRCS:M*.cxx)
DIRDEPS+= ${C_DIRDEPS}
.if ${MK_CLANG} == "yes"
DIRDEPS+= lib/libc++ lib/libcxxrt
.else
DIRDEPS+= gnu/lib/libstdc++ gnu/lib/libsupc++
.endif
# XXX: Clang and GCC always adds -lm currently, even when not needed.
DIRDEPS+= lib/msun
.endif	# CXX
.endif	# !defined(KMOD)
# Has yacc files.
.if !empty(SRCS:M*.y)
DIRDEPS+=	usr.bin/yacc.host
.endif
.if !empty(DPADD)
# Taken from meta.autodep.mk (where it only does something with
# BUILD_AT_LEVEL0, which we don't use).
# This only works for DPADD with full OBJ/SRC paths, which is mostly just
# _INTERNALLIBS.
_DP_DIRDEPS= \
	${DPADD:M${OBJTOP}*:H:tA:C,${OBJTOP}[^/]*/,,:N.:O:u} \
	${DPADD:M${OBJROOT}*:N${OBJTOP}*:N${STAGE_ROOT}/*:H:S,${OBJROOT},,:C,^([^/]+)/(.*),\2.\1,:S,${HOST_TARGET}$,host,:N.*:O:u}
# Resolve the paths to RELDIRs
DIRDEPS+= ${_DP_DIRDEPS:C,^,${SRCTOP}/,:tA:C,^${SRCTOP}/,,}
.endif
.if !empty(LIBADD)
# Also handle LIBADD for non-internal libraries.
.for _lib in ${LIBADD}
_lib${_lib}reldir= ${LIB${_lib:tu}DIR:C,${OBJTOP}/,,}
.if defined(LIB${_lib:tu}DIR) && ${DIRDEPS:M${_lib${_lib}reldir}} == "" && \
    exists(${SRCTOP}/${_lib${_lib}reldir})
DIRDEPS+= ${_lib${_lib}reldir}
.endif
.endfor
.endif	# !empty(LIBADD)
.endif	# no Makefile.depend*
.endif	# ${RELDIR} == ${DEP_RELDIR}

.endif	# ${DEP_MACHINE} != "host"

.if ${MK_STAGING} == "yes"
# we need targets/pseudo/stage to prep the stage tree
.if ${DEP_RELDIR} != "targets/pseudo/stage"
DIRDEPS += targets/pseudo/stage
.endif
.endif

DEP_MACHINE_ARCH = ${MACHINE_ARCH.${DEP_MACHINE}}
CSU_DIR.${DEP_MACHINE_ARCH} ?= csu/${DEP_MACHINE_ARCH}
CSU_DIR := ${CSU_DIR.${DEP_MACHINE_ARCH}}
BOOT_MACHINE_DIR:= ${BOOT_MACHINE_DIR.${DEP_MACHINE}}
KERNEL_NAME:= ${KERNEL_NAME.${DEP_MACHINE}}
