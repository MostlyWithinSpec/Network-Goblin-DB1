#!/bin/bash
# Host-side screen simulator: renders UI states to .ppm files without hardware.
cd "$(dirname "$0")"
gcc -O1 -w -DSIM -I stub -I ../main sim.c ../main/gfx.c -o sim && ./sim | tail -1 && echo "rendered: $(ls *.ppm | tr '\n' ' ')"
