#!/bin/bash
cd /usr/src/servers/sched
curl -kL https://raw.github.com/smorad/minix-sched/master/schedule.c | tee schedule.c
curl -kL https://raw.github.com/smorad/minix-sched/master/schedproc.h | tee schedproc.h
cd /usr/src/include/minix
curl -kL https://raw.github.com/smorad/minix-sched/master/config.h | tee config.h
cd /usr/src/tools
make -j4 && make install
