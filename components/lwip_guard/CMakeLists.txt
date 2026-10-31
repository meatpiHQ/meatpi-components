idf_component_register(
    SRCS "lwip_guard.c"
         "lwip_guard_core.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    # lwip: the semaphore type of the wrapped getter; esp_timer: the clock
    # of the log rate limit; log_manager: the log descriptor
    PRIV_REQUIRES lwip esp_timer log_manager
)

# IDF's lwIP port hands every socket or resolver call the calling task's
# semaphore through sys_thread_sem_get(); the semaphore is a 92-byte
# internal-RAM allocation on the task's first call, and when that fails
# the getter returns NULL, which lwIP's validity check (a memcmp without a
# NULL guard, 6.0.2 and 6.1) dereferences: issue 1024. The link wrap puts
# this component in front of the getter (the ip4_routes / restart_tracker
# pattern): a NULL answer becomes a static zeroed semaphore, which lwIP
# reads as "none" and answers with ERR_MEM. The -u beside the wrap makes
# the linker take the wrapper out of this static library at its first
# scan (an on-target test app links every component in the tree).
target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=sys_thread_sem_get"
                      "-u __wrap_sys_thread_sem_get")
