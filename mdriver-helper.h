/**
 * @file mdriver-helper.h
 * @brief Async-safe helper functions for mdriver
 *
 * This file consists of the SIO (safe I/O) package, which implements an async-signal-safe variant
 * of printf and related calls. It also includes a signal call, which is a wrapper around sigaction.
 *
 **/


 #ifndef CSAPP_H
 #define CSAPP_H
 
 #include <stdarg.h>    /* va_list */
 #include <stddef.h>    /* size_t */
 #include <sys/types.h> /* ssize_t */
 
 /* C2023, C2011, and historical GCC (also supported by Clang) each have
  * a different way to annotate a function as never returning to its
  * caller. We're going to keep using the historical GCC way for now,
  * because that involves fewer #ifdefs. There's no plausible prospect
  * of any compiler that supports this dropping it again.
  */
 #ifdef __GNUC__
 #define NORETURN void __attribute__ ((__noreturn__))
 #else
 #define NORETURN void
 #endif
 
 /* Default file permissions are DEF_MODE & ~DEF_UMASK */
 #define DEF_MODE (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
 #define DEF_UMASK (S_IWGRP | S_IWOTH)
 
 /* Persistent state for the robust I/O (Rio) package */
 #define RIO_BUFSIZE 8192
 typedef struct {
     int rio_fd;                /* Descriptor for this internal buf */
     ssize_t rio_cnt;           /* Unread bytes in internal buf */
     char *rio_bufptr;          /* Next unread byte in internal buf */
     char rio_buf[RIO_BUFSIZE]; /* Internal buffer */
 } rio_t;
 
 /* Misc constants */
 #define MAXTEXTLINE 8192 /* Max text line length */
 #define MAXBUF 8192  /* Max I/O buffer size */
 #define LISTENQ 1024 /* Second argument to listen() */
 
 /* Signal wrappers */
 typedef void handler_t(int);
 handler_t *Signal(int signum, handler_t *handler);
 
 /* Sio (Signal-safe I/O) routines */
 ssize_t sio_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
 ssize_t sio_dprintf(int fileno, const char *fmt, ...)
     __attribute__((format(printf, 2, 3)));
 ssize_t sio_eprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
 ssize_t sio_vdprintf(int fileno, const char *fmt, va_list argp)
     __attribute__((format(printf, 2, 0)));
 
 #define sio_assert(expr)                                                       \
     ((expr) ? (void)0 : __sio_assert_fail(#expr, __FILE__, __LINE__, __func__))
 
 NORETURN __sio_assert_fail(const char *assertion, const char *file,
                            unsigned int line, const char *function);
 
 /* Rio (Robust I/O) package */
 ssize_t rio_readn(int fd, void *usrbuf, size_t n);
 ssize_t rio_writen(int fd, const void *usrbuf, size_t n);
 void rio_readinitb(rio_t *rp, int fd);
 ssize_t rio_readnb(rio_t *rp, void *usrbuf, size_t n);
 ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen);
 
 /* Reentrant protocol-independent client/server helpers */
 int open_clientfd(const char *hostname, const char *port);
 int open_listenfd(const char *port);
 
 #endif /* CSAPP_H */
 
