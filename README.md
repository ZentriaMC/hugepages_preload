# hugepages_preload

This project was created to allow Java 14 ZGC to use 1 GiB hugepages, while JVM insisted only using 2 MiB ones.

Maybe it was because of not-so-standard system setup declaring additional hugepages pool and not changing the default one? Who knows.

It does not work reliably, causes JVM segfault sometimes.

Hopefully JVM will gain support for using larger hugepages and will decide max page size from the target path not generic sysconf/proc info. 
