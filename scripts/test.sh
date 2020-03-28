#!/usr/bin/env bash

KHTTPD_MOD=khttpd.ko

if [ "$EUID" -eq 0 ]
  then echo "Don't run this script as root"
  exit
fi

# load kHTTPd
sudo rmmod -f khttpd 2>/dev/null
sleep 1
sudo insmod $KHTTPD_MOD

# run HTTP benchmarking
./htstress -n 100000 -c 1 -t 4 http://localhost:8081/

# epilogue
sudo rmmod khttpd
echo "Complete"
