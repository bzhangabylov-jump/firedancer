#ifndef HEADER_fd_src_disco_fd_scheduler_shm_h
#define HEADER_fd_src_disco_fd_scheduler_shm_h

#include <sys/mman.h>
#include <linux/unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdio.h>
#include "../../tango/fd_tango.h"

extern long syscall(long number, ...);

/* Shared memory structure matching the one in scx_firedancer.c */
struct fd_scheduler_shm {
    int scheduler_pid;
    int test_counter;
    int is_leader;
    char message[256];
    struct {
        int pid;
        int registered;
        int idle;
        long deadline_ts;
        int cpu_id;
        char name[64];
    } tiles[50];
};

/* Global shared memory pointer for tiles */
static struct fd_scheduler_shm * g_scheduler_shm = NULL;
static int g_tile_id = -1;

/* Initialize shared memory connection for a tile */
static inline void
fd_scheduler_shm_init( const char * tile_name, int cpu_id ) {
    /* Try to open existing shared memory */
    int shm_fd = shm_open( "/fd_scheduler_shm", O_RDWR, 0666 );
    if( shm_fd < 0 ) {
        /* Scheduler not running, continue without it */
        FD_LOG_ERR(( "Scheduler shared memory not found - running without scheduler integration, errno: %d", errno ));
        return;
    }

    /* Map the shared memory */
    g_scheduler_shm = mmap( NULL, sizeof(struct fd_scheduler_shm),
                           PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0 );
    close( shm_fd );

    if( g_scheduler_shm == MAP_FAILED ) {
        FD_LOG_ERR(( "Failed to map scheduler shared memory: %s", strerror(errno) ));
        g_scheduler_shm = NULL;
        return;
    }

    /* Find a free tile slot */
    for( int i = 0; i < 50; i++ ) {
        if( !g_scheduler_shm->tiles[i].registered ) {
            g_tile_id = i;
            g_scheduler_shm->tiles[i].registered = 1;
            g_scheduler_shm->tiles[i].idle = 0;
            g_scheduler_shm->tiles[i].pid = (int)syscall(SYS_gettid);
            g_scheduler_shm->tiles[i].cpu_id = cpu_id;
            strncpy( g_scheduler_shm->tiles[i].name, tile_name, 63 );
            g_scheduler_shm->tiles[i].name[63] = '\0';
            FD_LOG_INFO(( "Tile %s registered with scheduler as tile %d (pid=%d cpu=%d)", tile_name, i, g_scheduler_shm->tiles[i].pid, g_scheduler_shm->tiles[i].cpu_id ));
            break;
        }
    }

    if( g_tile_id == -1 ) {
        FD_LOG_ERR(( "No free scheduler tile slots available" ));
    }
}

/* Update tile activity status */
static inline void
fd_scheduler_shm_idle_update( int is_idle ) {
    if( g_scheduler_shm && g_tile_id >= 0 ) {
        g_scheduler_shm->tiles[g_tile_id].idle = is_idle;
        g_scheduler_shm->test_counter++;
    }
}

static inline void
fd_scheduler_shm_deadline_update( long deadline_ts ) {
    if( g_scheduler_shm && g_tile_id >= 0 ) {
        g_scheduler_shm->tiles[g_tile_id].deadline_ts = deadline_ts;
        g_scheduler_shm->test_counter++;
    }
}

static inline void
fd_scheduler_shm_leader_update( ulong is_leader ) {
    if( g_scheduler_shm && g_scheduler_shm->is_leader != (int) is_leader) {
        g_scheduler_shm->is_leader = (int) is_leader;
        g_scheduler_shm->test_counter++;
    }
}

/* Cleanup shared memory on exit */
static inline void
fd_scheduler_shm_fini( void ) {
    if( g_scheduler_shm ) {
        if( g_tile_id >= 0 ) {
            g_scheduler_shm->tiles[g_tile_id].registered = 0;
        }
        munmap( g_scheduler_shm, sizeof(struct fd_scheduler_shm) );
        g_scheduler_shm = NULL;
    }
}

#endif /* HEADER_fd_src_disco_fd_scheduler_shm_h */
