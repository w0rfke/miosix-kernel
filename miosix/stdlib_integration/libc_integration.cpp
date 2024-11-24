/***************************************************************************
 *   Copyright (C) 2008-2023 by Terraneo Federico                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   As a special exception, if other files instantiate templates or use   *
 *   macros or inline functions from this file, or you compile this file   *
 *   and link it with other works to produce a work based on this file,    *
 *   this file does not by itself cause the resulting work to be covered   *
 *   by the GNU General Public License. However the source code for this   *
 *   file must still be made available in accordance with the GNU General  *
 *   Public License. This exception does not invalidate any other reasons  *
 *   why a work based on this file might be covered by the GNU General     *
 *   Public License.                                                       *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, see <http://www.gnu.org/licenses/>   *
 ***************************************************************************/

#include "libc_integration.h"
#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <reent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/times.h>
//// Settings
#include "config/miosix_settings.h"
//// Filesystem
#include "filesystem/file_access.h"
//// Console
#include "kernel/logging.h"
//// kernel interface
#include "kernel/kernel.h"
#include "kernel/process.h"
#include "interfaces/bsp.h"
#include "interfaces/os_timer.h"

using namespace std;

namespace miosix {

// This holds the max heap usage since the program started.
// It is written by _sbrk_r and read by getMaxHeap()
static unsigned int maxHeapEnd=0;

unsigned int getMaxHeap()
{
    //If getMaxHeap() is called before the first _sbrk_r() maxHeapEnd is zero.
    extern char _end asm("_end"); //defined in the linker script
    if(maxHeapEnd==0) return reinterpret_cast<unsigned int>(&_end);
    return maxHeapEnd;
}

/**
 * \return the global C reentrancy structure
 */
static struct _reent *kernelNotStartedGetReent() { return _GLOBAL_REENT; }

/**
 * Pointer to a function that retrieves the correct reentrancy structure.
 * When the C reentrancy structure is requested before the kernel is started,
 * the default reentrancy structure shall be returned, while after the kernel
 * is started, the per-thread reentrancy structure needs to be returned to
 * avoid race conditions between threads.
 * The function pointer is needed to switch between the two behaviors as the
 * per-thread code would cause a circular dependency if called before the
 * kernel is started (getCurrentThread needs to allocate a thread with malloc
 * if called before the kernel istarted, and malloc needs the reentrancy
 * structure).
 */
static struct _reent *(*getReent)()=kernelNotStartedGetReent;

void setCReentrancyCallback(struct _reent *(*callback)()) { getReent=callback; }

} //namespace miosix

#ifdef __cplusplus
extern "C" {
#endif

//
// C atexit support, for thread safety and code size optimizations
// ===============================================================

// Prior to Miosix 1.58 atexit was effectively unimplemented, but its partial
// support in newlib used ~384bytes of RAM. Within the kernel it will always
// be unimplemented, so newlib has been patched not to waste RAM.
// The support library for Miosix processes will instead implement those stubs
// so as to support atexit in processes, as in that case it makes sense.

/**
 * Function called by atexit(), on_exit() and __cxa_atexit() to register
 * C functions/C++ destructors to be run at program termintion.
 * It is called in this way:
 * atexit():       __register_exitproc(__et_atexit, fn, 0,   0)
 * on_exit():      __register_exitproc(__et_onexit, fn, arg, 0)
 * __cxa_atexit(): __register_exitproc(__et_cxa,    fn, arg, d)
 * \param type to understand if the function was called by atexit, on_exit, ...
 * \param fn pointer to function to be called
 * \param arg 0 in case of atexit, function argument in case of on_exit,
 * "this" parameter for C++ destructors registered with __cxa_atexit
 * \param d __dso_handle used to selectively call C++ destructors of a shared
 * library loaded dynamically, unused since Miosix does not support shared libs
 * \return 0 on success
 */
int __register_exitproc(int type, void (*fn)(void*), void *arg, void *d)
{
    (void) type;
    (void) fn;
    (void) arg;
    (void) d;

    return 0;
}

/**
 * Called by exit() to call functions registered through atexit()
 * \param code the exit code, for example with exit(1), code==1
 * \param d __dso_handle, see __register_exitproc
 */
void __call_exitprocs(int code, void *d)
{
    (void) code;
    (void) d;
}

/**
 * \internal
 * Required by C++ standard library.
 * See http://lists.debian.org/debian-gcc/2003/07/msg00057.html
 */
void *__dso_handle=(void*) &__dso_handle;




//
// C/C++ system calls, to support malloc, printf, fopen, etc.
// ==========================================================

/**
 * \internal
 * _exit, restarts the system
 */
void _exit(int n)
{
    miosix::reboot();
    //Never reach here
    for(;;) ; //Required to avoid a warning about noreturn functions
}

/**
 * \internal
 * _sbrk_r, allocates memory dynamically
 */
void *_sbrk_r(struct _reent *ptr, ptrdiff_t incr)
{
    //This is the absolute start of the heap
    extern char _end asm("_end"); //defined in the linker script
    //This is the absolute end of the heap
    extern char _heap_end asm("_heap_end"); //defined in the linker script
    //This holds the current end of the heap (static)
    static char *curHeapEnd=nullptr;
    //This holds the previous end of the heap
    char *prevHeapEnd;

    //Check if it's first time called
    if(curHeapEnd==nullptr) curHeapEnd=&_end;

    prevHeapEnd=curHeapEnd;
    if((curHeapEnd+incr)>&_heap_end)
    {
        //bad, heap overflow
        #ifdef __NO_EXCEPTIONS
        // When exceptions are disabled operator new would return nullptr, which
        // would cause undefined behaviour. So when exceptions are disabled,
        // a heap overflow causes a reboot.
        errorLog("\n***Heap overflow\n");
        _exit(1);
        #else //__NO_EXCEPTIONS
        return reinterpret_cast<void*>(-1);
        #endif //__NO_EXCEPTIONS
    }
    curHeapEnd+=incr;

    if(reinterpret_cast<unsigned int>(curHeapEnd) > miosix::maxHeapEnd)
        miosix::maxHeapEnd=reinterpret_cast<unsigned int>(curHeapEnd);
    
    return reinterpret_cast<void*>(prevHeapEnd);
}

void *sbrk(ptrdiff_t incr)
{
    return _sbrk_r(miosix::getReent(),incr);
}

/**
 * \internal
 * __malloc_lock, called by malloc to ensure no context switch happens during
 * memory allocation (the heap is global and shared between the threads, so
 * memory allocation should not be interrupted by a context switch)
 *
 *	WARNING:
 *	pauseKernel() does not stop interrupts, so interrupts may occur
 *	during memory allocation. So NEVER use malloc inside an interrupt!
 *	Also beware that some newlib functions, like printf, iprintf...
 *	do call malloc, so you must not use them inside an interrupt.
 */
void __malloc_lock()
{
    miosix::pauseKernel();
}

/**
 * \internal
 * __malloc_unlock, called by malloc after performing operations on the heap
 */
void __malloc_unlock()
{
    miosix::restartKernel();
}

/**
 * \internal
 * __getreent(), return the reentrancy structure of the current thread.
 * Used by newlib to make the C standard library thread safe
 */
struct _reent *__getreent()
{
    return miosix::getReent();
}




/**
 * \internal
 * _open_r, open a file
 */
int _open_r(struct _reent *ptr, const char *name, int flags, int mode)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().open(name,flags,mode);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    (void) name;
    (void) flags;
    (void) mode;

    ptr->_errno=ENFILE;
    return -1;
    #endif //WITH_FILESYSTEM
}

int open(const char *name, int flags, ...)
{
    int mode=0;
    if(flags & O_CREAT)
    {
        va_list arg;
        va_start(arg,flags);
        mode=va_arg(arg,int);
        va_end(arg);
    }
    return _open_r(miosix::getReent(),name,flags,mode);
}

/**
 * \internal
 * _close_r, close a file
 */
int _close_r(struct _reent *ptr, int fd)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().close(fd);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    (void) fd;
    ptr->_errno=EBADF;
    return -1;
    #endif //WITH_FILESYSTEM
}

int close(int fd)
{
    return _close_r(miosix::getReent(),fd);
}

/**
 * \internal
 * _write_r, write to a file
 */
ssize_t _write_r(struct _reent *ptr, int fd, const void *buf, size_t size)
{    
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        ssize_t result=miosix::getFileDescriptorTable().write(fd,buf,size);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    if(fd==STDOUT_FILENO || fd==STDERR_FILENO)
    {
        ssize_t result=miosix::DefaultConsole::instance().getTerminal()->write(buf,size);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    } else {
        ptr->_errno=EBADF;
        return -1;
    }
    #endif //WITH_FILESYSTEM
}

ssize_t write(int fd, const void *buf, size_t size)
{
    return _write_r(miosix::getReent(),fd,buf,size);
}

/**
 * \internal
 * _read_r, read from a file
 */
ssize_t _read_r(struct _reent *ptr, int fd, void *buf, size_t size)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        ssize_t result=miosix::getFileDescriptorTable().read(fd,buf,size);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    if(fd==STDIN_FILENO)
    {
        ssize_t result=miosix::DefaultConsole::instance().getTerminal()->read(buf,size);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    } else {
        ptr->_errno=EBADF;
        return -1;
    }
    #endif //WITH_FILESYSTEM
}

ssize_t read(int fd, void *buf, size_t size)
{
    return _read_r(miosix::getReent(),fd,buf,size);
}

/**
 * \internal
 * _lseek_r, move file pointer
 */
off_t _lseek_r(struct _reent *ptr, int fd, off_t pos, int whence)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        off_t result=miosix::getFileDescriptorTable().lseek(fd,pos,whence);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) fd;
    (void) pos;
    (void) whence;
    ptr->_errno=EBADF;
    return -1;
    #endif //WITH_FILESYSTEM
}

off_t lseek(int fd, off_t pos, int whence)
{
    return _lseek_r(miosix::getReent(),fd,pos,whence);
}

/**
 * \internal
 * _fstat_r, return file info
 */
int _fstat_r(struct _reent *ptr, int fd, struct stat *pstat)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().fstat(fd,pstat);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    switch(fd)
    {
        case STDIN_FILENO:
        case STDOUT_FILENO:
        case STDERR_FILENO:
            memset(pstat,0,sizeof(struct stat));
            pstat->st_mode=S_IFCHR;//Character device
            pstat->st_blksize=0; //Defualt file buffer equals to BUFSIZ
            return 0;
        default:
            ptr->_errno=EBADF;
            return -1;
    }
    #endif //WITH_FILESYSTEM
}

int fstat(int fd, struct stat *pstat)
{
    return _fstat_r(miosix::getReent(),fd,pstat);
}

/**
 * \internal
 * _stat_r, collect data about a file
 */
int _stat_r(struct _reent *ptr, const char *file, struct stat *pstat)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().stat(file,pstat);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) file;
    (void) pstat;
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int stat(const char *file, struct stat *pstat)
{
    return _stat_r(miosix::getReent(),file,pstat);
}

/**
 * \internal
 * _lstat_r, collect data about a file
 */
int _lstat_r(struct _reent *ptr, const char *file, struct stat *pstat)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().lstat(file,pstat);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int lstat(const char *file, struct stat *pstat)
{
    return _lstat_r(miosix::getReent(),file,pstat);
}

/**
 * \internal
 * isatty, returns 1 if fd is associated with a terminal
 */
int _isatty_r(struct _reent *ptr, int fd)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().isatty(fd);
        if(result>0) return result;
        if(result==0) ptr->_errno=ENOTTY;
        else ptr->_errno=-result;
        return 0;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return 0;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) ptr;
    switch(fd)
    {
        case STDIN_FILENO:
        case STDOUT_FILENO:
        case STDERR_FILENO:
            return 1;
        default:
            ptr->_errno=EBADF;
            return 0;
    }
    #endif //WITH_FILESYSTEM
}

int isatty(int fd)
{
    return _isatty_r(miosix::getReent(),fd);
}

/**
 * \internal
 * _fntl_r, perform operations on a file descriptor
 */
int _fcntl_r(struct _reent *ptr, int fd, int cmd, int opt)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().fcntl(fd,cmd,opt);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) fd;
    (void) cmd;
    (void) opt;
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int fcntl(int fd, int cmd, ...)
{
    va_list arg;
    int result;
    struct _reent *r=miosix::getReent();
    switch(cmd)
    {
        case F_DUPFD:
        case F_SETFD:
        case F_SETFL:
            va_start(arg,cmd);
            result=_fcntl_r(r,fd,cmd,va_arg(arg,int));
            va_end(arg);
        default:
            result=_fcntl_r(r,fd,cmd,0);
    }
    return result;
}

/**
 * \internal
 * _ioctl_r, perform operations on a file descriptor
 */
int _ioctl_r(struct _reent *ptr, int fd, int cmd, void *arg)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().ioctl(fd,cmd,arg);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    if(fd==STDIN_FILENO || fd==STDOUT_FILENO || fd==STDERR_FILENO)
    {
        int result=miosix::DefaultConsole::instance().getTerminal()->ioctl(cmd,arg);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    } else {
        ptr->_errno=ENOENT;
        return -1;
    }
    #endif //WITH_FILESYSTEM
}

int ioctl(int fd, int cmd, void *arg)
{
    return _ioctl_r(miosix::getReent(),fd,cmd,arg);
}

/**
 * \internal
 * _getcwd_r, return current directory
 */
char *_getcwd_r(struct _reent *ptr, char *buf, size_t size)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().getcwd(buf,size);
        if(result>=0) return buf;
        ptr->_errno=-result;
        return nullptr;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return nullptr;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) buf;
    (void) size;
    ptr->_errno=ENOENT;
    return nullptr;
    #endif //WITH_FILESYSTEM
}

char *getcwd(char *buf, size_t size)
{
    return _getcwd_r(miosix::getReent(),buf,size);
}

/**
 * \internal
 * _chdir_r, change current directory
 */
int _chdir_r(struct _reent *ptr, const char *path)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().chdir(path);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) path;
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int chdir(const char *path)
{
    return _chdir_r(miosix::getReent(),path);
}

/**
 * \internal
 * _mkdir_r, create a directory
 */
int _mkdir_r(struct _reent *ptr, const char *path, int mode)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().mkdir(path,mode);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) path;
    (void) mode;
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int mkdir(const char *path, mode_t mode)
{
    return _mkdir_r(miosix::getReent(),path,mode);
}

/**
 * \internal
 * _rmdir_r, remove a directory if empty
 */
int _rmdir_r(struct _reent *ptr, const char *path)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().rmdir(path);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) path;
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int rmdir(const char *path)
{
    return _rmdir_r(miosix::getReent(),path);
}

/**
 * \internal
 * _link_r: create hardlinks
 */
int _link_r(struct _reent *ptr, const char *f_old, const char *f_new)
{
    (void) f_old;
    (void) f_new;
    ptr->_errno=EMFILE; //Currently no fs supports hardlinks
    return -1;
}

int link(const char *f_old, const char *f_new)
{
    return _link_r(miosix::getReent(),f_old,f_new);
}

/**
 * \internal
 * _unlink_r, remove a file
 */
int _unlink_r(struct _reent *ptr, const char *file)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().unlink(file);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) file;
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int unlink(const char *file)
{
    return _unlink_r(miosix::getReent(),file);
}

/**
 * \internal
 * _symlink_r: create hardlinks
 */
int _symlink_r(struct _reent *ptr, const char *target, const char *linkpath)
{
    ptr->_errno=ENOENT; //Unimplemented at the moment
    return -1;
}

int symlink(const char *target, const char *linkpath)
{
    return _symlink_r(miosix::getReent(),target,linkpath);
}

/**
 * \internal
 * _readlink_r: read symlinks
 */
ssize_t _readlink_r(struct _reent *ptr, const char *path, char *buf, size_t size)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().readlink(path,buf,size);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

ssize_t readlink(const char *path, char *buf, size_t size)
{
    return _readlink_r(miosix::getReent(),path,buf,size);
}

/**
 * \internal
 * truncate, change file size
 */
int truncate(const char *path, off_t size)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().truncate(path,size);
        if(result>=0) return result;
        miosix::getReent()->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        miosix::getReent()->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    miosix::getReent()->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

/**
 * \internal
 * ftruncate, change file size
 */
int ftruncate(int fd, off_t size)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().ftruncate(fd,size);
        if(result>=0) return result;
        miosix::getReent()->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        miosix::getReent()->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    miosix::getReent()->_errno=EBADF;
    return -1;
    #endif //WITH_FILESYSTEM
}

/**
 * \internal
 * _rename_r, rename a file or directory
 */
int _rename_r(struct _reent *ptr, const char *f_old, const char *f_new)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().rename(f_old,f_new);
        if(result>=0) return result;
        ptr->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        ptr->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    (void) f_old;
    (void) f_new;
    ptr->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int rename(const char *f_old, const char *f_new)
{
    return _rename_r(miosix::getReent(),f_old,f_new);
}

/**
 * \internal
 * getdents, allows to list the content of a directory
 */
int getdents(int fd, struct dirent *buf, unsigned int size)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().getdents(fd,buf,size);
        if(result>=0) return result;
        miosix::getReent()->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        miosix::getReent()->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS
    
    #else //WITH_FILESYSTEM
    miosix::getReent()->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int dup(int fd)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().dup(fd);
        if(result>=0) return result;
        miosix::getReent()->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        miosix::getReent()->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    (void) fd;
    (void) dirp;
    (void) count;
    miosix::getReent()->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int dup2(int oldfd, int newfd)
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().dup2(oldfd,newfd);
        if(result>=0) return result;
        miosix::getReent()->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        miosix::getReent()->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    miosix::getReent()->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

int pipe(int fds[2])
{
    #ifdef WITH_FILESYSTEM

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        int result=miosix::getFileDescriptorTable().pipe(fds);
        if(result>=0) return result;
        miosix::getReent()->_errno=-result;
        return -1;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        miosix::getReent()->_errno=ENOMEM;
        return -1;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_FILESYSTEM
    miosix::getReent()->_errno=ENOENT;
    return -1;
    #endif //WITH_FILESYSTEM
}

/*
 * Time API in Miosix
 * ==================
 *
 * CORE
 * - clock_gettime
 * - clock_nanosleep
 * - clock_settime
 * - clock_getres
 *
 * DERIVED, preferred
 * - C++11 chrono system|steady_clock -> clock_gettime
 * - C++11 sleep_for|until            -> clock_nanosleep
 *
 * DERIVED, for compatibility (replace with core functions when possible)
 * - sleep|usleep -> nanosleep        -> clock_nanosleep
 * - clock        -> times            -> clock_gettime
 * - time         -> gettimeofday     -> clock_gettime
 *
 * UNSUPPORTED
 * - timer_create -> ? 
 */

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    (void) clock_id;
    (void) tp;

    if(tp==nullptr) return -1;
    //TODO: support CLOCK_REALTIME
    miosix::ll2timespec(miosix::getTime(),tp);
    return 0;
}

int clock_settime(clockid_t clock_id, const struct timespec *tp)
{
    (void) clock_id;
    (void) tp;

    //TODO: support CLOCK_REALTIME
    return -1;
}

int clock_getres(clockid_t clock_id, struct timespec *res)
{
    (void) clock_id;

    if(res==nullptr) return -1;
    //TODO: support CLOCK_REALTIME

    //Integer division with round-to-nearest for better accuracy
    int resolution=2*miosix::nsPerSec/miosix::internal::osTimerGetFrequency();
    resolution=(resolution & 1) ? resolution/2+1 : resolution/2;

    res->tv_sec=0;
    res->tv_nsec=resolution;
    return 0;
}

int clock_nanosleep(clockid_t clock_id, int flags,
                    const struct timespec *req, struct timespec *rem)
{
    (void) clock_id;
    (void) rem;

    if(req==nullptr) return -1;
    //TODO: support CLOCK_REALTIME
    long long timeNs=miosix::timespec2ll(req);
    if(flags!=TIMER_ABSTIME) timeNs+=miosix::getTime();
    miosix::Thread::nanoSleepUntil(timeNs);
    return 0;
}

/**
 * \internal
 * _times_r, return elapsed time
 */
clock_t _times_r(struct _reent *ptr, struct tms *tim)
{
    (void) ptr;

    struct timespec tp;
    //No CLOCK_PROCESS_CPUTIME_ID support, use CLOCK_MONOTONIC
    if(clock_gettime(CLOCK_MONOTONIC,&tp)) return static_cast<clock_t>(-1);
    constexpr int divFactor=1000000000/CLOCKS_PER_SEC;
    clock_t utime=tp.tv_sec*CLOCKS_PER_SEC + tp.tv_nsec/divFactor;

    //Unfortunately, the behavior of _times_r is poorly specified and ambiguous.
    //The return value is either tim.utime or -1 on failure, but clock_t is
    //unsigned. If someone calls _times_r in an unlucky moment where tim.utime
    //is 0xffffffff it could be interpreted as the -1 error code even if there
    //is no error.
    //This is not as unlikely as it seems because CLOCKS_PER_SEC is a relatively
    //huge number (100 for Miosix's implementation).
    //To solve the ambiguity Miosix never returns 0xffffffff except in case of
    //error. If tim.utime happens to be 0xffffffff, _times_r returns 0 instead.
    //We also implement the Linux extension where tim can be NULL.
    if(tim!=nullptr)
    {
        tim->tms_utime=utime;
        tim->tms_stime=0;
        tim->tms_cutime=0;
        tim->tms_cstime=0;
    }
    return utime==static_cast<clock_t>(-1)?0:utime;
}

clock_t times(struct tms *tim)
{
    return _times_r(miosix::getReent(),tim);
}

int _gettimeofday_r(struct _reent *ptr, struct timeval *tv, void *tz)
{
    (void) ptr;

    if(tv==nullptr || tz!=nullptr) return -1;
    struct timespec tp;
    if(clock_gettime(CLOCK_REALTIME,&tp)) return -1;
    tv->tv_sec=tp.tv_sec;
    tv->tv_usec=tp.tv_nsec/1000;
    return 0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    return _gettimeofday_r(miosix::getReent(),tv,tz);
}


int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return clock_nanosleep(CLOCK_MONOTONIC,0,req,rem);
}




/**
 * \internal
 * it looks like abort() calls _kill instead of exit, this implementation
 * calls _exit() so that calling abort() really terminates the program
 */
int _kill_r(struct _reent* ptr, int pid, int sig)
{
    (void) ptr;
    (void) sig;
    if(pid==0) _exit(1); //pid 0 is the kernel
    else return -1;
}

int kill(int pid, int sig)
{
    return _kill_r(miosix::getReent(),pid,sig);
}

/**
 * \internal
 * _getpid_r, the kernel has pid 0 so getpid return 0 in the kernel
 */
int _getpid_r(struct _reent* ptr)
{
    (void) ptr;

    return 0;
}

/**
 * \internal
 * getpid, the kernel has pid 0 so getpid return 0 in the kernel
 */
int getpid()
{
    return 0;
}

/**
 * \internal
 * getppid, the kernel has no parent, in the kernel getppid returns 0
 */
int getppid()
{
    return 0;
}

/**
 * \internal
 * _wait_r, wait for process termination
 */
int _wait_r(struct _reent *ptr, int *status)
{
    #ifdef WITH_PROCESSES
    int result=miosix::Process::wait(status);
    if(result>=0) return result;
    ptr->_errno=-result;
    return -1;
    #else //WITH_PROCESSES
    (void) ptr;
    (void) status;
    return -1;
    #endif //WITH_PROCESSES
}

pid_t wait(int *status)
{
    return _wait_r(miosix::getReent(),status);
}

/**
 * \internal
 * waitpid, wait for process termination
 */
pid_t waitpid(pid_t pid, int *status, int options)
{
    #ifdef WITH_PROCESSES
    int result=miosix::Process::waitpid(pid,status,options);
    if(result>=0) return result;
    errno=-result;
    return -1;
    #else //WITH_PROCESSES
    return -1;
    #endif //WITH_PROCESSES
}

/**
 * \internal
 * _execve_r, always fails when called from kernelspace because the kernel
 * cannot be switched for another program
 */
int _execve_r(struct _reent *ptr, const char *path, char *const argv[],
        char *const env[])
{
    #ifdef WITH_PROCESSES
    ptr->_errno=-EFAULT;
    return -1;
    #else //WITH_PROCESSES
    return -1;
    #endif //WITH_PROCESSES
}

int execve(const char *path, char *const argv[], char *const env[])
{
    return _execve_r(miosix::getReent(),path,argv,env);
}

/**
 * \internal
 * posix_spawn, spawn child processes
 */
int posix_spawn(pid_t *pid, const char *path,
        const posix_spawn_file_actions_t *a, const posix_spawnattr_t *s,
        char *const argv[], char *const envp[])
{
    #ifdef WITH_PROCESSES

    #ifndef __NO_EXCEPTIONS
    try {
    #endif //__NO_EXCEPTIONS
        if(a!=nullptr || s!=nullptr) return EFAULT; //Not supported yet
        pid_t result=miosix::Process::spawn(path,argv,envp);
        if(result>=0)
        {
            if(pid) *pid=result;
            return 0;
        }
        return -result;
    #ifndef __NO_EXCEPTIONS
    } catch(exception& e) {
        return ENOMEM;
    }
    #endif //__NO_EXCEPTIONS

    #else //WITH_PROCESSES
    (void) ptr;
    (void) path;
    (void) argv;
    (void) env;

    return 1;
    #endif //WITH_PROCESSES
}

#ifdef __cplusplus
}
#endif




//
// Check that newlib has been configured correctly
// ===============================================

#ifndef _REENT_SMALL
#error "_REENT_SMALL not defined"
#endif //_REENT_SMALL

#ifndef _POSIX_THREADS
#error "_POSIX_THREADS not defined"
#endif //_POSIX_THREADS

#ifndef __DYNAMIC_REENT__
#error "__DYNAMIC_REENT__ not defined"
#endif
