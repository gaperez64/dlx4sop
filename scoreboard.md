# SOP Solver Scoreboard

_Generated 2026-06-17 11:59 UTC_

## Local sop-solve backends — geomean wall time by tier

### Tier 1–8   (n=6 instances, best=treewidth 0.398 ms)

```
components                     █                                           0.434 ms   1.09x
brute-force                    █                                           0.433 ms   1.09x
branch (native rw)             █                                           0.422 ms   1.06x
branch (from-tw)               █                                            0.44 ms   1.11x
treewidth                      █                                           0.398 ms   1.00x
rankwidth                      █                                           0.424 ms   1.07x
```

### Tier 9–16   (n=4 instances, best=treewidth 0.54 ms)

```
components                     ██████                                       1.42 ms   2.64x
brute-force                    ██████                                       1.52 ms   2.81x
branch (native rw)             ██                                          0.745 ms   1.38x
branch (from-tw)               ██                                          0.677 ms   1.26x
treewidth                      █                                            0.54 ms   1.00x
rankwidth                      █                                           0.583 ms   1.08x
```

### Tier 17–32   (n=4 instances, best=treewidth 0.63 ms)

```
components                     ████████████████████████                     37.1 ms  58.95x
brute-force                    ████████████████████████████                 71.9 ms  114.04x
branch (native rw)             ███                                         0.921 ms   1.46x  (error 2)
branch (from-tw)               █                                           0.737 ms   1.17x  (error 2)
treewidth                      █                                            0.63 ms   1.00x
rankwidth                      ██████                                        1.6 ms   2.53x
```

### Tier 33–64   (n=3 instances, best=treewidth 0.814 ms)

```
branch (native rw)             —                                        n/a            (error 3)
branch (from-tw)               —                                        n/a            (error 3)
treewidth                      █                                           0.814 ms   1.00x  (error 1)
rankwidth                      ████                                         1.57 ms   1.92x  (timeout 1)
```

**Local backend winner by tier:**
```
1–8          treewidth
9–16         treewidth
17–32        treewidth
33–64        treewidth
```

## External comparisons — geomean wall time by source/tier

### Source: local-corpus-ganak

Tier 1–8, n=6

```
ganak (ganak-amp-and, ganak-amp-soft)  █                                            14.9 ms   1.00x
```

Tier 9–16, n=4

```
ganak (ganak-amp-and, ganak-amp-soft)  █                                            30.7 ms   1.00x
```

Tier 17–32, n=4

```
ganak (ganak-amp-and, ganak-amp-soft)  █                                            54.7 ms   1.00x
```

Tier 33–64, n=3

```
ganak (ganak-amp-and, ganak-amp-soft)  █                                              90 ms   1.00x
```

