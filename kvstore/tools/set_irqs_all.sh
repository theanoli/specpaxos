for replica in $replicas; do ssh $replica 'cd specpaxos/kvstore/tools; cat ~/specpaxos/scripts/passwd | sudo -S ./set_irqs.sh `ifconfig | grep 198.0.0. -B1 | head -n1 | cut -d: -f1` 1'; done
