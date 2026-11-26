#include "ruby/ruby.h"
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

/* Declared here, so that zlib.h is not needed */
unsigned long crc32_z(unsigned long crc, const unsigned char *buf, size_t len);
unsigned long adler32_z(unsigned long adler, const unsigned char *buf, size_t len);
int deflate(void *strm, int flush);

/* System calls that LLVM does not know as library functions. They never call
 * back into Ruby, so they do not trigger GC. */
VALUE
ok_socket_calls(VALUE str, int fd, void *buf, size_t len)
{
    const char *p = RSTRING_PTR(str);
    send(fd, buf, len, 0);
    sendto(fd, buf, len, 0, NULL, 0);
    recv(fd, buf, len, 0);
    recvfrom(fd, buf, len, 0, NULL, NULL);
    return INT2FIX(p[0]);
}

VALUE
ok_thread_calls(VALUE str)
{
    const char *p = RSTRING_PTR(str);
    pthread_setname_np(pthread_self(), "x");
    sleep(0);
    return INT2FIX(p[0]);
}

/* zlib's checksums only compute over the buffer. Not reported. */
VALUE
ok_zlib_checksums(VALUE str, const unsigned char *buf, size_t len)
{
    const char *p = RSTRING_PTR(str);
    unsigned long c = crc32_z(0, buf, len) + adler32_z(1, buf, len);
    return INT2FIX(p[0] + c);
}

/* deflate allocates through the zalloc of the stream, which the zlib extension
 * sets to a function using xmalloc, so it may trigger GC. Reported
 * (boundary). */
VALUE
bad_zlib_deflate(VALUE str, void *strm)
{
    const char *p = RSTRING_PTR(str);
    deflate(strm, 0);
    return INT2FIX(p[0]);
}
