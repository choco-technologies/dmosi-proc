#define DMOD_ENABLE_REGISTRATION
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include "dmod.h"
#include "dmosi.h"

// Test result tracking
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(condition, test_name) \
    do { \
        if (condition) { \
            printf("✓ PASS: %s\n", test_name); \
            tests_passed++; \
        } else { \
            printf("✗ FAIL: %s\n", test_name); \
            tests_failed++; \
        } \
    } while(0)

// -----------------------------------------
//
//      Test: Process creation
//
// -----------------------------------------
void test_process_create(void)
{
    printf("\n=== Testing process creation ===\n");

    // Create a process with valid name and module name
    dmosi_process_t proc = dmosi_process_create("test_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process with valid name and module name");

    // Verify initial state is RUNNING
    TEST_ASSERT(dmosi_process_get_state(proc) == DMOSI_PROCESS_STATE_RUNNING,
                "Initial process state is RUNNING");

    // Verify name
    TEST_ASSERT(strcmp(dmosi_process_get_name(proc), "test_proc") == 0,
                "Process name matches");

    // Verify module name
    TEST_ASSERT(strcmp(dmosi_process_get_module_name(proc), "test_module") == 0,
                "Process module name matches");

    // Verify process ID is non-zero
    TEST_ASSERT(dmosi_process_get_id(proc) != 0,
                "Process ID is non-zero");

    // Verify no parent
    TEST_ASSERT(dmosi_process_get_parent(proc) == NULL,
                "Process parent is NULL for detached process");

    // Verify initial UID is 0
    TEST_ASSERT(dmosi_process_get_uid(proc) == 0,
                "Initial process UID is 0");

    // Verify default PWD is "/"
    TEST_ASSERT(strcmp(dmosi_process_get_pwd(proc), "/") == 0,
                "Default process PWD is '/'");

    // Verify initial exit status is 0
    TEST_ASSERT(dmosi_process_get_exit_status(proc) == 0,
                "Initial process exit status is 0");

    dmosi_process_destroy(proc);

    // Create a process with NULL module name (should default to "system")
    dmosi_process_t proc2 = dmosi_process_create("proc_no_module", NULL, NULL);
    TEST_ASSERT(proc2 != NULL, "Create process with NULL module name");
    TEST_ASSERT(strcmp(dmosi_process_get_module_name(proc2), "system") == 0,
                "NULL module name defaults to 'system'");
    dmosi_process_destroy(proc2);
}

// -----------------------------------------
//
//      Test: Process creation with NULL name
//
// -----------------------------------------
void test_process_create_null_name(void)
{
    printf("\n=== Testing process creation with NULL name ===\n");

    dmosi_process_t proc = dmosi_process_create(NULL, "test_module", NULL);
    TEST_ASSERT(proc == NULL, "Create process with NULL name returns NULL");
}

// -----------------------------------------
//
//      Test: Process parent-child relationship
//
// -----------------------------------------
void test_process_parent_child(void)
{
    printf("\n=== Testing process parent-child relationship ===\n");

    dmosi_process_t parent = dmosi_process_create("parent_proc", "test_module", NULL);
    TEST_ASSERT(parent != NULL, "Create parent process");

    dmosi_process_t child = dmosi_process_create("child_proc", "test_module", parent);
    TEST_ASSERT(child != NULL, "Create child process with parent");

    TEST_ASSERT(dmosi_process_get_parent(child) == parent,
                "Child process parent matches");

    dmosi_process_destroy(child);
    dmosi_process_destroy(parent);
}

// -----------------------------------------
//
//      Test: Process UID management
//
// -----------------------------------------
void test_process_uid(void)
{
    printf("\n=== Testing process UID management ===\n");

    dmosi_process_t proc = dmosi_process_create("uid_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for UID test");

    // Set UID
    TEST_ASSERT(dmosi_process_set_uid(proc, 42) == 0,
                "Set process UID to 42");

    // Get UID
    TEST_ASSERT(dmosi_process_get_uid(proc) == 42,
                "Get process UID returns 42");

    // Set UID to 0
    TEST_ASSERT(dmosi_process_set_uid(proc, 0) == 0,
                "Set process UID to 0");
    TEST_ASSERT(dmosi_process_get_uid(proc) == 0,
                "Get process UID returns 0 after reset");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Process working directory
//
// -----------------------------------------
void test_process_pwd(void)
{
    printf("\n=== Testing process working directory ===\n");

    dmosi_process_t proc = dmosi_process_create("pwd_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for PWD test");

    // Default PWD should be "/"
    TEST_ASSERT(strcmp(dmosi_process_get_pwd(proc), "/") == 0,
                "Default PWD is '/'");

    // Set PWD
    TEST_ASSERT(dmosi_process_set_pwd(proc, "/home/user") == 0,
                "Set process PWD to '/home/user'");

    // Get PWD
    TEST_ASSERT(strcmp(dmosi_process_get_pwd(proc), "/home/user") == 0,
                "Get process PWD returns '/home/user'");

    // Update PWD
    TEST_ASSERT(dmosi_process_set_pwd(proc, "/tmp") == 0,
                "Update process PWD to '/tmp'");
    TEST_ASSERT(strcmp(dmosi_process_get_pwd(proc), "/tmp") == 0,
                "Get process PWD returns '/tmp' after update");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Process standard streams (stdin/stdout/stderr/stdlog)
//
// -----------------------------------------
void test_process_stdio(void)
{
    printf("\n=== Testing process standard streams ===\n");

    // The stdin slot is opened for reading, so it needs a file that already exists.
    const char* stdin_path = "/tmp/dmosi_test_stdin.txt";
    FILE* seed = fopen(stdin_path, "w");
    TEST_ASSERT(seed != NULL, "Create seed file for stdin stream test");
    if(seed)
    {
        fputs("test\n", seed);
        fclose(seed);
    }

    dmosi_process_t proc = dmosi_process_create("stdio_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for stdio test");

    // Default streams should be unset (NULL)
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDIN) == NULL, "Default stdin stream is NULL");
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDOUT) == NULL, "Default stdout stream is NULL");
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDERR) == NULL, "Default stderr stream is NULL");
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDLOG) == NULL, "Default stdlog stream is NULL");

    // Set streams (stdin must point at an existing file; stdout/stderr/stdlog are appended to)
    TEST_ASSERT(dmosi_process_set_stream(proc, DMOSI_STREAM_STDIN, stdin_path) == 0, "Set stdin stream");
    TEST_ASSERT(dmosi_process_set_stream(proc, DMOSI_STREAM_STDOUT, "/tmp/dmosi_test_stdout.log") == 0, "Set stdout stream");
    TEST_ASSERT(dmosi_process_set_stream(proc, DMOSI_STREAM_STDERR, "/tmp/dmosi_test_stderr.log") == 0, "Set stderr stream");
    TEST_ASSERT(dmosi_process_set_stream(proc, DMOSI_STREAM_STDLOG, "/tmp/dmosi_test_stdlog.log") == 0, "Set stdlog stream");

    // Get streams: each slot should now hold an open file handle
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDIN) != NULL, "Get stdin stream returns a handle");
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDOUT) != NULL, "Get stdout stream returns a handle");
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDERR) != NULL, "Get stderr stream returns a handle");
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDLOG) != NULL, "Get stdlog stream returns a handle");

    // Re-binding a slot must close the previous handle without leaking/crashing
    void* old_stdout_handle = dmosi_process_get_stream(proc, DMOSI_STREAM_STDOUT);
    TEST_ASSERT(dmosi_process_set_stream(proc, DMOSI_STREAM_STDOUT, "/tmp/dmosi_test_stdout2.log") == 0, "Re-bind stdout stream to a new path");
    TEST_ASSERT(dmosi_process_get_stream(proc, DMOSI_STREAM_STDOUT) != old_stdout_handle, "Re-binding stdout stream produces a new handle");

    dmosi_process_destroy(proc);

    remove(stdin_path);
    remove("/tmp/dmosi_test_stdout.log");
    remove("/tmp/dmosi_test_stdout2.log");
    remove("/tmp/dmosi_test_stderr.log");
    remove("/tmp/dmosi_test_stdlog.log");
}

// -----------------------------------------
//
//      Test: Process stream lock/unlock
//
// -----------------------------------------
void test_process_stream_lock(void)
{
    printf("\n=== Testing process stream lock/unlock ===\n");

    dmosi_process_t proc = dmosi_process_create("lock_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for stream lock test");

    TEST_ASSERT(dmosi_process_lock_stream(proc, DMOSI_STREAM_STDLOG) == 0,
                "Lock stdlog stream succeeds the first time");

    TEST_ASSERT(dmosi_process_lock_stream(proc, DMOSI_STREAM_STDLOG) == -EBUSY,
                "Locking an already-locked stream returns -EBUSY (recursion guard)");

    // Other slots are independent of the locked one
    TEST_ASSERT(dmosi_process_lock_stream(proc, DMOSI_STREAM_STDOUT) == 0,
                "Locking a different stream slot is unaffected by another slot's lock");
    TEST_ASSERT(dmosi_process_unlock_stream(proc, DMOSI_STREAM_STDOUT) == 0,
                "Unlock stdout stream succeeds");

    TEST_ASSERT(dmosi_process_unlock_stream(proc, DMOSI_STREAM_STDLOG) == 0,
                "Unlock stdlog stream succeeds");

    TEST_ASSERT(dmosi_process_lock_stream(proc, DMOSI_STREAM_STDLOG) == 0,
                "Stdlog stream can be locked again after unlocking");
    TEST_ASSERT(dmosi_process_unlock_stream(proc, DMOSI_STREAM_STDLOG) == 0,
                "Unlock stdlog stream succeeds again");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Process stream inheritance
//
// -----------------------------------------
void test_process_stream_inheritance(void)
{
    printf("\n=== Testing process stream inheritance ===\n");

    const char* log_path = "/tmp/dmosi_test_inherited_stdlog.log";

    dmosi_process_t parent = dmosi_process_create("stream_parent", "test_module", NULL);
    TEST_ASSERT(parent != NULL, "Create parent process for stream inheritance test");

    TEST_ASSERT(dmosi_process_set_stream(parent, DMOSI_STREAM_STDLOG, log_path) == 0,
                "Set stdlog stream on parent process");
    TEST_ASSERT(dmosi_process_get_stream(parent, DMOSI_STREAM_STDOUT) == NULL,
                "Parent stdout stream is left unset");

    dmosi_process_t child = dmosi_process_create("stream_child", "test_module", parent);
    TEST_ASSERT(child != NULL, "Create child process under parent with a bound stream");

    void* parent_handle = dmosi_process_get_stream(parent, DMOSI_STREAM_STDLOG);
    void* child_handle = dmosi_process_get_stream(child, DMOSI_STREAM_STDLOG);
    TEST_ASSERT(child_handle != NULL, "Child inherits a bound stdlog stream handle");
    TEST_ASSERT(child_handle != parent_handle, "Child gets its own handle, distinct from the parent's");

    TEST_ASSERT(dmosi_process_get_stream(child, DMOSI_STREAM_STDOUT) == NULL,
                "Child does not inherit a stream slot the parent never set");

    // Destroying the child must not affect the parent's still-open handle
    dmosi_process_destroy(child);
    TEST_ASSERT(dmosi_process_get_stream(parent, DMOSI_STREAM_STDLOG) == parent_handle,
                "Parent stream handle is unaffected by destroying the child");

    dmosi_process_destroy(parent);

    remove(log_path);
}

// -----------------------------------------
//
//      Test: Process exit status
//
// -----------------------------------------
void test_process_exit_status(void)
{
    printf("\n=== Testing process exit status ===\n");

    dmosi_process_t proc = dmosi_process_create("exit_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for exit status test");

    // Initial exit status should be 0
    TEST_ASSERT(dmosi_process_get_exit_status(proc) == 0,
                "Initial exit status is 0");

    // Set exit status
    TEST_ASSERT(dmosi_process_set_exit_status(proc, 42) == 0,
                "Set process exit status to 42");

    // Get exit status
    TEST_ASSERT(dmosi_process_get_exit_status(proc) == 42,
                "Get process exit status returns 42");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Process ID management
//
// -----------------------------------------
void test_process_id(void)
{
    printf("\n=== Testing process ID management ===\n");

    dmosi_process_t proc = dmosi_process_create("id_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for ID test");

    // Verify auto-assigned ID is non-zero
    dmosi_process_id_t auto_id = dmosi_process_get_id(proc);
    TEST_ASSERT(auto_id != 0, "Auto-assigned process ID is non-zero");

    // Set a new process ID
    TEST_ASSERT(dmosi_process_set_id(proc, 123) == 0,
                "Set process ID to 123");

    // Verify new ID
    TEST_ASSERT(dmosi_process_get_id(proc) == 123,
                "Get process ID returns 123 after set");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Process module name management
//
// -----------------------------------------
void test_process_module_name(void)
{
    printf("\n=== Testing process module name management ===\n");

    dmosi_process_t proc = dmosi_process_create("module_proc", "old_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for module name test");

    // Verify initial module name
    TEST_ASSERT(strcmp(dmosi_process_get_module_name(proc), "old_module") == 0,
                "Initial module name matches");

    // Set new module name
    TEST_ASSERT(dmosi_process_set_module_name(proc, "new_module") == 0,
                "Set process module name to 'new_module'");

    // Verify new module name
    TEST_ASSERT(strcmp(dmosi_process_get_module_name(proc), "new_module") == 0,
                "Get process module name returns 'new_module'");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Process foreground module management
//
// -----------------------------------------
void test_process_foreground_module(void)
{
    printf("\n=== Testing process foreground module management ===\n");

    dmosi_process_t proc = dmosi_process_create("foreground_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for foreground module test");

    // Verify initial foreground module is unset
    TEST_ASSERT(dmosi_process_get_foreground_module(proc) == NULL,
                "Initial foreground module is NULL");

    // Set foreground module context
    Dmod_Context_t context_a;
    TEST_ASSERT(dmosi_process_set_foreground_module(proc, &context_a) == 0,
                "Set foreground module context");

    TEST_ASSERT(dmosi_process_get_foreground_module(proc) == &context_a,
                "Get foreground module returns previously set context");

    // Simulate a nested/synchronous call switching the foreground context
    Dmod_Context_t context_b;
    TEST_ASSERT(dmosi_process_set_foreground_module(proc, &context_b) == 0,
                "Set foreground module to a nested context");

    TEST_ASSERT(dmosi_process_get_foreground_module(proc) == &context_b,
                "Get foreground module returns nested context");

    // Restore the previous foreground context
    TEST_ASSERT(dmosi_process_set_foreground_module(proc, &context_a) == 0,
                "Restore previous foreground module context");

    TEST_ASSERT(dmosi_process_get_foreground_module(proc) == &context_a,
                "Get foreground module returns restored context");

    // Clear foreground module
    TEST_ASSERT(dmosi_process_set_foreground_module(proc, NULL) == 0,
                "Clear foreground module context");

    TEST_ASSERT(dmosi_process_get_foreground_module(proc) == NULL,
                "Get foreground module returns NULL after clearing");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Process kill
//
// -----------------------------------------
void test_process_kill(void)
{
    printf("\n=== Testing process kill ===\n");

    dmosi_process_t proc = dmosi_process_create("kill_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for kill test");

    // Verify initial state
    TEST_ASSERT(dmosi_process_get_state(proc) == DMOSI_PROCESS_STATE_RUNNING,
                "Process is RUNNING before kill");

    // Kill the process
    TEST_ASSERT(dmosi_process_kill(proc, 1) == 0,
                "Kill process returns 0");

    // Verify state is now TERMINATED
    TEST_ASSERT(dmosi_process_get_state(proc) == DMOSI_PROCESS_STATE_TERMINATED,
                "Process state is TERMINATED after kill");

    // Verify exit status is set
    TEST_ASSERT(dmosi_process_get_exit_status(proc) == 1,
                "Exit status is set to kill status");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Process wait
//
// -----------------------------------------
void test_process_wait(void)
{
    printf("\n=== Testing process wait ===\n");

    // Test waiting on an already-terminated process
    dmosi_process_t proc = dmosi_process_create("wait_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for wait test");

    dmosi_process_kill(proc, 0);

    TEST_ASSERT(dmosi_process_wait(proc, -1) == 0,
                "Wait on terminated process returns 0");

    dmosi_process_destroy(proc);

    // Test timeout on a running process
    dmosi_process_t proc2 = dmosi_process_create("timeout_proc", "test_module", NULL);
    TEST_ASSERT(proc2 != NULL, "Create process for timeout test");

    TEST_ASSERT(dmosi_process_wait(proc2, 0) == -ETIMEDOUT,
                "Wait with timeout=0 on running process returns -ETIMEDOUT");

    dmosi_process_destroy(proc2);
}

// -----------------------------------------
//
//      Test: Unique process IDs
//
// -----------------------------------------
void test_process_unique_ids(void)
{
    printf("\n=== Testing unique process IDs ===\n");

    dmosi_process_t proc1 = dmosi_process_create("unique_proc1", "test_module", NULL);
    dmosi_process_t proc2 = dmosi_process_create("unique_proc2", "test_module", NULL);

    TEST_ASSERT(proc1 != NULL && proc2 != NULL, "Create two processes");

    TEST_ASSERT(dmosi_process_get_id(proc1) != dmosi_process_get_id(proc2),
                "Two processes have different IDs");

    dmosi_process_destroy(proc1);
    dmosi_process_destroy(proc2);
}

// -----------------------------------------
//
//      Test: Process exit callbacks
//
// -----------------------------------------

static int g_callback_invocations = 0;
static dmosi_process_t g_last_callback_process = NULL;
static int g_last_callback_exit_status = 0;
static void* g_last_callback_arg = NULL;

static void test_exit_callback(dmosi_process_t process, int exit_status, void* arg)
{
    g_callback_invocations++;
    g_last_callback_process = process;
    g_last_callback_exit_status = exit_status;
    g_last_callback_arg = arg;
}

static void test_exit_callback_second(dmosi_process_t process, int exit_status, void* arg)
{
    (void)process;
    (void)exit_status;
    g_callback_invocations++;
    g_last_callback_arg = arg;
}

void test_process_exit_callback(void)
{
    printf("\n=== Testing process exit callbacks ===\n");

    // A single callback is invoked exactly once, with the right process/status/arg, on kill
    g_callback_invocations = 0;
    g_last_callback_process = NULL;
    g_last_callback_exit_status = 0;
    g_last_callback_arg = NULL;

    dmosi_process_t proc = dmosi_process_create("callback_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for exit callback test");

    int marker = 123;
    dmosi_process_exit_callback_handle_t handle = dmosi_process_register_exit_callback(proc, test_exit_callback, &marker);
    TEST_ASSERT(handle != NULL, "Register exit callback returns a handle");

    TEST_ASSERT(dmosi_process_kill(proc, 7) == 0, "Kill process with registered callback");

    TEST_ASSERT(g_callback_invocations == 1, "Exit callback invoked exactly once on kill");
    TEST_ASSERT(g_last_callback_process == proc, "Exit callback receives the terminated process");
    TEST_ASSERT(g_last_callback_exit_status == 7, "Exit callback receives the exit status");
    TEST_ASSERT(g_last_callback_arg == &marker, "Exit callback receives the registered argument");

    // Destroying an already-killed process must not re-invoke the callback
    dmosi_process_destroy(proc);
    TEST_ASSERT(g_callback_invocations == 1, "Destroying an already-killed process does not re-invoke callback");

    // Multiple callbacks on the same process are all invoked
    g_callback_invocations = 0;
    dmosi_process_t proc2 = dmosi_process_create("multi_callback_proc", "test_module", NULL);
    TEST_ASSERT(proc2 != NULL, "Create process for multiple exit callback test");

    TEST_ASSERT(dmosi_process_register_exit_callback(proc2, test_exit_callback, NULL) != NULL,
                "Register first callback on process");
    TEST_ASSERT(dmosi_process_register_exit_callback(proc2, test_exit_callback_second, NULL) != NULL,
                "Register second callback on process");

    dmosi_process_kill(proc2, 0);
    TEST_ASSERT(g_callback_invocations == 2, "Both registered callbacks are invoked on kill");

    dmosi_process_destroy(proc2);

    // A process destroyed without ever being killed still fires its exit callbacks
    g_callback_invocations = 0;
    dmosi_process_t proc3 = dmosi_process_create("destroy_only_proc", "test_module", NULL);
    TEST_ASSERT(proc3 != NULL, "Create process for destroy-only exit callback test");
    TEST_ASSERT(dmosi_process_register_exit_callback(proc3, test_exit_callback, NULL) != NULL,
                "Register callback on process that will only be destroyed");
    dmosi_process_destroy(proc3);
    TEST_ASSERT(g_callback_invocations == 1, "Exit callback fires on destroy even without an explicit kill");

    // Unregistering a callback prevents it from being invoked
    g_callback_invocations = 0;
    dmosi_process_t proc4 = dmosi_process_create("unregister_proc", "test_module", NULL);
    TEST_ASSERT(proc4 != NULL, "Create process for unregister test");

    dmosi_process_exit_callback_handle_t handle4 = dmosi_process_register_exit_callback(proc4, test_exit_callback, NULL);
    TEST_ASSERT(handle4 != NULL, "Register callback for unregister test");
    TEST_ASSERT(dmosi_process_unregister_exit_callback(proc4, handle4) == 0,
                "Unregister previously registered callback succeeds");

    dmosi_process_kill(proc4, 0);
    TEST_ASSERT(g_callback_invocations == 0, "Unregistered callback is not invoked on kill");

    dmosi_process_destroy(proc4);

    // Unregistering the same handle twice fails the second time
    g_callback_invocations = 0;
    dmosi_process_t proc5 = dmosi_process_create("double_unregister_proc", "test_module", NULL);
    TEST_ASSERT(proc5 != NULL, "Create process for double-unregister test");

    dmosi_process_exit_callback_handle_t handle5 = dmosi_process_register_exit_callback(proc5, test_exit_callback, NULL);
    TEST_ASSERT(handle5 != NULL, "Register callback for double-unregister test");
    TEST_ASSERT(dmosi_process_unregister_exit_callback(proc5, handle5) == 0,
                "First unregister of a handle succeeds");
    TEST_ASSERT(dmosi_process_unregister_exit_callback(proc5, handle5) == -EINVAL,
                "Second unregister of the same handle returns -EINVAL");

    dmosi_process_destroy(proc5);

    // Registering on an already-terminated process fails
    dmosi_process_t proc6 = dmosi_process_create("terminated_register_proc", "test_module", NULL);
    TEST_ASSERT(proc6 != NULL, "Create process for register-after-terminated test");
    dmosi_process_kill(proc6, 0);
    TEST_ASSERT(dmosi_process_register_exit_callback(proc6, test_exit_callback, NULL) == NULL,
                "Registering on an already-terminated process returns NULL");
    dmosi_process_destroy(proc6);

    // NULL handling
    TEST_ASSERT(dmosi_process_register_exit_callback(NULL, test_exit_callback, NULL) == NULL,
                "Register callback on NULL process returns NULL");

    dmosi_process_t proc7 = dmosi_process_create("null_callback_proc", "test_module", NULL);
    TEST_ASSERT(proc7 != NULL, "Create process for NULL callback test");
    TEST_ASSERT(dmosi_process_register_exit_callback(proc7, NULL, NULL) == NULL,
                "Register NULL callback function returns NULL");
    TEST_ASSERT(dmosi_process_unregister_exit_callback(proc7, NULL) == -EINVAL,
                "Unregister NULL handle returns -EINVAL");
    TEST_ASSERT(dmosi_process_unregister_exit_callback(NULL, (dmosi_process_exit_callback_handle_t)&marker) == -EINVAL,
                "Unregister on NULL process returns -EINVAL");
    dmosi_process_destroy(proc7);
}

// -----------------------------------------
//
//      Test: NULL input handling
//
// -----------------------------------------
void test_null_inputs(void)
{
    printf("\n=== Testing NULL input handling ===\n");

    // NULL process handle inputs
    TEST_ASSERT(dmosi_process_kill(NULL, 0) == -EINVAL,
                "Kill NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_wait(NULL, 0) == -EINVAL,
                "Wait on NULL process returns -EINVAL");

    TEST_ASSERT((int)dmosi_process_get_state(NULL) == -EINVAL,
                "Get state of NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_get_id(NULL) == 0,
                "Get ID of NULL process returns 0");

    TEST_ASSERT(dmosi_process_get_name(NULL) == NULL,
                "Get name of NULL process returns NULL");

    TEST_ASSERT(dmosi_process_get_module_name(NULL) == NULL,
                "Get module name of NULL process returns NULL");

    TEST_ASSERT(dmosi_process_get_parent(NULL) == NULL,
                "Get parent of NULL process returns NULL");

    TEST_ASSERT(dmosi_process_get_exit_status(NULL) == -EINVAL,
                "Get exit status of NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_get_uid(NULL) == 0,
                "Get UID of NULL process returns 0");

    TEST_ASSERT(dmosi_process_get_pwd(NULL) == NULL,
                "Get PWD of NULL process returns NULL");

    TEST_ASSERT(dmosi_process_get_foreground_module(NULL) == NULL,
                "Get foreground module of NULL process returns NULL");

    TEST_ASSERT(dmosi_process_set_foreground_module(NULL, NULL) == -EINVAL,
                "Set foreground module of NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_get_stream(NULL, DMOSI_STREAM_STDIN) == NULL,
                "Get stream of NULL process returns NULL");

    TEST_ASSERT(dmosi_process_lock_stream(NULL, DMOSI_STREAM_STDLOG) == -EINVAL,
                "Lock stream on NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_unlock_stream(NULL, DMOSI_STREAM_STDLOG) == -EINVAL,
                "Unlock stream on NULL process returns -EINVAL");

    // NULL name for find_by_name
    TEST_ASSERT(dmosi_process_find_by_name(NULL) == NULL,
                "Find by NULL name returns NULL");

    // Zero PID for find_by_id
    TEST_ASSERT(dmosi_process_find_by_id(0) == NULL,
                "Find by zero ID returns NULL");

    // NULL process handle for setter functions that use validate_process
    TEST_ASSERT(dmosi_process_set_uid(NULL, 1) == -EINVAL,
                "Set UID on NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_set_id(NULL, 1) == -EINVAL,
                "Set ID on NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_set_module_name(NULL, "module") == -EINVAL,
                "Set module name on NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_set_pwd(NULL, "/") == -EINVAL,
                "Set PWD on NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_set_exit_status(NULL, 0) == -EINVAL,
                "Set exit status on NULL process returns -EINVAL");

    TEST_ASSERT(dmosi_process_set_stream(NULL, DMOSI_STREAM_STDOUT, "/tmp/dmosi_test_null.log") == -EINVAL,
                "Set stream on NULL process returns -EINVAL");

    // NULL arguments for setter functions
    dmosi_process_t proc = dmosi_process_create("null_arg_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for NULL argument tests");

    TEST_ASSERT(dmosi_process_set_module_name(proc, NULL) == -EINVAL,
                "Set NULL module name returns -EINVAL");

    TEST_ASSERT(dmosi_process_set_pwd(proc, NULL) == -EINVAL,
                "Set NULL PWD returns -EINVAL");

    TEST_ASSERT(dmosi_process_set_stream(proc, DMOSI_STREAM_STDOUT, NULL) == 0,
                "Set stream with NULL path clears the binding and returns 0");

    TEST_ASSERT(dmosi_process_set_stream(proc, (dmosi_stream_index_t)999, "/tmp/dmosi_test_null.log") == -EINVAL,
                "Set stream with out-of-range index returns -EINVAL");

    TEST_ASSERT(dmosi_process_get_stream(proc, (dmosi_stream_index_t)999) == NULL,
                "Get stream with out-of-range index returns NULL");

    TEST_ASSERT(dmosi_process_lock_stream(proc, (dmosi_stream_index_t)999) == -EINVAL,
                "Lock stream with out-of-range index returns -EINVAL");

    dmosi_process_destroy(proc);
}

// -----------------------------------------
//
//      Test: Current process before system start
//
// -----------------------------------------
void test_process_current_before_start(void)
{
    printf("\n=== Testing process current before system start ===\n");

    // dmosi_is_started() returns false in the test environment (no RTOS scheduler),
    // so dmosi_process_current() should return NULL silently without logging an error.
    TEST_ASSERT(dmosi_process_current() == NULL,
                "process_current returns NULL when system has not started");
}

// -----------------------------------------
//
//      Test: Find process by name/ID (without threads)
//
// -----------------------------------------
void test_process_find(void)
{
    printf("\n=== Testing process find functions (no threads) ===\n");

    // Without threads, find functions should return NULL (no threads to search)
    TEST_ASSERT(dmosi_process_find_by_name("any_proc") == NULL,
                "Find by name returns NULL when no threads exist");

    TEST_ASSERT(dmosi_process_find_by_id(1) == NULL,
                "Find by ID returns NULL when no threads exist");
}

// -----------------------------------------
//
//      Main function
//
// -----------------------------------------
int main(void)
{
    printf("========================================\n");
    printf("  DMOSI Process Implementation Tests\n");
    printf("========================================\n");

    test_process_create();
    test_process_create_null_name();
    test_process_parent_child();
    test_process_uid();
    test_process_pwd();
    test_process_stdio();
    test_process_stream_lock();
    test_process_stream_inheritance();
    test_process_exit_status();
    test_process_id();
    test_process_module_name();
    test_process_foreground_module();
    test_process_kill();
    test_process_exit_callback();
    test_process_wait();
    test_process_unique_ids();
    test_null_inputs();
    test_process_current_before_start();
    test_process_find();

    printf("\n========================================\n");
    printf("  Test Summary\n");
    printf("========================================\n");
    printf("Total tests: %d\n", tests_passed + tests_failed);
    printf("Passed:      %d\n", tests_passed);
    printf("Failed:      %d\n", tests_failed);
    printf("========================================\n");

    if (tests_failed == 0) {
        printf("\n✓ ALL TESTS PASSED\n\n");
        return 0;
    } else {
        printf("\n✗ SOME TESTS FAILED\n\n");
        return 1;
    }
}
