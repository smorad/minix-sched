#!/bin/bash
#cd /root/
#curl -kL https://d1b10bmlvqabco.cloudfront.net/attach/hpvmf4tj33s6oq/hm2ffxrreaxur/hr1mnbt11oad/minixR3.1.8tocommit300773cdca8cae8a8bb9a50d649e6b2a3d445b3a.patch.tar.gz | tar -xvf patch
#cd /usr/src
#patch -p1 < /root/patch
cd /usr/src/servers/sched
curl -kL https://raw.github.com/smorad/minix-sched/master/schedule.c | tee schedule.c
curl -kL https://raw.github.com/smorad/minix-sched/master/schedproc.h | tee schedproc.h
cd /usr/src/include/minix
curl -kL https://raw.github.com/smorad/minix-sched/master/config.h | tee config.h
cd /usr/src/tools
make -j4 && make install
