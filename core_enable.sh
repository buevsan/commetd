ulimit -c unlimited
echo "core" > /proc/sys/kernel/core_pattern
echo "1" > /proc/sys/kernel/core_uses_pid
