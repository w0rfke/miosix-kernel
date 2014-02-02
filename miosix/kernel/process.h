/***************************************************************************
 *   Copyright (C) 2012 by Terraneo Federico                               *
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

#ifndef PROCESS_H
#define	PROCESS_H

#include <vector>
#include <map>
#include <list>
#include <set>
#include <sys/types.h>
#include "kernel.h"
#include "sync.h"
#include "elf_program.h"
#include "config/miosix_settings.h"
#include "filesystem/file_access.h"

#ifdef WITH_PROCESSES

namespace miosix {

class Process;

/**
 * This class contains the fields that are in common between the kernel and
 * processes
 */
class ProcessBase
{
public:
    /**
     * Constructor
     */
    ProcessBase() : pid(0), ppid(0) {}
    
    /**
     * \return the process' pid 
     */
    pid_t getPid() const { return pid; }
    
    FileDescriptorTable& getFileTable() { return fileTable; }
    
protected:
    pid_t pid;  ///<The pid of this process
    pid_t ppid; ///<The parent pid of this process
    std::list<Process *> childs;   ///<Living child processes are stored here
    std::list<Process *> zombies;  ///<Dead child processes are stored here
    FileDescriptorTable fileTable; ///<The file descriptor table
    
private:
    ProcessBase(const ProcessBase&);
    ProcessBase& operator= (const ProcessBase&);
    
    friend class Process;
};

/**
 * Process class, allows to create and handle processes
 */
class Process : public ProcessBase
{
public:
    /**
     * Create a new process
     * \param program Program that the process will execute
     * \return the pid of the newly created process
     * \throws std::exception or a subclass in case of errors, including
     * not emough memory to spawn the process
     */
    static pid_t create(const ElfProgram& program);
    
    /**
     * Given a process, returns the pid of its parent.
     * \param proc the pid of a process
     * \return the pid of the parent process, or zero if the process was created
     * by the kernel directly, or -1 if proc is not a valid process
     */
    static pid_t getppid(pid_t proc);
    
    /**
     * Wait for child process termination
     * \param exit the process exit code will be returned here, if the pointer
     * is not null
     * \return the pid of the terminated process, or -1 in case of errors
     */
    static pid_t wait(int *exit) { return waitpid(-1,exit,0); }
    
    /**
     * Wait for a specific child process to terminate
     * \param pid pid of the process, or -1 to wait for any child process
     * \param exit the process exit code will be returned here, if the pointer
     * is not null
     * \param options only 0 and WNOHANG are supported
     * \return the pid of the terminated process, or -1 in case of errors. In
     * case WNOHANG  is specified and the specified process has not terminated,
     * 0 is returned
     */
    static pid_t waitpid(pid_t pid, int *exit, int options);
    
    /**
     * Destructor
     */
    ~Process();
    
private:
    
    /**
     * Constructor
     * \param program program that will be executed by the process
     */
    Process(const ElfProgram& program);
    
    /**
     * Contains the process' main loop. 
     * \param argv ignored parameter
     * \return null
     */
    static void *start(void *argv);
    
    /**
     * \return an unique pid that is not zero and is not already in use in the
     * system, used to assign a pid to a new process.<br>
     */
    static pid_t getNewPid();
    
    ElfProgram program; ///<The program that is running inside the process
    ProcessImage image; ///<The RAM image of a process
    miosix_private::FaultData fault; ///< Contains information about faults
    miosix_private::MPUConfiguration mpu; ///<Memory protection data
    
    std::vector<Thread *> threads; ///<Threads that belong to the process
    
    ///Contains the count of active wait calls which specifically requested
    ///to wait on this process
    int waitCount;
    ///Active wait calls which specifically requested to wait on this process
    ///wait on this condition variable
    ConditionVariable waiting;
    bool zombie; ///< True for terminated not yet joined processes
    short int exitCode; ///< Contains the exit code
    
    //Needs access to fault,mpu
    friend class Thread;
    //Needs access to mpu
    friend class PriorityScheduler;
    //Needs access to mpu
    friend class ControlScheduler;
    //Needs access to mpu
    friend class EDFScheduler;
};

} //namespace miosix

#endif //WITH_PROCESSES

#endif //PROCESS_H
