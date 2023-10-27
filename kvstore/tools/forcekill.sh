sudo kill -9 `ps aux | grep -v grep | grep specpaxos | awk '{print $2}'`
