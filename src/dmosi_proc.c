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
    dmosi_process_t parent;                         /**< Parent process (NULL for detached processes) */
    char module_name[DMOD_MAX_MODULE_NAME_LENGTH];  /**< Name of the associated module */
    int exit_status;                                /**< Exit status code (set when process is killed) */
    dmosi_process_state_t state;                    /**< Current state of the process */
    dmosi_process_id_t pid;                         /**< Unique process ID */
    dmosi_user_id_t uid;                            /**< User ID associated with the process */
    char* pwd;                                      /**< Working directory path */
    dmosi_process_stream_t streams[DMOSI_STREAM_COUNT]; /**< Stream slots (stdin/stdout/stderr/stdlog) */
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
 * @brief Kill all threads associated with a process
 *
 * @param process Process handle whose threads to kill
 * @param status Exit status code to pass to threads
 * @return bool true on success, false on failure
 */
static bool kill_threads( dmosi_process_t process, int status )
{
    size_t count = dmosi_thread_get_by_process(process, NULL, 0);
    if(count == 0)
        return true; // No threads to kill

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
    memset(process->streams, 0, sizeof(process->streams));
    if(!process->name)
    {
        DMOD_LOG_ERROR("Failed to duplicate process name %s for module %s\n", name, module_name);
        Dmod_Free(process);
        return NULL;
    }

    process->context = NULL; /* set via dmosi_process_set_context once the module to run is known */
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
    DMOD_LOG_VERBOSE("Killing process %s of module %s with status %d\n", process->name, process->module_name, status);

    // Set exit_status/state *before* kill_threads(), not after: when a process kills its
    // own currently-running thread (the normal case - every spawned module calls this on
    // itself via Dmod_Exit when it finishes), dmosi_thread_kill() ends in vTaskDelete(NULL),
    // which never returns to its caller. Anything placed after kill_threads() here would
    // simply never run for that case, leaving the process stuck at its prior state forever
    // (observed as background jobs whose underlying task is long gone but that dmell's
    // reaper never sees as DMOSI_PROCESS_STATE_TERMINATED, so it never frees them).
    process->exit_status = status;
    process->state = DMOSI_PROCESS_STATE_TERMINATED;

    if(!kill_threads(process, status))
    {
        DMOD_LOG_ERROR("Failed to kill threads while killing process %s of module %s\n", process->name, process->module_name);
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
