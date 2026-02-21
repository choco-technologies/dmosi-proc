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

    // NULL arguments for setter functions
    dmosi_process_t proc = dmosi_process_create("null_arg_proc", "test_module", NULL);
    TEST_ASSERT(proc != NULL, "Create process for NULL argument tests");

    TEST_ASSERT(dmosi_process_set_module_name(proc, NULL) == -EINVAL,
                "Set NULL module name returns -EINVAL");

    TEST_ASSERT(dmosi_process_set_pwd(proc, NULL) == -EINVAL,
                "Set NULL PWD returns -EINVAL");

    dmosi_process_destroy(proc);
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
    test_process_exit_status();
    test_process_id();
    test_process_module_name();
    test_process_kill();
    test_process_wait();
    test_process_unique_ids();
    test_null_inputs();
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
