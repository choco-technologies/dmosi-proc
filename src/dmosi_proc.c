#include "dmod.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

// DMOSPROC in ASCII
#define MAGIC_NUMBER    0x444D4F5350524F43ULL    

static dmosi_process_id_t next_process_id = 1;

/**
 * @brief State of a single process stream slot (stdin/stdout/stderr/stdlog)
 */
typedef struct
{
    char* path;             /**< Path bound to this stream slot, NULL if unset */
    void* handle;           /**< Open file handle for path, NULL if unset */
    volatile bool locked;   /**< Recursion guard, set while the slot is locked */
} dmosi_process_stream_t;

/**
 * @brief Node for a single registered process exit callback
 *
 * Stored as a singly-linked list on struct dmosi_process. The node pointer
 * itself is handed out as the opaque dmosi_process_exit_callback_handle_t.
 */
struct dmosi_process_exit_callback
{
    dmosi_process_exit_callback_t callback;    /**< Callback to invoke on process exit */
    void* arg;                                 /**< User-provided argument for the callback */
    struct dmosi_process_exit_callback* next;  /**< Next registration, or NULL */
};

/**
 * @brief Opaque type for process
 *
 * This type represents a process in the DMOD OSI system.
 *
 * @note The actual implementation of the process is hidden
 * from the user and is specific to the underlying OS.
 */
struct dmosi_process
{
    uint64_t magic;                                 /**< Magic number for validation */
    char* name;                                     /**< Name of the process */
    Dmod_Context_t* context;                        /**< Dmod_Context_t this process is running, set via dmosi_process_set_context */
    Dmod_Context_t* foreground_module;              /**< Context whose Main is currently executing in this process, set via dmosi_process_set_foreground_module */
    dmosi_process_t parent;                         /**< Parent process (NULL for detached processes) */
    char module_name[DMOD_MAX_MODULE_NAME_LENGTH];  /**< Name of the associated module */
    int exit_status;                                /**< Exit status code (set when process is killed) */
    dmosi_process_state_t state;                    /**< Current state of the process */
    dmosi_process_id_t pid;                         /**< Unique process ID */
    dmosi_user_id_t uid;                            /**< User ID associated with the process */
    char* pwd;                                      /**< Working directory path */
    dmosi_process_stream_t streams[DMOSI_STREAM_COUNT]; /**< Stream slots (stdin/stdout/stderr/stdlog) */
    struct dmosi_process_exit_callback* exit_callbacks; /**< Registered exit callbacks (singly-linked) */
};

/**
 * @brief Validate a process handle
 *
 * @return bool true if the process handle is valid, false otherwise
 */
static bool validate_process( dmosi_process_t process )
{
    if(!process)
        return false;
    if(process->magic != MAGIC_NUMBER)
        return false;
    return true;
}

/**
 * @brief Generate a unique process ID
 *
 * @return dmosi_process_id_t Unique process ID
 */
static dmosi_process_id_t generate_process_id()
{
    return next_process_id++;
}

/**
 * @brief Detach and invoke all exit callbacks registered on a process
 *
 * Atomically detaches the callback list from the process (so a concurrent
 * dmosi_process_register_exit_callback()/dmosi_process_unregister_exit_callback()
 * call no longer sees it), then invokes and frees each node outside the lock
 * so user callback code never runs while holding it.
 *
 * @param process Process that has terminated
 * @param exit_status Exit status to report to the callbacks
 */
static void invoke_exit_callbacks( dmosi_process_t process, int exit_status )
{
    Dmod_EnterCritical();
    struct dmosi_process_exit_callback* node = process->exit_callbacks;
    process->exit_callbacks = NULL;
    Dmod_ExitCritical();

    while(node)
    {
        struct dmosi_process_exit_callback* next = node->next;
        node->callback(process, exit_status, node->arg);
        Dmod_Free(node);
        node = next;
    }
}

/**
 * @brief Shared state tracking how many of a process's threads are still alive
 *
 * One instance is allocated per dmosi_process_kill()/dmosi_process_destroy() call and
 * shared (via dmosi_thread_register_exit_callback()'s arg) across every thread being
 * killed, so the process's own exit callbacks fire exactly once - from inside the last
 * thread's real termination path, never before.
 */
typedef struct
{
    dmosi_process_t process;
    int exit_status;
    size_t threads_remaining;  /**< Dmod_EnterCritical/ExitCritical-protected countdown */
} process_kill_completion_t;

/**
 * @brief Thread exit callback used internally by kill_threads() to detect real thread death
 *
 * Registered on every thread of a process right before it is killed. Because
 * dmosi_thread_kill() invokes a thread's registered exit callbacks itself - from within
 * the kill call for other threads, or just before self-termination for the thread killing
 * itself - this fires exactly when a thread has actually exited, never merely when it was
 * asked to. Only once every thread has reported in does it invoke the process's own exit
 * callbacks, so they can never observe (or race against) a thread that is still alive.
 *
 * @param thread Thread that just exited (unused - only the countdown matters here)
 * @param arg Shared process_kill_completion_t for this kill/destroy call
 */
static void on_process_thread_exited( dmosi_thread_t thread, void* arg )
{
    (void)thread;
    process_kill_completion_t* completion = (process_kill_completion_t*)arg;

    Dmod_EnterCritical();
    completion->threads_remaining--;
    bool all_exited = (completion->threads_remaining == 0);
    Dmod_ExitCritical();

    if(all_exited)
    {
        invoke_exit_callbacks(completion->process, completion->exit_status);
        Dmod_Free(completion);
    }
}

/**
 * @brief Kill all threads associated with a process
 *
 * Once every thread has genuinely exited (as reported by dmosi_thread_kill() through the
 * internal exit-callback hook registered here, see on_process_thread_exited()), the
 * process's own exit callbacks are invoked. If the process has no threads at all, there is
 * nothing to wait for, so they are invoked immediately.
 *
 * @param process Process handle whose threads to kill
 * @param status Exit status code to pass to threads and to the process's exit callbacks
 * @return bool true on success, false on failure
 */
static bool kill_threads( dmosi_process_t process, int status )
{
    size_t count = dmosi_thread_get_by_process(process, NULL, 0);
    if(count == 0)
    {
        invoke_exit_callbacks(process, status);
        return true; // No threads to kill
    }

    dmosi_thread_t* threads = Dmod_MallocEx(sizeof(dmosi_thread_t) * count, process->module_name);
    if(!threads)
    {
        DMOD_LOG_ERROR("Failed to allocate memory for thread handles while killing process %s of module %s\n", process->name, process->module_name);
        return false;
    }

    size_t actual_count = dmosi_thread_get_by_process(process, threads, count);
    if(actual_count != count)
    {
        DMOD_LOG_WARN("Thread count mismatch while killing process %s of module %s: expected %zu, got %zu\n", process->name, process->module_name, count, actual_count);
    }

    if(actual_count == 0)
    {
        Dmod_Free(threads);
        invoke_exit_callbacks(process, status);
        return true;
    }

    process_kill_completion_t* completion = Dmod_MallocEx(sizeof(*completion), process->module_name);
    if(!completion)
    {
        DMOD_LOG_ERROR("Failed to allocate memory for exit-callback tracking while killing process %s of module %s\n", process->name, process->module_name);
        Dmod_Free(threads);
        return false;
    }
    completion->process = process;
    completion->exit_status = status;
    completion->threads_remaining = actual_count;

    // Register on every thread *before* killing any of them: dmosi_thread_kill() on a
    // thread killing itself never returns to this loop, so any registration still to be
    // done after that point would simply never happen, and the countdown would stall
    // forever with the process's exit callbacks never firing.
    for(size_t i = 0; i < actual_count; i++)
    {
        if(!dmosi_thread_register_exit_callback(threads[i], on_process_thread_exited, completion))
        {
            // Thread had already exited by the time we tried to observe it - count it as
            // done right away instead of waiting for a callback that will never fire.
            on_process_thread_exited(threads[i], completion);
        }
    }

    for(size_t i = 0; i < actual_count; i++)
    {
        if(!dmosi_thread_kill(threads[i], status))
        {
            DMOD_LOG_ERROR("Failed to kill thread in process %s of module %s\n", process->name, process->module_name);
            Dmod_Free(threads);
            return false;
        }
    }

    Dmod_Free(threads);
    return true;
}

/**
 * @brief Find a process using a custom predicate function
 *
 * @param predicate Function that returns true if process matches search criteria
 * @param context Context data passed to the predicate function
 * @param search_description Description of what is being searched for (for logging)
 * @return dmosi_process_t Found process or NULL if not found
 */
static dmosi_process_t find_process_with_predicate(bool (*predicate)(dmosi_process_t process, void* context), void* context, const char* search_description)
{
    DMOD_LOG_VERBOSE("Searching for process: %s\n", search_description);
    
    // Get all threads to find processes through them
    size_t thread_count = dmosi_thread_get_all(NULL, 0);
    if(thread_count == 0)
    {
        return NULL; // No threads, no processes
    }
    
    dmosi_thread_t* threads = Dmod_Malloc(sizeof(dmosi_thread_t) * thread_count);
    if(!threads)
    {
        DMOD_LOG_ERROR("Failed to allocate memory for thread handles while searching for process: %s\n", search_description);
        return NULL;
    }
    
    size_t actual_count = dmosi_thread_get_all(threads, thread_count);
    dmosi_process_t found_process = NULL;
    
    // Check each thread's process
    for(size_t i = 0; i < actual_count && !found_process; i++)
    {
        dmosi_process_t process = dmosi_thread_get_process(threads[i]);
        if(process && predicate(process, context))
        {
            found_process = process;
            break;
        }
    }
    
    Dmod_Free(threads);
    return found_process;
}

/**
 * @brief Determine the fopen-style mode to use when binding a stream slot to a file
 *
 * Stdin is opened for reading; the output-oriented slots (stdout/stderr/stdlog) are
 * opened for appending so that binding the same path from multiple processes (e.g.
 * after inheritance) does not truncate what earlier processes already wrote to it.
 *
 * @param index Stream slot being opened
 * @return const char* fopen-style mode string
 */
static const char* stream_open_mode(dmosi_stream_index_t index)
{
    return (index == DMOSI_STREAM_STDIN) ? "r" : "a";
}

/**
 * @brief Validate that a stream index refers to an existing slot in a process
 *
 * @param index Stream slot index to validate
 * @return bool true if the index is within range, false otherwise
 */
static bool validate_stream_index(dmosi_stream_index_t index)
{
    return index >= 0 && index < DMOSI_STREAM_COUNT;
}

/**
 * @brief Predicate function for finding process by name
 */
static bool process_name_predicate(dmosi_process_t process, void* context)
{
    const char* name = (const char*)context;
    return process->name && strcmp(process->name, name) == 0;
}

/**
 * @brief Predicate function for finding process by ID
 */
static bool process_id_predicate(dmosi_process_t process, void* context)
{
    dmosi_process_id_t* pid = (dmosi_process_id_t*)context;
    return process->pid == *pid;
}

/**
 * @brief Predicate function for finding a live process whose parent is a given process
 */
static bool process_parent_predicate(dmosi_process_t process, void* context)
{
    dmosi_process_t parent = (dmosi_process_t)context;
    return process->parent == parent;
}

/**
 * @brief Kill a process and every process spawned from it, recursively
 *
 * A killed process's children do not die with it on their own - dmosi_process_create()
 * only records the parent link, it never ties a child's lifetime to it. Left alone, a
 * supervisor that only ever tracks one PID per unit (e.g. console launching a shell via
 * Dmod_SpawnModule and then waiting on it - the shell is a separate process, merely
 * parented to console's own) would find that killing the one PID it tracks leaves the
 * real long-running work completely untouched. This walks the same parent link
 * dmosi_process_get_parent() exposes, mirroring how a real OS tears down a whole
 * process tree/cgroup instead of a single PID.
 *
 * @param process Process whose subtree to kill (already validated non-NULL by the caller)
 * @param status Exit status code applied to every process in the subtree
 * @return bool true if every thread in the subtree was killed successfully
 */
static bool kill_process_tree( dmosi_process_t process, int status )
{
    bool ok = true;

    // Kill children before the process itself: find_process_with_predicate() only ever
    // sees processes with at least one live thread, so once a child's own threads are
    // gone it simply stops being found - this loop naturally terminates as the tree is
    // consumed from the leaves up, and never revisits an already-killed child.
    dmosi_process_t child = find_process_with_predicate(process_parent_predicate, process, "child process");
    while(child != NULL)
    {
        if(!kill_process_tree(child, status))
        {
            ok = false;
        }
        child = find_process_with_predicate(process_parent_predicate, process, "child process");
    }

    // Set exit_status/state *before* kill_threads(), not after: when a process kills its
    // own currently-running thread (the normal case - every spawned module calls this on
    // itself via Dmod_Exit when it finishes), dmosi_thread_kill() ends in vTaskDelete(NULL),
    // which never returns to its caller. Anything placed after kill_threads() here would
    // simply never run for that case, leaving the process stuck at its prior state forever
    // (observed as background jobs whose underlying task is long gone but that dmell's
    // reaper never sees as DMOSI_PROCESS_STATE_TERMINATED, so it never frees them).
    process->exit_status = status;
    process->state = DMOSI_PROCESS_STATE_TERMINATED;

    // The process's own exit callbacks are invoked by kill_threads(), once every thread
    // has actually exited (see on_process_thread_exited()) - not here, since the threads
    // are still very much alive at this point and a callback must never be able to
    // observe (or race against, e.g. by restarting the process) one that is still running.
    if(!kill_threads(process, status))
    {
        DMOD_LOG_ERROR("Failed to kill threads while killing process %s of module %s\n", process->name, process->module_name);
        ok = false;
    }

    return ok;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_t, _process_create,(const char* name, const char* module_name, dmosi_process_t parent) )
{
    if(name == NULL)
    {
        DMOD_LOG_ERROR("Process name cannot be NULL\n");
        return NULL;
    }
    module_name = module_name ? module_name : DMOSI_SYSTEM_MODULE_NAME;
    dmosi_process_t process = Dmod_MallocEx(sizeof(struct dmosi_process), module_name );
    if(!process)
    {
        DMOD_LOG_ERROR("Failed to allocate memory for process %s of module %s\n", name, module_name);
        return NULL;
    }

    process->magic = MAGIC_NUMBER;
    process->exit_status = 0;
    process->state = DMOSI_PROCESS_STATE_RUNNING;
    process->name = Dmod_StrDup(name);
    process->pid = generate_process_id();
    process->uid = 0;
    process->pwd = NULL;
    process->exit_callbacks = NULL;
    memset(process->streams, 0, sizeof(process->streams));
    if(!process->name)
    {
        DMOD_LOG_ERROR("Failed to duplicate process name %s for module %s\n", name, module_name);
        Dmod_Free(process);
        return NULL;
    }

    process->context = NULL; /* set via dmosi_process_set_context once the module to run is known */
    process->foreground_module = NULL;
    process->parent = parent;
    strcpy(process->module_name, module_name);

    // Inherit the parent's stream bindings (by path, not by handle, so that
    // closing one process's stream handle never affects the other's).
    if(parent)
    {
        for(dmosi_stream_index_t i = 0; i < DMOSI_STREAM_COUNT; i++)
        {
            if(parent->streams[i].path && dmosi_process_set_stream(process, i, parent->streams[i].path) != 0)
            {
                DMOD_LOG_WARN("Failed to inherit stream %d (%s) from parent process %s into %s\n", (int)i, parent->streams[i].path, parent->name, name);
            }
        }
    }

    DMOD_LOG_VERBOSE("Created process %s of module %s\n", name, module_name);
    return process;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void, _process_destroy, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot destroy NULL process\n");
        return;
    }
    DMOD_LOG_VERBOSE("Destroying process %s of module %s\n", process->name, process->module_name);

    Dmod_EnterCritical();

    if(!kill_threads(process, process->exit_status))
    {
        DMOD_LOG_ERROR("Failed to kill threads while destroying process %s of module %s\n", process->name, process->module_name);
    }

    process->state = DMOSI_PROCESS_STATE_TERMINATED;
    process->magic = 0; // Invalidate the process handle

    for(dmosi_stream_index_t i = 0; i < DMOSI_STREAM_COUNT; i++)
    {
        if(process->streams[i].handle)
        {
            Dmod_FileClose(process->streams[i].handle);
        }
        Dmod_Free(process->streams[i].path);
    }

    Dmod_Free(process->name);
    Dmod_Free(process->pwd);
    Dmod_Free(process);

    Dmod_ExitCritical();
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_kill, (dmosi_process_t process, int status) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot kill NULL process\n");
        return -EINVAL;
    }
    DMOD_LOG_VERBOSE("Killing process %s of module %s (and its children) with status %d\n", process->name, process->module_name, status);

    if(!kill_process_tree(process, status))
    {
        DMOD_LOG_ERROR("Failed to kill process tree rooted at process %s of module %s\n", process->name, process->module_name);
        return -EFAULT;
    }

    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_wait, (dmosi_process_t process, int32_t timeout_ms) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot wait for NULL process\n");
        return -EINVAL;
    }
    DMOD_LOG_VERBOSE("Waiting for process %s of module %s to terminate with timeout %d ms\n", process->name, process->module_name, timeout_ms);

    int elapsed = 0;
    const int poll_interval = 100; // Poll every 100 ms

    while(process->state != DMOSI_PROCESS_STATE_TERMINATED)
    {
        if(timeout_ms >= 0 && elapsed >= timeout_ms)
        {
            DMOD_LOG_WARN("Timeout while waiting for process %s of module %s to terminate\n", process->name, process->module_name);
            return -ETIMEDOUT;
        }
        dmosi_thread_sleep(poll_interval);
        elapsed += poll_interval;
    }

    DMOD_LOG_VERBOSE("Process %s of module %s has terminated with exit status %d\n", process->name, process->module_name, process->exit_status);

    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_t, _process_current,   (void) )
{
    if(!dmosi_is_started())
    {
        return NULL;
    }
    // Deliberately not logged: having no current thread/process is a routine condition
    // here (early boot, driver-owned threads never registered with dmosi, ...), not an
    // error - and logging from this exact path is dangerous. Any log call eventually
    // reaches Dmod_LockStdio, which itself calls dmosi_process_current() to resolve
    // whose stdout to lock - on the very same "no process" thread, that recurses
    // straight back into this branch, logs again, locks again, and so on, blowing the
    // stack in a handful of frames (this has been observed to hard-fault the target).
    dmosi_thread_t current_thread = dmosi_thread_current();
    if(!current_thread)
    {
        return NULL;
    }
    dmosi_process_t process = dmosi_thread_get_process(current_thread);
    if(!process)
    {
        return NULL;
    }
    return process;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_get_exit_status, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot get exit status of NULL process\n");
        return -EINVAL;
    }
    return process->exit_status;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_state_t, _process_get_state, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot get state of NULL process\n");
        return -EINVAL;
    }
    return process->state;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_id_t, _process_get_id, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot get ID of NULL process\n");
        return 0;
    }
    return process->pid;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, const char*, _process_get_name, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot get name of NULL process\n");
        return NULL;
    }
    return process->name;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, const char*, _process_get_module_name, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot get module name of NULL process\n");
        return NULL;
    }
    return process->module_name;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_set_context, (dmosi_process_t process, Dmod_Context_t* context) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to set context\n");
        return -EINVAL;
    }
    process->context = context;
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, Dmod_Context_t*, _process_get_context, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot get context of NULL process\n");
        return NULL;
    }
    return process->context;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_set_foreground_module, (dmosi_process_t process, Dmod_Context_t* context) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to set foreground module\n");
        return -EINVAL;
    }
    process->foreground_module = context;
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, Dmod_Context_t*, _process_get_foreground_module, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot get foreground module of NULL process\n");
        return NULL;
    }
    // No foreground module has been explicitly pushed (no synchronous nested call is in
    // progress): the process's own module - set via dmosi_process_set_context at spawn
    // time - is by definition the one currently running, so it's the foreground module.
    return process->foreground_module ? process->foreground_module : process->context;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int,            _process_set_uid,   (dmosi_process_t process, dmosi_user_id_t uid) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to set UID\n");
        return -EINVAL;
    }
    process->uid = uid;
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_user_id_t, _process_get_uid,   (dmosi_process_t process) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to get UID\n");
        return 0;
    }
    return process->uid;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_t, _process_get_parent, (dmosi_process_t process) )
{
    if(!process)
    {
        DMOD_LOG_ERROR("Cannot get parent of NULL process\n");
        return NULL;
    }
    return process->parent;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_set_id, (dmosi_process_t process, dmosi_process_id_t pid) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to set process ID\n");
        return -EINVAL;
    }
    DMOD_LOG_VERBOSE("Setting process ID of %s to %u\n", process->name, pid);
    process->pid = pid;
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_set_module_name, (dmosi_process_t process, const char* module_name) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to set module name\n");
        return -EINVAL;
    }
    if(!module_name)
    {
        DMOD_LOG_ERROR("Module name cannot be NULL\n");
        return -EINVAL;
    }
    if(strlen(module_name) >= DMOD_MAX_MODULE_NAME_LENGTH)
    {
        DMOD_LOG_ERROR("Module name too long: %s\n", module_name);
        return -EINVAL;
    }
    DMOD_LOG_VERBOSE("Setting module name of process %s to %s\n", process->name, module_name);
    strcpy(process->module_name, module_name);
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_set_pwd, (dmosi_process_t process, const char* pwd) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to set working directory\n");
        return -EINVAL;
    }
    if(!pwd)
    {
        DMOD_LOG_ERROR("Working directory cannot be NULL\n");
        return -EINVAL;
    }
    DMOD_LOG_VERBOSE("Setting working directory of process %s to %s\n", process->name, pwd);
    
    // Free existing pwd if any
    if(process->pwd)
    {
        Dmod_Free(process->pwd);
    }
    
    // Allocate and copy new pwd
    process->pwd = Dmod_StrDup(pwd);
    if(!process->pwd)
    {
        DMOD_LOG_ERROR("Failed to allocate memory for working directory\n");
        return -ENOMEM;
    }
    
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, const char*, _process_get_pwd, (dmosi_process_t process) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to get working directory\n");
        return NULL;
    }
    
    // Return stored pwd or default to root if not set
    return process->pwd ? process->pwd : "/";
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_set_stream, (dmosi_process_t process, dmosi_stream_index_t index, const char* path) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to set stream\n");
        return -EINVAL;
    }
    if(!validate_stream_index(index))
    {
        DMOD_LOG_ERROR("Invalid stream index %d provided to set stream\n", (int)index);
        return -EINVAL;
    }

    dmosi_process_stream_t* stream = &process->streams[index];

    if(!path)
    {
        // Clear the binding: close the file backing this slot (if any) and revert to the
        // unbound state, where callers fall back to their own default resolution.
        if(stream->handle)
        {
            Dmod_FileClose(stream->handle);
        }
        Dmod_Free(stream->path);
        stream->path = NULL;
        stream->handle = NULL;

        DMOD_LOG_VERBOSE("Cleared stream %d of process %s\n", (int)index, process->name);
        return 0;
    }

    void* handle = Dmod_FileOpen(path, stream_open_mode(index));
    if(!handle)
    {
        DMOD_LOG_ERROR("Failed to open %s for stream %d of process %s\n", path, (int)index, process->name);
        return -EIO;
    }

    char* new_path = Dmod_StrDup(path);
    if(!new_path)
    {
        DMOD_LOG_ERROR("Failed to allocate memory for stream path\n");
        Dmod_FileClose(handle);
        return -ENOMEM;
    }

    if(stream->handle)
    {
        Dmod_FileClose(stream->handle);
    }
    Dmod_Free(stream->path);

    stream->path = new_path;
    stream->handle = handle;

    DMOD_LOG_VERBOSE("Set stream %d of process %s to %s\n", (int)index, process->name, path);
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, void*, _process_get_stream, (dmosi_process_t process, dmosi_stream_index_t index) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to get stream\n");
        return NULL;
    }
    if(!validate_stream_index(index))
    {
        DMOD_LOG_ERROR("Invalid stream index %d provided to get stream\n", (int)index);
        return NULL;
    }

    // NULL (unset) is a normal result here: callers fall back to raw kernel I/O in that case.
    return process->streams[index].handle;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, const char*, _process_get_stream_path, (dmosi_process_t process, dmosi_stream_index_t index) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to get stream path\n");
        return NULL;
    }
    if(!validate_stream_index(index))
    {
        DMOD_LOG_ERROR("Invalid stream index %d provided to get stream path\n", (int)index);
        return NULL;
    }

    // NULL (unset) is a normal result here: it means the slot has no explicit binding.
    return process->streams[index].path;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_lock_stream, (dmosi_process_t process, dmosi_stream_index_t index) )
{
    if(!validate_process(process) || !validate_stream_index(index))
    {
        return -EINVAL;
    }

    // No logging in this path: it must be interrupt-safe and re-entrancy-safe,
    // since it is what guards logging itself against recursing into itself.
    dmosi_process_stream_t* stream = &process->streams[index];

    Dmod_EnterCritical();
    if(stream->locked)
    {
        Dmod_ExitCritical();
        return -EBUSY;
    }
    stream->locked = true;
    Dmod_ExitCritical();

    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_unlock_stream, (dmosi_process_t process, dmosi_stream_index_t index) )
{
    if(!validate_process(process) || !validate_stream_index(index))
    {
        return -EINVAL;
    }

    dmosi_process_stream_t* stream = &process->streams[index];

    Dmod_EnterCritical();
    stream->locked = false;
    Dmod_ExitCritical();

    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_set_exit_status, (dmosi_process_t process, int exit_status) )
{
    if(!validate_process(process))
    {
        DMOD_LOG_ERROR("Invalid process handle provided to set exit status\n");
        return -EINVAL;
    }
    DMOD_LOG_VERBOSE("Setting exit status of process %s to %d\n", process->name, exit_status);
    process->exit_status = exit_status;
    return 0;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_t, _process_find_by_name, (const char* name) )
{
    if(!name)
    {
        DMOD_LOG_ERROR("Process name cannot be NULL\n");
        return NULL;
    }
    
    return find_process_with_predicate(process_name_predicate, (void*)name, name);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_t, _process_find_by_id, (dmosi_process_id_t pid) )
{
    if(pid == 0)
    {
        DMOD_LOG_ERROR("Process ID cannot be 0\n");
        return NULL;
    }
    
    char pid_str[32];
    Dmod_SnPrintf(pid_str, sizeof(pid_str), "ID %u", pid);
    return find_process_with_predicate(process_id_predicate, &pid, pid_str);
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, dmosi_process_exit_callback_handle_t, _process_register_exit_callback, (dmosi_process_t process, dmosi_process_exit_callback_t callback, void* arg) )
{
    if(!validate_process(process) || !callback)
    {
        DMOD_LOG_ERROR("Invalid process handle or callback provided to register exit callback\n");
        return NULL;
    }

    struct dmosi_process_exit_callback* node = Dmod_MallocEx(sizeof(*node), process->module_name);
    if(!node)
    {
        DMOD_LOG_ERROR("Failed to allocate memory for exit callback registration on process %s\n", process->name);
        return NULL;
    }
    node->callback = callback;
    node->arg = arg;

    Dmod_EnterCritical();
    bool already_terminated = (process->state == DMOSI_PROCESS_STATE_TERMINATED);
    if(!already_terminated)
    {
        node->next = process->exit_callbacks;
        process->exit_callbacks = node;
    }
    Dmod_ExitCritical();

    if(already_terminated)
    {
        // The process's exit callbacks have already run and been discarded, so there
        // is nothing left to attach this registration to.
        Dmod_Free(node);
        DMOD_LOG_WARN("Cannot register exit callback on already terminated process %s\n", process->name);
        return NULL;
    }

    DMOD_LOG_VERBOSE("Registered exit callback on process %s\n", process->name);
    return (dmosi_process_exit_callback_handle_t)node;
}

DMOD_INPUT_API_DECLARATION( dmosi, 1.0, int, _process_unregister_exit_callback, (dmosi_process_t process, dmosi_process_exit_callback_handle_t handle) )
{
    if(!validate_process(process) || !handle)
    {
        DMOD_LOG_ERROR("Invalid process handle or callback handle provided to unregister exit callback\n");
        return -EINVAL;
    }

    struct dmosi_process_exit_callback* target = (struct dmosi_process_exit_callback*)handle;

    Dmod_EnterCritical();
    struct dmosi_process_exit_callback** link = &process->exit_callbacks;
    while(*link != NULL && *link != target)
    {
        link = &(*link)->next;
    }
    bool found = (*link == target);
    if(found)
    {
        *link = target->next;
    }
    Dmod_ExitCritical();

    if(!found)
    {
        DMOD_LOG_WARN("Exit callback handle not found on process %s (already unregistered or already invoked)\n", process->name);
        return -EINVAL;
    }

    Dmod_Free(target);
    DMOD_LOG_VERBOSE("Unregistered exit callback on process %s\n", process->name);
    return 0;
}
