#include "../topo/fd_topo.h"
#include "../metrics/generated/fd_metrics_all.h"
#include "../metrics/fd_metrics.h"
#include "../topo/fd_cpu_topo.h"
#include "../../util/tile/fd_tile_private.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sys/mman.h>
#include "fd_mgr_tile.h"

/* Demo manual override: read from /dev/shm/fd_manual_leader */
static volatile int * g_manual_leader_mgr = NULL;
static void fd_manual_leader_mgr_init( void ) {
  if( g_manual_leader_mgr ) return;
  int fd = shm_open( "/fd_manual_leader", O_RDWR|O_CREAT, 0666 );
  FD_LOG_WARNING(( "fd_manual_leader_mgr_init: %d", fd ));
  if( fd<0 ) return;
  if( FD_UNLIKELY( ftruncate( fd, (off_t)sizeof(int) )<0 ) ) { close( fd ); return; }
  void * p = mmap( NULL, sizeof(int), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0 );
  close( fd );
  if( p==MAP_FAILED ) return;
  g_manual_leader_mgr = (volatile int *)p;
}
static int fd_manual_leader_mgr_read( void ) {
  if( FD_UNLIKELY( !g_manual_leader_mgr ) ) return -1;
  int v = *g_manual_leader_mgr;
  return v ? 1 : 0;
}

void update_is_leader( ulong new_is_leader ) {
  (void)new_is_leader;
}

static int
is_number_str( char const * s ) {
  for( ; *s; s++ ) if( *s<'0' || *s>'9' ) return 0;
  return 1;
}

static int
proc_cmdline_contains( ulong pid, char const * needle ) {
  char path[ PATH_MAX ];
  FD_TEST( fd_cstr_printf_check( path, PATH_MAX, NULL, "/proc/%lu/cmdline", pid ) );
  int fd = open( path, O_RDONLY );
  if( FD_UNLIKELY( fd<0 ) ) return 0;
  char buf[ 4096 ];
  ssize_t n = read( fd, buf, sizeof( buf ) );
  close( fd );
  if( FD_UNLIKELY( n<=0 ) ) return 0;
  for( ssize_t i=0; i<n && i<(ssize_t)sizeof(buf); i++ ) if( buf[i]=='\0' ) buf[i] = ' ';
  if( n >= (ssize_t)sizeof(buf) ) n = (ssize_t)sizeof(buf) - 1;
  buf[ n ] = '\0';
  return strstr( buf, needle )!=NULL;
}

static ulong
find_agave_pid( void ) {
  DIR * d = opendir( "/proc" );
  if( FD_UNLIKELY( !d ) ) return 0UL;
  struct dirent * ent;
  while( (ent = readdir( d )) ) {
    if( !is_number_str( ent->d_name ) ) continue;
    ulong pid = strtoul( ent->d_name, NULL, 10 );
    if( FD_UNLIKELY( !pid ) ) continue;
    if( proc_cmdline_contains( pid, "run-agave" ) ) {
      closedir( d );
      return pid;
    }
  }
  closedir( d );
  return 0UL;
}

static void
set_affinity_for_pid_threads( ulong pid, fd_cpuset_t const * mask ) {
  char path[ PATH_MAX ];
  FD_TEST( fd_cstr_printf_check( path, PATH_MAX, NULL, "/proc/%lu/task", pid ) );
  DIR * d = opendir( path );
  if( FD_UNLIKELY( !d ) ) return;
  struct dirent * ent;
  while( (ent = readdir( d )) ) {
    if( !is_number_str( ent->d_name ) ) continue;
    ulong tid = strtoul( ent->d_name, NULL, 10 );
    if( FD_UNLIKELY( !tid ) ) continue;
    if( FD_UNLIKELY( fd_cpuset_setaffinity( tid, mask ) ) ) {
      FD_LOG_WARNING(( "sched_setaffinity failed for tid %lu (%i-%s)", tid, errno, fd_io_strerror( errno ) ));
    }
  }
  closedir( d );
}

static void
set_tile_affinity( fd_topo_t * topo, fd_topo_tile_t * t, fd_cpuset_t const * mask, int desired_nice ) {
  volatile ulong * m = NULL;
  if( FD_LIKELY( topo->objs[ t->metrics_obj_id ].wksp_id<topo->wksp_cnt ) )
    m = fd_metrics_join( fd_topo_obj_laddr( topo, t->metrics_obj_id ) );
  if( FD_UNLIKELY( !m ) ) return;

  volatile ulong * mtile = fd_metrics_tile( (ulong *)m );
  ulong tid = mtile[ FD_METRICS_GAUGE_TILE_TID_OFF ];
  ulong pid = mtile[ FD_METRICS_GAUGE_TILE_PID_OFF ];
  if( FD_UNLIKELY( !tid && !pid ) ) return;

  if( FD_LIKELY( tid ) ) {
    if( FD_UNLIKELY( fd_cpuset_setaffinity( tid, mask ) ) ) {
      FD_LOG_WARNING(( "sched_setaffinity failed for tile %s:%lu tid %lu (%i-%s)", t->name, t->kind_id, tid, errno, fd_io_strerror( errno ) ));
    }
  } else {
    set_affinity_for_pid_threads( pid, mask );
  }

  if( desired_nice!=INT_MIN ) {
    if( FD_LIKELY( tid ) ) (void)setpriority( PRIO_PROCESS, (id_t)tid, desired_nice );
    else if( pid )         (void)setpriority( PRIO_PROCESS, (id_t)pid, desired_nice );
  }
}

static void
apply_fd_tile_affinities( fd_topo_t * topo, int desired_nice ) {
  for( ulong i=0UL; i<topo->tile_cnt; i++ ) {
    fd_topo_tile_t * t = &topo->tiles[ i ];
    if( FD_UNLIKELY( t->is_agave ) ) continue;
    if( FD_UNLIKELY( !strcmp( t->name, "mgr" ) ) ) continue;
    if( t->cpu_idx==ULONG_MAX || t->cpu_idx>=65535UL ) continue;

    FD_CPUSET_DECL( one );
    fd_cpuset_null( one );
    fd_cpuset_insert( one, t->cpu_idx );

    set_tile_affinity( topo, t, one, desired_nice );
  }
}

static void
apply_agave_affinity( fd_cpuset_t const * agave_mask ) {
  ulong agave_pid = find_agave_pid();
  if( FD_UNLIKELY( !agave_pid ) ) {
    FD_LOG_WARNING(( "manager: could not find agave process" ));
    return;
  }
  set_affinity_for_pid_threads( agave_pid, agave_mask );
}

static void
float_fd_tiles( fd_topo_t * topo, fd_cpuset_t const * all_cores, int nice_value ) {
  for( ulong i=0UL; i<topo->tile_cnt; i++ ) {
    fd_topo_tile_t * t = &topo->tiles[ i ];
    if( FD_UNLIKELY( t->is_agave ) ) continue;
    set_tile_affinity( topo, t, all_cores, nice_value );
  }
}

static void
manager_run( fd_topo_t * topo, fd_topo_tile_t * tile ) {
  (void)tile;
  FD_LOG_NOTICE(( "manager tile starting" ));

  /* Optional runtime toggle: set FD_MANAGER_ENABLED=0 to disable manager logic */
  int manager_enabled = 1;
  if( FD_UNLIKELY( !manager_enabled ) ) {
    FD_LOG_WARNING(( "manager: disabled via FD_MANAGER_ENABLED=0 (idling)" ));
    for( ;; ) sleep( 1000U * 1000U ); /* 1 second */
  }

  fd_topo_join_workspaces( topo, FD_SHMEM_JOIN_MODE_READ_ONLY );
  fd_topo_fill( topo );

  /* Demo: map manual override */
  fd_manual_leader_mgr_init();

  fd_topo_cpus_t cpus[1];
  fd_topo_cpus_init( cpus );

  /* Precompute masks once */
  FD_CPUSET_DECL( all_cores );
  fd_cpuset_null( all_cores );
  for( ulong i=0UL; i<cpus->cpu_cnt; i++ ) fd_cpuset_insert( all_cores, i );

  FD_CPUSET_DECL( fd_mask );
  fd_cpuset_null( fd_mask );
  for( ulong i=0UL; i<topo->tile_cnt; i++ ) {
    fd_topo_tile_t const * t = &topo->tiles[ i ];
    if( FD_UNLIKELY( t->is_agave ) ) continue;
    if( FD_UNLIKELY( !strcmp( t->name, "mgr" ) ) ) continue;
    if( t->cpu_idx!=ULONG_MAX && t->cpu_idx<65535UL ) fd_cpuset_insert( fd_mask, t->cpu_idx );
  }

  FD_CPUSET_DECL( agave_mask );
  fd_cpuset_null( agave_mask );
  for( ulong i=0UL; i<cpus->cpu_cnt; i++ ) {
    if( !fd_cpuset_test( fd_mask, i ) ) fd_cpuset_insert( agave_mask, i );
  }

  ulong last_leader_value = ULONG_MAX;
  for( ;; ) {
    /* Demo manual override (comment out to rely on PoH logic) */
    int manual = fd_manual_leader_mgr_read();
    FD_LOG_WARNING(( "manual =================: %d", manual ));
    ulong cur_is_leader = (ulong)((manual>=0) ? manual : 1);

    if( cur_is_leader != last_leader_value ) {
      FD_LOG_WARNING(( "REGIME SWITCH: is_leader = %lu", cur_is_leader ));
      last_leader_value = cur_is_leader;
      if( FD_LIKELY( cur_is_leader ) ) {
        apply_fd_tile_affinities( topo, -19 );
        apply_agave_affinity( agave_mask );
      } else {
        float_fd_tiles( topo, all_cores, 0 );
        apply_agave_affinity( all_cores );
      }
    }
  }
}

fd_topo_run_tile_t fd_tile_mgr = {
  .name              = "mgr",
  .rlimit_file_cnt   = 16UL,
  .populate_allowed_seccomp = NULL,
  .populate_allowed_fds     = NULL,
  .scratch_align     = NULL,
  .scratch_footprint = NULL,
  .privileged_init   = NULL,
  .unprivileged_init = NULL,
  .run               = manager_run,
};