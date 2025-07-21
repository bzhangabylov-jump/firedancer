#!/bin/bash
# Monitor sleep percentage for top CPU-consuming threads

declare -A last_exec_time
declare -A last_wall_time

# Optional: specify a process name to filter (e.g., "fddev")
PROCESS_FILTER="${1:-}"

get_top_threads() {
  if [ -n "$PROCESS_FILTER" ]; then
    # Filter by process name - get ALL matching threads
    ps -eLo pid,tid,pcpu,psr,comm,cmd --sort=-pcpu | grep "$PROCESS_FILTER" | grep -v grep
  else
    # Get ALL threads by CPU usage system-wide
    ps -eLo pid,tid,pcpu,psr,comm,cmd --sort=-pcpu
  fi
}

monitor_sleep() {
  local current_wall=$(date +%s%N)
  local output=""

  # Get top threads - now includes cmd
  while IFS= read -r line; do
    # Parse the ps output
    pid=$(echo "$line" | awk '{print $1}')
    tid=$(echo "$line" | awk '{print $2}')
    pcpu=$(echo "$line" | awk '{print $3}')
    psr=$(echo "$line" | awk '{print $4}')
    comm=$(echo "$line" | awk '{print $5}')
    # Get everything after the 5th field as cmd
    cmd=$(echo "$line" | awk '{for(i=6;i<=NF;i++) printf "%s ", $i; print ""}')

    # Skip header line or empty lines
    if [[ "$pid" == "PID" ]] || [[ -z "$pid" ]]; then continue; fi

    tid_dir="/proc/$pid/task/$tid"
    if [ -d "$tid_dir" ] && [ -f "$tid_dir/schedstat" ]; then
      # Try to get the actual thread name from /proc
      thread_name=$(cat "$tid_dir/comm" 2>/dev/null)

      # If thread name is generic (like fddev), try to identify it better
      if [[ "$thread_name" == "fddev" ]] || [[ -z "$thread_name" ]]; then
        # Check if it's the main process
        if [[ "$pid" == "$tid" ]]; then
          thread_name="fddev-main"
        else
          # Try to get the thread name from /proc/PID/task/TID/status
          name_from_status=$(grep "^Name:" "$tid_dir/status" 2>/dev/null | awk '{print $2}')
          if [[ -n "$name_from_status" ]] && [[ "$name_from_status" != "fddev" ]]; then
            thread_name="$name_from_status"
          else
            # Check if this is likely an Agave thread based on the parent cmd
            if [[ "$cmd" == *"agave"* ]] || [[ "$cmd" == *"solana"* ]]; then
              thread_name="agave-$tid"
            else
              thread_name="fddev-$tid"
            fi
          fi
        fi
      fi

      # Read schedstat: exec_runtime wait_runtime nr_switches
      read exec_time wait_time switches < "$tid_dir/schedstat" 2>/dev/null || continue

      # Calculate differences
      key="${pid}_${tid}"

      # Only calculate if we have previous values
      if [ -n "${last_exec_time[$key]}" ]; then
        exec_diff=$((exec_time - last_exec_time[$key]))
        wall_diff=$((current_wall - last_wall_time[$key]))

        if [ $wall_diff -gt 0 ]; then
          # CPU usage = (exec_time_diff / wall_time_diff) * 100
          cpu_pct=$(awk "BEGIN {printf \"%.1f\", ($exec_diff / $wall_diff) * 100}")
          sleep_pct=$(awk "BEGIN {printf \"%.1f\", 100 - $cpu_pct}")

          # Color code based on CPU usage (using integer comparison)
          # Convert to integer by removing decimal point
          cpu_pct_int=$(echo "$cpu_pct" | awk '{print int($1)}')
          if [ $cpu_pct_int -gt 80 ]; then
            color="\033[31m"  # Red for high CPU
          elif [ $cpu_pct_int -gt 50 ]; then
            color="\033[33m"  # Yellow for medium CPU
          else
            color="\033[32m"  # Green for low CPU
          fi
          reset="\033[0m"

          output+=$(printf "${color}%-15s${reset} PID:%-6s TID:%-6s CPU:%5s%% Sleep:%5s%% Core:%-2s" \
                 "$thread_name" "$pid" "$tid" "$cpu_pct" "$sleep_pct" "$psr")
          output+=$'\n'  # Only one newline at the end
        fi
      else
        # First iteration - just show thread info without percentages
        output+=$(printf "%-15s PID:%-6s TID:%-6s CPU:   --%% Sleep:   --%% Core:%-2s (collecting...)" \
               "$thread_name" "$pid" "$tid" "$psr")
        output+=$'\n'
      fi

      # Store current values for next iteration
      last_exec_time[$key]=$exec_time
      last_wall_time[$key]=$current_wall
    fi
  done < <(get_top_threads)

  # Sort by CPU percentage (reverse numeric on column 5)
  echo -e "$output" | grep -v "^$" | sort -k5 -rn
}

# Run continuously
echo "Monitoring sleep percentages for ${PROCESS_FILTER:-all processes}..."
echo "First reading will show as 'collecting...', actual percentages appear in 2 seconds..."
echo ""

iteration=0
while true; do
  clear
  echo "=== Thread Sleep Percentages ($(date '+%H:%M:%S')) ==="
  echo "Thread          PID    TID    CPU%   Sleep%  Core"
  echo "--------------------------------------------------------------"
  monitor_sleep
  echo ""
  echo "Color: Red = High CPU (>80%), Yellow = Medium (>50%), Green = Low (<50%)"
  echo "Press Ctrl+C to exit"

  ((iteration++))
  sleep 2
done