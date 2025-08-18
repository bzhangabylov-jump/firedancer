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

static int is_leader = 0; /* test variable */

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
compute_fd_core_set( fd_topo_t const * topo, fd_cpuset_t * out_mask ) {
  FD_CPUSET_DECL( mask );
  fd_cpuset_null( mask );
  for( ulong i=0UL; i<topo->tile_cnt; i++ ) {
    fd_topo_tile_t const * t = &topo->tiles[ i ];
    if( FD_UNLIKELY( t->is_agave ) ) continue;
    if( FD_UNLIKELY( !strcmp( t->name, "mgr" ) ) ) continue;
    if( t->cpu_idx!=ULONG_MAX && t->cpu_idx<65535UL ) fd_cpuset_insert( mask, t->cpu_idx );
  }
  fd_memcpy( out_mask, mask, fd_cpuset_footprint() );
}

static void
compute_agave_core_set( fd_cpuset_t const * fd_mask, fd_cpuset_t * out_mask ) {
  fd_topo_cpus_t cpus[1];
  fd_topo_cpus_init( cpus );
  FD_CPUSET_DECL( mask );
  fd_cpuset_null( mask );
  for( ulong i=0UL; i<cpus->cpu_cnt; i++ ) {
    if( !fd_cpuset_test( fd_mask, i ) ) fd_cpuset_insert( mask, i );
  }
  fd_memcpy( out_mask, mask, fd_cpuset_footprint() );
}

static void
apply_fd_tile_affinities( fd_topo_t * topo ) {
  for( ulong i=0UL; i<topo->tile_cnt; i++ ) {
    fd_topo_tile_t * t = &topo->tiles[ i ];
    if( FD_UNLIKELY( t->is_agave ) ) continue;
    if( FD_UNLIKELY( !strcmp( t->name, "mgr" ) ) ) continue;
    if( t->cpu_idx==ULONG_MAX || t->cpu_idx>=65535UL ) continue;

    volatile ulong * m = NULL;
    if( FD_LIKELY( topo->objs[ t->metrics_obj_id ].wksp_id<topo->wksp_cnt ) )
      m = fd_metrics_join( fd_topo_obj_laddr( topo, t->metrics_obj_id ) );
    if( FD_UNLIKELY( !m ) ) continue;

    volatile ulong * mtile = fd_metrics_tile( (ulong *)m );
    ulong pid = mtile[ FD_METRICS_GAUGE_TILE_PID_OFF ];
    if( FD_UNLIKELY( !pid ) ) continue;

    FD_CPUSET_DECL( one );
    fd_cpuset_null( one );
    fd_cpuset_insert( one, t->cpu_idx );
    if( FD_UNLIKELY( fd_cpuset_setaffinity( pid, one ) ) ) {
      FD_LOG_WARNING(( "sched_setaffinity failed for tile %s:%lu pid %lu (%i-%s)", t->name, t->kind_id, pid, errno, fd_io_strerror( errno ) ));
    }
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
manager_run( fd_topo_t * topo, fd_topo_tile_t * tile ) {
  (void)tile;
  FD_LOG_NOTICE(( "manager tile starting" ));

  fd_topo_join_workspaces( topo, FD_SHMEM_JOIN_MODE_READ_ONLY );
  fd_topo_fill( topo );

  fd_topo_cpus_t cpus[1];
  fd_topo_cpus_init( cpus );
  FD_CPUSET_DECL( all_cores );
  fd_cpuset_null( all_cores );
  for( ulong i=0UL; i<cpus->cpu_cnt; i++ ) fd_cpuset_insert( all_cores, i );

  for( ;; ) {
    sleep( 5U ); /* 1 second */
    if( FD_LIKELY( is_leader ) ) {
      FD_CPUSET_DECL( fd_mask );
      compute_fd_core_set( topo, fd_mask );

      FD_CPUSET_DECL( agave_mask );
      compute_agave_core_set( fd_mask, agave_mask );

      apply_fd_tile_affinities( topo );
      apply_agave_affinity( agave_mask );
    } else {
      /* Float FD tiles, set nice to 0, Agave to all cores */
      for( ulong i=0UL; i<topo->tile_cnt; i++ ) {
        fd_topo_tile_t * t = &topo->tiles[ i ];
        if( FD_UNLIKELY( t->is_agave ) ) continue;

        volatile ulong * m = NULL;
        if( FD_LIKELY( topo->objs[ t->metrics_obj_id ].wksp_id<topo->wksp_cnt ) )
          m = fd_metrics_join( fd_topo_obj_laddr( topo, t->metrics_obj_id ) );
        if( FD_UNLIKELY( !m ) ) continue;

        volatile ulong * mtile = fd_metrics_tile( (ulong *)m );
        ulong pid = mtile[ FD_METRICS_GAUGE_TILE_PID_OFF ];
        if( FD_UNLIKELY( !pid ) ) continue;

        (void)fd_cpuset_setaffinity( pid, all_cores );
        (void)setpriority( PRIO_PROCESS, (id_t)pid, 0 );
      }

      ulong agave_pid = find_agave_pid();
      if( FD_LIKELY( agave_pid ) ) set_affinity_for_pid_threads( agave_pid, all_cores );
    }

    sleep( 1U ); /* 1 second */
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