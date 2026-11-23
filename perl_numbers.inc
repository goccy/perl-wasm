/* The families of operating-system numbers and, of each, the names the C
 * library of the interpreter defines. Included by perl_numbers.cc with
 * NUMBER_FAMILY(id, family, kind) and NUMBER(family, name) defined. The
 * ids are those of the host import host_number_at. */
NUMBER_FAMILY(1, errno, ENUM)
#ifdef E2BIG
NUMBER(errno, E2BIG)
#endif
#ifdef EACCES
NUMBER(errno, EACCES)
#endif
#ifdef EADDRINUSE
NUMBER(errno, EADDRINUSE)
#endif
#ifdef EADDRNOTAVAIL
NUMBER(errno, EADDRNOTAVAIL)
#endif
#ifdef EAFNOSUPPORT
NUMBER(errno, EAFNOSUPPORT)
#endif
#ifdef EAGAIN
NUMBER(errno, EAGAIN)
#endif
#ifdef EALREADY
NUMBER(errno, EALREADY)
#endif
#ifdef EBADF
NUMBER(errno, EBADF)
#endif
#ifdef EBADMSG
NUMBER(errno, EBADMSG)
#endif
#ifdef EBUSY
NUMBER(errno, EBUSY)
#endif
#ifdef ECANCELED
NUMBER(errno, ECANCELED)
#endif
#ifdef ECHILD
NUMBER(errno, ECHILD)
#endif
#ifdef ECONNABORTED
NUMBER(errno, ECONNABORTED)
#endif
#ifdef ECONNREFUSED
NUMBER(errno, ECONNREFUSED)
#endif
#ifdef ECONNRESET
NUMBER(errno, ECONNRESET)
#endif
#ifdef EDEADLK
NUMBER(errno, EDEADLK)
#endif
#ifdef EDESTADDRREQ
NUMBER(errno, EDESTADDRREQ)
#endif
#ifdef EDOM
NUMBER(errno, EDOM)
#endif
#ifdef EDQUOT
NUMBER(errno, EDQUOT)
#endif
#ifdef EEXIST
NUMBER(errno, EEXIST)
#endif
#ifdef EFAULT
NUMBER(errno, EFAULT)
#endif
#ifdef EFBIG
NUMBER(errno, EFBIG)
#endif
#ifdef EHOSTUNREACH
NUMBER(errno, EHOSTUNREACH)
#endif
#ifdef EIDRM
NUMBER(errno, EIDRM)
#endif
#ifdef EILSEQ
NUMBER(errno, EILSEQ)
#endif
#ifdef EINPROGRESS
NUMBER(errno, EINPROGRESS)
#endif
#ifdef EINTR
NUMBER(errno, EINTR)
#endif
#ifdef EINVAL
NUMBER(errno, EINVAL)
#endif
#ifdef EIO
NUMBER(errno, EIO)
#endif
#ifdef EISCONN
NUMBER(errno, EISCONN)
#endif
#ifdef EISDIR
NUMBER(errno, EISDIR)
#endif
#ifdef ELOOP
NUMBER(errno, ELOOP)
#endif
#ifdef EMFILE
NUMBER(errno, EMFILE)
#endif
#ifdef EMLINK
NUMBER(errno, EMLINK)
#endif
#ifdef EMSGSIZE
NUMBER(errno, EMSGSIZE)
#endif
#ifdef EMULTIHOP
NUMBER(errno, EMULTIHOP)
#endif
#ifdef ENAMETOOLONG
NUMBER(errno, ENAMETOOLONG)
#endif
#ifdef ENETDOWN
NUMBER(errno, ENETDOWN)
#endif
#ifdef ENETRESET
NUMBER(errno, ENETRESET)
#endif
#ifdef ENETUNREACH
NUMBER(errno, ENETUNREACH)
#endif
#ifdef ENFILE
NUMBER(errno, ENFILE)
#endif
#ifdef ENOBUFS
NUMBER(errno, ENOBUFS)
#endif
#ifdef ENODEV
NUMBER(errno, ENODEV)
#endif
#ifdef ENOENT
NUMBER(errno, ENOENT)
#endif
#ifdef ENOEXEC
NUMBER(errno, ENOEXEC)
#endif
#ifdef ENOLCK
NUMBER(errno, ENOLCK)
#endif
#ifdef ENOLINK
NUMBER(errno, ENOLINK)
#endif
#ifdef ENOMEM
NUMBER(errno, ENOMEM)
#endif
#ifdef ENOMSG
NUMBER(errno, ENOMSG)
#endif
#ifdef ENOPROTOOPT
NUMBER(errno, ENOPROTOOPT)
#endif
#ifdef ENOSPC
NUMBER(errno, ENOSPC)
#endif
#ifdef ENOSYS
NUMBER(errno, ENOSYS)
#endif
#ifdef ENOTCONN
NUMBER(errno, ENOTCONN)
#endif
#ifdef ENOTDIR
NUMBER(errno, ENOTDIR)
#endif
#ifdef ENOTEMPTY
NUMBER(errno, ENOTEMPTY)
#endif
#ifdef ENOTRECOVERABLE
NUMBER(errno, ENOTRECOVERABLE)
#endif
#ifdef ENOTSOCK
NUMBER(errno, ENOTSOCK)
#endif
#ifdef ENOTSUP
NUMBER(errno, ENOTSUP)
#endif
#ifdef ENOTTY
NUMBER(errno, ENOTTY)
#endif
#ifdef ENXIO
NUMBER(errno, ENXIO)
#endif
#ifdef EOVERFLOW
NUMBER(errno, EOVERFLOW)
#endif
#ifdef EOWNERDEAD
NUMBER(errno, EOWNERDEAD)
#endif
#ifdef EPERM
NUMBER(errno, EPERM)
#endif
#ifdef EPIPE
NUMBER(errno, EPIPE)
#endif
#ifdef EPROTO
NUMBER(errno, EPROTO)
#endif
#ifdef EPROTONOSUPPORT
NUMBER(errno, EPROTONOSUPPORT)
#endif
#ifdef EPROTOTYPE
NUMBER(errno, EPROTOTYPE)
#endif
#ifdef ERANGE
NUMBER(errno, ERANGE)
#endif
#ifdef EROFS
NUMBER(errno, EROFS)
#endif
#ifdef ESPIPE
NUMBER(errno, ESPIPE)
#endif
#ifdef ESRCH
NUMBER(errno, ESRCH)
#endif
#ifdef ESTALE
NUMBER(errno, ESTALE)
#endif
#ifdef ETIMEDOUT
NUMBER(errno, ETIMEDOUT)
#endif
#ifdef ETXTBSY
NUMBER(errno, ETXTBSY)
#endif
#ifdef EXDEV
NUMBER(errno, EXDEV)
#endif
#ifdef ENOTCAPABLE
NUMBER(errno, ENOTCAPABLE)
#endif
#ifdef EOPNOTSUPP
NUMBER(errno, EOPNOTSUPP)
#endif
#ifdef EWOULDBLOCK
NUMBER(errno, EWOULDBLOCK)
#endif
NUMBER_FAMILY(2, signal, ENUM)
#ifdef SIGHUP
NUMBER(signal, SIGHUP)
#endif
#ifdef SIGINT
NUMBER(signal, SIGINT)
#endif
#ifdef SIGQUIT
NUMBER(signal, SIGQUIT)
#endif
#ifdef SIGILL
NUMBER(signal, SIGILL)
#endif
#ifdef SIGTRAP
NUMBER(signal, SIGTRAP)
#endif
#ifdef SIGABRT
NUMBER(signal, SIGABRT)
#endif
#ifdef SIGIOT
NUMBER(signal, SIGIOT)
#endif
#ifdef SIGBUS
NUMBER(signal, SIGBUS)
#endif
#ifdef SIGFPE
NUMBER(signal, SIGFPE)
#endif
#ifdef SIGKILL
NUMBER(signal, SIGKILL)
#endif
#ifdef SIGUSR1
NUMBER(signal, SIGUSR1)
#endif
#ifdef SIGSEGV
NUMBER(signal, SIGSEGV)
#endif
#ifdef SIGUSR2
NUMBER(signal, SIGUSR2)
#endif
#ifdef SIGPIPE
NUMBER(signal, SIGPIPE)
#endif
#ifdef SIGALRM
NUMBER(signal, SIGALRM)
#endif
#ifdef SIGTERM
NUMBER(signal, SIGTERM)
#endif
#ifdef SIGSTKFLT
NUMBER(signal, SIGSTKFLT)
#endif
#ifdef SIGCHLD
NUMBER(signal, SIGCHLD)
#endif
#ifdef SIGCLD
NUMBER(signal, SIGCLD)
#endif
#ifdef SIGCONT
NUMBER(signal, SIGCONT)
#endif
#ifdef SIGSTOP
NUMBER(signal, SIGSTOP)
#endif
#ifdef SIGTSTP
NUMBER(signal, SIGTSTP)
#endif
#ifdef SIGTTIN
NUMBER(signal, SIGTTIN)
#endif
#ifdef SIGTTOU
NUMBER(signal, SIGTTOU)
#endif
#ifdef SIGURG
NUMBER(signal, SIGURG)
#endif
#ifdef SIGXCPU
NUMBER(signal, SIGXCPU)
#endif
#ifdef SIGXFSZ
NUMBER(signal, SIGXFSZ)
#endif
#ifdef SIGVTALRM
NUMBER(signal, SIGVTALRM)
#endif
#ifdef SIGPROF
NUMBER(signal, SIGPROF)
#endif
#ifdef SIGWINCH
NUMBER(signal, SIGWINCH)
#endif
#ifdef SIGIO
NUMBER(signal, SIGIO)
#endif
#ifdef SIGPOLL
NUMBER(signal, SIGPOLL)
#endif
#ifdef SIGPWR
NUMBER(signal, SIGPWR)
#endif
#ifdef SIGSYS
NUMBER(signal, SIGSYS)
#endif
#ifdef SIGUNUSED
NUMBER(signal, SIGUNUSED)
#endif
#ifdef SIGEMT
NUMBER(signal, SIGEMT)
#endif
#ifdef SIGINFO
NUMBER(signal, SIGINFO)
#endif
NUMBER_FAMILY(3, open, FLAGS)
#ifdef O_RDONLY
NUMBER(open, O_RDONLY)
#endif
#ifdef O_WRONLY
NUMBER(open, O_WRONLY)
#endif
#ifdef O_RDWR
NUMBER(open, O_RDWR)
#endif
#ifdef O_ACCMODE
NUMBER(open, O_ACCMODE)
#endif
#ifdef O_APPEND
NUMBER(open, O_APPEND)
#endif
#ifdef O_CREAT
NUMBER(open, O_CREAT)
#endif
#ifdef O_DIRECTORY
NUMBER(open, O_DIRECTORY)
#endif
#ifdef O_DSYNC
NUMBER(open, O_DSYNC)
#endif
#ifdef O_EXCL
NUMBER(open, O_EXCL)
#endif
#ifdef O_NOCTTY
NUMBER(open, O_NOCTTY)
#endif
#ifdef O_NOFOLLOW
NUMBER(open, O_NOFOLLOW)
#endif
#ifdef O_NONBLOCK
NUMBER(open, O_NONBLOCK)
#endif
#ifdef O_NDELAY
NUMBER(open, O_NDELAY)
#endif
#ifdef O_RSYNC
NUMBER(open, O_RSYNC)
#endif
#ifdef O_SYNC
NUMBER(open, O_SYNC)
#endif
#ifdef O_TRUNC
NUMBER(open, O_TRUNC)
#endif
#ifdef O_CLOEXEC
NUMBER(open, O_CLOEXEC)
#endif
NUMBER_FAMILY(4, poll, FLAGS)
#ifdef POLLIN
NUMBER(poll, POLLIN)
#endif
#ifdef POLLPRI
NUMBER(poll, POLLPRI)
#endif
#ifdef POLLOUT
NUMBER(poll, POLLOUT)
#endif
#ifdef POLLERR
NUMBER(poll, POLLERR)
#endif
#ifdef POLLHUP
NUMBER(poll, POLLHUP)
#endif
#ifdef POLLNVAL
NUMBER(poll, POLLNVAL)
#endif
#ifdef POLLRDNORM
NUMBER(poll, POLLRDNORM)
#endif
#ifdef POLLRDBAND
NUMBER(poll, POLLRDBAND)
#endif
#ifdef POLLWRNORM
NUMBER(poll, POLLWRNORM)
#endif
#ifdef POLLWRBAND
NUMBER(poll, POLLWRBAND)
#endif
NUMBER_FAMILY(5, af, ENUM)
#ifdef AF_UNSPEC
NUMBER(af, AF_UNSPEC)
#endif
#ifdef AF_INET
NUMBER(af, AF_INET)
#endif
#ifdef AF_INET6
NUMBER(af, AF_INET6)
#endif
#ifdef AF_UNIX
NUMBER(af, AF_UNIX)
#endif
#ifdef AF_LOCAL
NUMBER(af, AF_LOCAL)
#endif
NUMBER_FAMILY(6, socktype, ENUM)
#ifdef SOCK_STREAM
NUMBER(socktype, SOCK_STREAM)
#endif
#ifdef SOCK_DGRAM
NUMBER(socktype, SOCK_DGRAM)
#endif
#ifdef SOCK_RAW
NUMBER(socktype, SOCK_RAW)
#endif
#ifdef SOCK_SEQPACKET
NUMBER(socktype, SOCK_SEQPACKET)
#endif
#ifdef SOCK_RDM
NUMBER(socktype, SOCK_RDM)
#endif
NUMBER_FAMILY(7, sockflag, FLAGS)
#ifdef SOCK_NONBLOCK
NUMBER(sockflag, SOCK_NONBLOCK)
#endif
#ifdef SOCK_CLOEXEC
NUMBER(sockflag, SOCK_CLOEXEC)
#endif
NUMBER_FAMILY(8, sol, ENUM)
#ifdef SOL_SOCKET
NUMBER(sol, SOL_SOCKET)
#endif
NUMBER_FAMILY(9, so, ENUM)
#ifdef SO_ACCEPTCONN
NUMBER(so, SO_ACCEPTCONN)
#endif
#ifdef SO_BROADCAST
NUMBER(so, SO_BROADCAST)
#endif
#ifdef SO_DEBUG
NUMBER(so, SO_DEBUG)
#endif
#ifdef SO_DONTROUTE
NUMBER(so, SO_DONTROUTE)
#endif
#ifdef SO_ERROR
NUMBER(so, SO_ERROR)
#endif
#ifdef SO_KEEPALIVE
NUMBER(so, SO_KEEPALIVE)
#endif
#ifdef SO_LINGER
NUMBER(so, SO_LINGER)
#endif
#ifdef SO_OOBINLINE
NUMBER(so, SO_OOBINLINE)
#endif
#ifdef SO_RCVBUF
NUMBER(so, SO_RCVBUF)
#endif
#ifdef SO_RCVLOWAT
NUMBER(so, SO_RCVLOWAT)
#endif
#ifdef SO_RCVTIMEO
NUMBER(so, SO_RCVTIMEO)
#endif
#ifdef SO_REUSEADDR
NUMBER(so, SO_REUSEADDR)
#endif
#ifdef SO_REUSEPORT
NUMBER(so, SO_REUSEPORT)
#endif
#ifdef SO_SNDBUF
NUMBER(so, SO_SNDBUF)
#endif
#ifdef SO_SNDLOWAT
NUMBER(so, SO_SNDLOWAT)
#endif
#ifdef SO_SNDTIMEO
NUMBER(so, SO_SNDTIMEO)
#endif
#ifdef SO_TYPE
NUMBER(so, SO_TYPE)
#endif
#ifdef SO_PROTOCOL
NUMBER(so, SO_PROTOCOL)
#endif
#ifdef SO_DOMAIN
NUMBER(so, SO_DOMAIN)
#endif
NUMBER_FAMILY(10, ip, ENUM)
#ifdef IP_ADD_MEMBERSHIP
NUMBER(ip, IP_ADD_MEMBERSHIP)
#endif
#ifdef IP_ADD_SOURCE_MEMBERSHIP
NUMBER(ip, IP_ADD_SOURCE_MEMBERSHIP)
#endif
#ifdef IP_BIND_ADDRESS_NO_PORT
NUMBER(ip, IP_BIND_ADDRESS_NO_PORT)
#endif
#ifdef IP_DROP_MEMBERSHIP
NUMBER(ip, IP_DROP_MEMBERSHIP)
#endif
#ifdef IP_DROP_SOURCE_MEMBERSHIP
NUMBER(ip, IP_DROP_SOURCE_MEMBERSHIP)
#endif
#ifdef IP_FREEBIND
NUMBER(ip, IP_FREEBIND)
#endif
#ifdef IP_HDRINCL
NUMBER(ip, IP_HDRINCL)
#endif
#ifdef IP_MTU
NUMBER(ip, IP_MTU)
#endif
#ifdef IP_MTU_DISCOVER
NUMBER(ip, IP_MTU_DISCOVER)
#endif
#ifdef IP_MULTICAST_ALL
NUMBER(ip, IP_MULTICAST_ALL)
#endif
#ifdef IP_MULTICAST_IF
NUMBER(ip, IP_MULTICAST_IF)
#endif
#ifdef IP_MULTICAST_LOOP
NUMBER(ip, IP_MULTICAST_LOOP)
#endif
#ifdef IP_MULTICAST_TTL
NUMBER(ip, IP_MULTICAST_TTL)
#endif
#ifdef IP_NODEFRAG
NUMBER(ip, IP_NODEFRAG)
#endif
#ifdef IP_OPTIONS
NUMBER(ip, IP_OPTIONS)
#endif
#ifdef IP_RECVERR
NUMBER(ip, IP_RECVERR)
#endif
#ifdef IP_RECVOPTS
NUMBER(ip, IP_RECVOPTS)
#endif
#ifdef IP_RECVRETOPTS
NUMBER(ip, IP_RECVRETOPTS)
#endif
#ifdef IP_RETOPTS
NUMBER(ip, IP_RETOPTS)
#endif
#ifdef IP_TOS
NUMBER(ip, IP_TOS)
#endif
#ifdef IP_TRANSPARENT
NUMBER(ip, IP_TRANSPARENT)
#endif
#ifdef IP_TTL
NUMBER(ip, IP_TTL)
#endif
NUMBER_FAMILY(11, ipv6, ENUM)
#ifdef IPV6_ADD_MEMBERSHIP
NUMBER(ipv6, IPV6_ADD_MEMBERSHIP)
#endif
#ifdef IPV6_DROP_MEMBERSHIP
NUMBER(ipv6, IPV6_DROP_MEMBERSHIP)
#endif
#ifdef IPV6_JOIN_GROUP
NUMBER(ipv6, IPV6_JOIN_GROUP)
#endif
#ifdef IPV6_LEAVE_GROUP
NUMBER(ipv6, IPV6_LEAVE_GROUP)
#endif
#ifdef IPV6_MTU
NUMBER(ipv6, IPV6_MTU)
#endif
#ifdef IPV6_MTU_DISCOVER
NUMBER(ipv6, IPV6_MTU_DISCOVER)
#endif
#ifdef IPV6_MULTICAST_HOPS
NUMBER(ipv6, IPV6_MULTICAST_HOPS)
#endif
#ifdef IPV6_MULTICAST_IF
NUMBER(ipv6, IPV6_MULTICAST_IF)
#endif
#ifdef IPV6_MULTICAST_LOOP
NUMBER(ipv6, IPV6_MULTICAST_LOOP)
#endif
#ifdef IPV6_RECVERR
NUMBER(ipv6, IPV6_RECVERR)
#endif
#ifdef IPV6_ROUTER_ALERT
NUMBER(ipv6, IPV6_ROUTER_ALERT)
#endif
#ifdef IPV6_TCLASS
NUMBER(ipv6, IPV6_TCLASS)
#endif
#ifdef IPV6_UNICAST_HOPS
NUMBER(ipv6, IPV6_UNICAST_HOPS)
#endif
#ifdef IPV6_V6ONLY
NUMBER(ipv6, IPV6_V6ONLY)
#endif
NUMBER_FAMILY(12, tcp, ENUM)
#ifdef TCP_CC_INFO
NUMBER(tcp, TCP_CC_INFO)
#endif
#ifdef TCP_CONGESTION
NUMBER(tcp, TCP_CONGESTION)
#endif
#ifdef TCP_CORK
NUMBER(tcp, TCP_CORK)
#endif
#ifdef TCP_DEFER_ACCEPT
NUMBER(tcp, TCP_DEFER_ACCEPT)
#endif
#ifdef TCP_FASTOPEN
NUMBER(tcp, TCP_FASTOPEN)
#endif
#ifdef TCP_FASTOPEN_CONNECT
NUMBER(tcp, TCP_FASTOPEN_CONNECT)
#endif
#ifdef TCP_FASTOPEN_KEY
NUMBER(tcp, TCP_FASTOPEN_KEY)
#endif
#ifdef TCP_FASTOPEN_NO_COOKIE
NUMBER(tcp, TCP_FASTOPEN_NO_COOKIE)
#endif
#ifdef TCP_INFO
NUMBER(tcp, TCP_INFO)
#endif
#ifdef TCP_KEEPCNT
NUMBER(tcp, TCP_KEEPCNT)
#endif
#ifdef TCP_KEEPIDLE
NUMBER(tcp, TCP_KEEPIDLE)
#endif
#ifdef TCP_KEEPINTVL
NUMBER(tcp, TCP_KEEPINTVL)
#endif
#ifdef TCP_LINGER2
NUMBER(tcp, TCP_LINGER2)
#endif
#ifdef TCP_MAXSEG
NUMBER(tcp, TCP_MAXSEG)
#endif
#ifdef TCP_MD5SIG
NUMBER(tcp, TCP_MD5SIG)
#endif
#ifdef TCP_MD5SIG_EXT
NUMBER(tcp, TCP_MD5SIG_EXT)
#endif
#ifdef TCP_NODELAY
NUMBER(tcp, TCP_NODELAY)
#endif
#ifdef TCP_NOTSENT_LOWAT
NUMBER(tcp, TCP_NOTSENT_LOWAT)
#endif
#ifdef TCP_QUICKACK
NUMBER(tcp, TCP_QUICKACK)
#endif
#ifdef TCP_SYNCNT
NUMBER(tcp, TCP_SYNCNT)
#endif
#ifdef TCP_TIMESTAMP
NUMBER(tcp, TCP_TIMESTAMP)
#endif
#ifdef TCP_TX_DELAY
NUMBER(tcp, TCP_TX_DELAY)
#endif
#ifdef TCP_USER_TIMEOUT
NUMBER(tcp, TCP_USER_TIMEOUT)
#endif
#ifdef TCP_WINDOW_CLAMP
NUMBER(tcp, TCP_WINDOW_CLAMP)
#endif
NUMBER_FAMILY(13, msg, FLAGS)
#ifdef MSG_PEEK
NUMBER(msg, MSG_PEEK)
#endif
#ifdef MSG_WAITALL
NUMBER(msg, MSG_WAITALL)
#endif
#ifdef MSG_TRUNC
NUMBER(msg, MSG_TRUNC)
#endif
#ifdef MSG_DONTWAIT
NUMBER(msg, MSG_DONTWAIT)
#endif
#ifdef MSG_EOR
NUMBER(msg, MSG_EOR)
#endif
#ifdef MSG_NOSIGNAL
NUMBER(msg, MSG_NOSIGNAL)
#endif
#ifdef MSG_OOB
NUMBER(msg, MSG_OOB)
#endif
#ifdef MSG_DONTROUTE
NUMBER(msg, MSG_DONTROUTE)
#endif
#ifdef MSG_CTRUNC
NUMBER(msg, MSG_CTRUNC)
#endif
NUMBER_FAMILY(14, ai, FLAGS)
#ifdef AI_PASSIVE
NUMBER(ai, AI_PASSIVE)
#endif
#ifdef AI_CANONNAME
NUMBER(ai, AI_CANONNAME)
#endif
#ifdef AI_NUMERICHOST
NUMBER(ai, AI_NUMERICHOST)
#endif
#ifdef AI_V4MAPPED
NUMBER(ai, AI_V4MAPPED)
#endif
#ifdef AI_ALL
NUMBER(ai, AI_ALL)
#endif
#ifdef AI_ADDRCONFIG
NUMBER(ai, AI_ADDRCONFIG)
#endif
#ifdef AI_NUMERICSERV
NUMBER(ai, AI_NUMERICSERV)
#endif
NUMBER_FAMILY(15, ni, FLAGS)
#ifdef NI_NUMERICHOST
NUMBER(ni, NI_NUMERICHOST)
#endif
#ifdef NI_NUMERICSERV
NUMBER(ni, NI_NUMERICSERV)
#endif
#ifdef NI_NOFQDN
NUMBER(ni, NI_NOFQDN)
#endif
#ifdef NI_NAMEREQD
NUMBER(ni, NI_NAMEREQD)
#endif
#ifdef NI_DGRAM
NUMBER(ni, NI_DGRAM)
#endif
NUMBER_FAMILY(16, eai, ENUM)
#ifdef EAI_AGAIN
NUMBER(eai, EAI_AGAIN)
#endif
#ifdef EAI_BADFLAGS
NUMBER(eai, EAI_BADFLAGS)
#endif
#ifdef EAI_FAIL
NUMBER(eai, EAI_FAIL)
#endif
#ifdef EAI_FAMILY
NUMBER(eai, EAI_FAMILY)
#endif
#ifdef EAI_MEMORY
NUMBER(eai, EAI_MEMORY)
#endif
#ifdef EAI_NONAME
NUMBER(eai, EAI_NONAME)
#endif
#ifdef EAI_SERVICE
NUMBER(eai, EAI_SERVICE)
#endif
#ifdef EAI_SOCKTYPE
NUMBER(eai, EAI_SOCKTYPE)
#endif
#ifdef EAI_SYSTEM
NUMBER(eai, EAI_SYSTEM)
#endif
#ifdef EAI_OVERFLOW
NUMBER(eai, EAI_OVERFLOW)
#endif
#ifdef EAI_NODATA
NUMBER(eai, EAI_NODATA)
#endif
#ifdef EAI_ADDRFAMILY
NUMBER(eai, EAI_ADDRFAMILY)
#endif
NUMBER_FAMILY(17, lc, ENUM)
#ifdef LC_ALL
NUMBER(lc, LC_ALL)
#endif
#ifdef LC_COLLATE
NUMBER(lc, LC_COLLATE)
#endif
#ifdef LC_CTYPE
NUMBER(lc, LC_CTYPE)
#endif
#ifdef LC_MESSAGES
NUMBER(lc, LC_MESSAGES)
#endif
#ifdef LC_MONETARY
NUMBER(lc, LC_MONETARY)
#endif
#ifdef LC_NUMERIC
NUMBER(lc, LC_NUMERIC)
#endif
#ifdef LC_TIME
NUMBER(lc, LC_TIME)
#endif
NUMBER_FAMILY(18, sighow, ENUM)
#ifdef SIG_BLOCK
NUMBER(sighow, SIG_BLOCK)
#endif
#ifdef SIG_UNBLOCK
NUMBER(sighow, SIG_UNBLOCK)
#endif
#ifdef SIG_SETMASK
NUMBER(sighow, SIG_SETMASK)
#endif
NUMBER_FAMILY(19, sa, FLAGS)
#ifdef SA_NOCLDSTOP
NUMBER(sa, SA_NOCLDSTOP)
#endif
#ifdef SA_NOCLDWAIT
NUMBER(sa, SA_NOCLDWAIT)
#endif
#ifdef SA_NODEFER
NUMBER(sa, SA_NODEFER)
#endif
#ifdef SA_RESETHAND
NUMBER(sa, SA_RESETHAND)
#endif
#ifdef SA_RESTART
NUMBER(sa, SA_RESTART)
#endif
#ifdef SA_SIGINFO
NUMBER(sa, SA_SIGINFO)
#endif
#ifdef SA_ONSTACK
NUMBER(sa, SA_ONSTACK)
#endif
NUMBER_FAMILY(20, fcntl, ENUM)
#ifdef F_DUPFD
NUMBER(fcntl, F_DUPFD)
#endif
#ifdef F_DUPFD_CLOEXEC
NUMBER(fcntl, F_DUPFD_CLOEXEC)
#endif
#ifdef F_GETFD
NUMBER(fcntl, F_GETFD)
#endif
#ifdef F_SETFD
NUMBER(fcntl, F_SETFD)
#endif
#ifdef F_GETFL
NUMBER(fcntl, F_GETFL)
#endif
#ifdef F_SETFL
NUMBER(fcntl, F_SETFL)
#endif
#ifdef F_GETLK
NUMBER(fcntl, F_GETLK)
#endif
#ifdef F_SETLK
NUMBER(fcntl, F_SETLK)
#endif
#ifdef F_SETLKW
NUMBER(fcntl, F_SETLKW)
#endif
#ifdef F_GETOWN
NUMBER(fcntl, F_GETOWN)
#endif
#ifdef F_SETOWN
NUMBER(fcntl, F_SETOWN)
#endif
NUMBER_FAMILY(21, ioctl, ENUM)
#ifdef FIONREAD
NUMBER(ioctl, FIONREAD)
#endif
#ifdef FIONBIO
NUMBER(ioctl, FIONBIO)
#endif
#ifdef TCGETS
NUMBER(ioctl, TCGETS)
#endif
#ifdef TCSETS
NUMBER(ioctl, TCSETS)
#endif
#ifdef TIOCEXCL
NUMBER(ioctl, TIOCEXCL)
#endif
#ifdef TIOCNXCL
NUMBER(ioctl, TIOCNXCL)
#endif
#ifdef TIOCSCTTY
NUMBER(ioctl, TIOCSCTTY)
#endif
#ifdef TIOCGPGRP
NUMBER(ioctl, TIOCGPGRP)
#endif
#ifdef TIOCSPGRP
NUMBER(ioctl, TIOCSPGRP)
#endif
#ifdef TIOCOUTQ
NUMBER(ioctl, TIOCOUTQ)
#endif
#ifdef TIOCGWINSZ
NUMBER(ioctl, TIOCGWINSZ)
#endif
#ifdef TIOCSWINSZ
NUMBER(ioctl, TIOCSWINSZ)
#endif
#ifdef TIOCNOTTY
NUMBER(ioctl, TIOCNOTTY)
#endif
#ifdef TIOCGSID
NUMBER(ioctl, TIOCGSID)
#endif
#ifdef TIOCGPTN
NUMBER(ioctl, TIOCGPTN)
#endif
#ifdef TIOCSPTLCK
NUMBER(ioctl, TIOCSPTLCK)
#endif
NUMBER_FAMILY(22, langinfo, ENUM)
#ifdef ABDAY_1
NUMBER(langinfo, ABDAY_1)
#endif
#ifdef ABDAY_2
NUMBER(langinfo, ABDAY_2)
#endif
#ifdef ABDAY_3
NUMBER(langinfo, ABDAY_3)
#endif
#ifdef ABDAY_4
NUMBER(langinfo, ABDAY_4)
#endif
#ifdef ABDAY_5
NUMBER(langinfo, ABDAY_5)
#endif
#ifdef ABDAY_6
NUMBER(langinfo, ABDAY_6)
#endif
#ifdef ABDAY_7
NUMBER(langinfo, ABDAY_7)
#endif
#ifdef ABMON_1
NUMBER(langinfo, ABMON_1)
#endif
#ifdef ABMON_2
NUMBER(langinfo, ABMON_2)
#endif
#ifdef ABMON_3
NUMBER(langinfo, ABMON_3)
#endif
#ifdef ABMON_4
NUMBER(langinfo, ABMON_4)
#endif
#ifdef ABMON_5
NUMBER(langinfo, ABMON_5)
#endif
#ifdef ABMON_6
NUMBER(langinfo, ABMON_6)
#endif
#ifdef ABMON_7
NUMBER(langinfo, ABMON_7)
#endif
#ifdef ABMON_8
NUMBER(langinfo, ABMON_8)
#endif
#ifdef ABMON_9
NUMBER(langinfo, ABMON_9)
#endif
#ifdef ABMON_10
NUMBER(langinfo, ABMON_10)
#endif
#ifdef ABMON_11
NUMBER(langinfo, ABMON_11)
#endif
#ifdef ABMON_12
NUMBER(langinfo, ABMON_12)
#endif
#ifdef ALT_DIGITS
NUMBER(langinfo, ALT_DIGITS)
#endif
#ifdef AM_STR
NUMBER(langinfo, AM_STR)
#endif
#ifdef CODESET
NUMBER(langinfo, CODESET)
#endif
#ifdef CRNCYSTR
NUMBER(langinfo, CRNCYSTR)
#endif
#ifdef DAY_1
NUMBER(langinfo, DAY_1)
#endif
#ifdef DAY_2
NUMBER(langinfo, DAY_2)
#endif
#ifdef DAY_3
NUMBER(langinfo, DAY_3)
#endif
#ifdef DAY_4
NUMBER(langinfo, DAY_4)
#endif
#ifdef DAY_5
NUMBER(langinfo, DAY_5)
#endif
#ifdef DAY_6
NUMBER(langinfo, DAY_6)
#endif
#ifdef DAY_7
NUMBER(langinfo, DAY_7)
#endif
#ifdef D_FMT
NUMBER(langinfo, D_FMT)
#endif
#ifdef D_T_FMT
NUMBER(langinfo, D_T_FMT)
#endif
#ifdef ERA
NUMBER(langinfo, ERA)
#endif
#ifdef ERA_D_FMT
NUMBER(langinfo, ERA_D_FMT)
#endif
#ifdef ERA_D_T_FMT
NUMBER(langinfo, ERA_D_T_FMT)
#endif
#ifdef ERA_T_FMT
NUMBER(langinfo, ERA_T_FMT)
#endif
#ifdef MON_1
NUMBER(langinfo, MON_1)
#endif
#ifdef MON_2
NUMBER(langinfo, MON_2)
#endif
#ifdef MON_3
NUMBER(langinfo, MON_3)
#endif
#ifdef MON_4
NUMBER(langinfo, MON_4)
#endif
#ifdef MON_5
NUMBER(langinfo, MON_5)
#endif
#ifdef MON_6
NUMBER(langinfo, MON_6)
#endif
#ifdef MON_7
NUMBER(langinfo, MON_7)
#endif
#ifdef MON_8
NUMBER(langinfo, MON_8)
#endif
#ifdef MON_9
NUMBER(langinfo, MON_9)
#endif
#ifdef MON_10
NUMBER(langinfo, MON_10)
#endif
#ifdef MON_11
NUMBER(langinfo, MON_11)
#endif
#ifdef MON_12
NUMBER(langinfo, MON_12)
#endif
#ifdef NOEXPR
NUMBER(langinfo, NOEXPR)
#endif
#ifdef NOSTR
NUMBER(langinfo, NOSTR)
#endif
#ifdef PM_STR
NUMBER(langinfo, PM_STR)
#endif
#ifdef RADIXCHAR
NUMBER(langinfo, RADIXCHAR)
#endif
#ifdef THOUSEP
NUMBER(langinfo, THOUSEP)
#endif
#ifdef T_FMT
NUMBER(langinfo, T_FMT)
#endif
#ifdef T_FMT_AMPM
NUMBER(langinfo, T_FMT_AMPM)
#endif
#ifdef YESEXPR
NUMBER(langinfo, YESEXPR)
#endif
#ifdef YESSTR
NUMBER(langinfo, YESSTR)
#endif
