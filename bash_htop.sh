#!/bin/bash

# Core monitoring tool for Firedancer - htop style

declare -A last_exec_time
declare -A last_wall_time
declare -A core_threads

monitor_cores() {
    local current_wall=$(date +%s%N)

    # Clear core data
    for i in {0..127}; do
        core_threads[$i]=""
    done

    # Get all threads from fddev or agave processes
    while IFS= read -r line; do
        pid=$(echo "$line" | awk '{print $1}')
        tid=$(echo "$line" | awk '{print $2}')
        pcpu=$(echo "$line" | awk '{print $3}')
        psr=$(echo "$line" | awk '{print $4}')
        comm=$(echo "$line" | awk '{print $5}')
        cmd=$(echo "$line" | awk '{for(i=6;i<=NF;i++) printf "%s ", $i; print ""}')

        # Skip header or invalid lines
        if [[ "$pid" == "PID" ]] || [[ -z "$pid" ]] || [[ "$psr" == "-" ]]; then
            continue
        fi

        # Get the thread name from /proc
        thread_name="$comm"
        tid_dir="/proc/$pid/task/$tid"
        if [ -f "$tid_dir/comm" ]; then
            thread_name=$(cat "$tid_dir/comm" 2>/dev/null || echo "$comm")
        fi

        # Get actual CPU usage
        if [ -f "$tid_dir/schedstat" ]; then
            read exec_time wait_time switches < "$tid_dir/schedstat" 2>/dev/null || continue

            key="${pid}_${tid}"
            cpu_pct="--.-"

            if [ -n "${last_exec_time[$key]}" ]; then
                exec_diff=$((exec_time - last_exec_time[$key]))
                wall_diff=$((current_wall - last_wall_time[$key]))

                if [ $wall_diff -gt 0 ]; then
                    cpu_pct=$(awk "BEGIN {printf \"%.1f\", ($exec_diff / $wall_diff) * 100}")
                fi
            fi

            # Store for next iteration
            last_exec_time[$key]=$exec_time
            last_wall_time[$key]=$current_wall

            # Add to core data - APPEND all threads instead of keeping highest
            # Store format using | as delimiter: thread_name|pid|tid|cpu_pct
            thread_info="$thread_name|$pid|$tid|$cpu_pct"

            if [[ -z "${core_threads[$psr]}" ]]; then
                core_threads[$psr]="$thread_info"
            else
                # Append with newline separator
                core_threads[$psr]="${core_threads[$psr]}\n$thread_info"
            fi
        fi

    # display all of fddev & agave
    # done < <(ps -eLo pid,tid,pcpu,psr,comm,cmd --sort=-pcpu | grep -E "(fddev)" | grep -v grep )
    # only display fddev tiles
    # done < <(ps -eLo pid,tid,pcpu,psr,comm,cmd --sort=-pcpu | grep -E "(fddev)" | grep -v grep | grep -E "(benchg:|benchs:|bencho:bank:|poh:|verify:|pack:|quic:|net:|sock:|xdp:|dedup:|netlnk:|resolv:|shred:|sign:|store:|metric:|plugin:|gui:|bundle:|cswtch)")
    done < <(ps -eLo pid,tid,pcpu,psr,comm,cmd --sort=-pcpu | grep -v grep | grep -E "(benchg:|benchs:|bencho:bank:|poh:|verify:|pack:|quic:|net:|sock:|xdp:|dedup:|netlnk:|resolv:|shred:|sign:|store:|metric:|plugin:|gui:|bundle:|cswtch)")
}

# Main loop
clear
echo "Monitoring Firedancer CPU cores... (2 sec intervals)"
echo ""

sleep 2

while true; do
    monitor_cores

    # Clear screen and reset cursor
    tput clear

    # Print header
    printf "%-7s %-20s %-15s %8s\n" "CORE" "THREAD" "PID:TID" "CPU%"
    printf "%-7s %-20s %-15s %8s\n" "----" "------" "-------" "----"

    # Print each core
    num_cpus=$(nproc)
    for ((i=0; i<num_cpus; i++)); do
        if [[ -n "${core_threads[$i]}" ]]; then
            # Sort threads by CPU usage and print them
            first_thread=true
            echo -e "${core_threads[$i]}" | sort -t'|' -k4 -nr | while IFS='|' read -r thread_name pid tid cpu_usage; do
                # Skip empty lines
                [[ -z "$thread_name" ]] && continue

                # Color based on CPU usage
                cpu_int=$(echo "$cpu_usage" | awk '{print int($1)}')
                if [ $cpu_int -gt 80 ]; then
                    color="\033[31m"  # Red
                elif [ $cpu_int -gt 40 ]; then
                    color="\033[33m"  # Yellow
                else
                    color="\033[32m"  # Green
                fi

                if $first_thread; then
                    printf "CORE %-2d ${color}%-20s %6s:%-7s %7s%%\033[0m\n" "$i" "$thread_name" "$pid" "$tid" "$cpu_usage"
                    first_thread=false
                else
                    # Indent subsequent threads
                    printf "        ${color}%-20s %6s:%-7s %7s%%\033[0m\n" "$thread_name" "$pid" "$tid" "$cpu_usage"
                fi
            done
        else
            # Fixed: Add spaces to ensure we overwrite any previous content
            printf "CORE %-2d \033[90m%-20s %-15s %7s%%\033[0m     \n" "$i" "[idle]" "" "0.0"
        fi
    done

    sleep 2
done