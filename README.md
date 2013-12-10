# JSON crusher TrafficServer plugin

This plugin has been developped and tested on GNU/Linux Debian only.

It makes use of json-c, which is available on Debian Wheezy from backport repository

    deb http://ftp.fr.debian.org/debian/ wheezy-backports main non-free contrib
    deb-src http://ftp.fr.debian.org/debian/ wheezy-backports main non-free contrib

## Introduction

Some JSON APIs do not always optimize JSON response. Sometime, content is returned
formatted and indented in a human-readable way. That's great for debugging, not for
production.

When TrafficServer handle a request, it will check `HTTP Status` and `content-type`.
If `content-type` is set to `application/json` and `HTTP Status code` is 200, then
it'll try to load Json body into memory and dump it back without any spaces and/or
new line.

A better solution is to use `gzip` compression, but this comes with a price: cache
invalidation in order to load compressed content. Thus, « crushing » JSON
representents a cheap optimization and bandwith spare with little efforts.

## Build

1. install build dependencies (I assume you already have a build environment)

    apt-get install libjson-c-dev # from Wheezy backport repo

2. You have to build trafficserver so that headers files will be available. Build can
   be done either from source, or you need to install `trafficserver-dev` package.

3. Finally build JCruncher.

    #If you build ATS from source
    tsxs -o jcrusher.so -c jcrusher-transform.c -I<PATH_TO_ATS_BUILD>/lib/ts -ljson-c -Ljson-c

    #If you installed trafficserver-dev package
    tsxs -o jcrusher.so -c jcrusher-transform.c -ljson-c -Ljson-c

## Install

1. copy `jcrusher.so` to your ATS server into `/usr/lib/trafficserver/modules/`

2. install libjson-c on your ATS server

    apt-get install libjson-c2 # from Wheezy backport repo

2. add `jcrusher.so` to `/etc/trafficserver/plugin.config`

3. restart TrafficServer
