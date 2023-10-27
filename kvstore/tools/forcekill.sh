echo "splashed-grimace-grass3" | sudo -S kill -9 `ps aux | grep -v grep | grep specpaxos | awk '{print $2}'`
