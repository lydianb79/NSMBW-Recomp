#!/bin/bash
# Double-click to (re)build NSMBW Recomp and run a short diagnostic.
# Build output -> build_log.txt, diagnostic -> headless_log.txt + headless_frame.ppm
cd "$(dirname "$0")"
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
{
  echo "=== $(date) ==="; sw_vers; uname -m
  if [ -f build/gen/sources.cmake ] && [ -f build/image.bin ]; then
    cmake -S . -B build/cmake -DBUILD_DATA_DIR="$PWD/build" && cmake --build build/cmake -j "$(sysctl -n hw.ncpu)"
    rc=$?
    if [ $rc = 0 ]; then
      mkdir -p dist && rm -rf "dist/NSMBW Recomp.app" && cp -R "build/cmake/NSMBW Recomp.app" dist/
      # make Finder/Dock pick up a changed app icon (macOS caches icons per app)
      touch "dist/NSMBW Recomp.app"
      /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "dist/NSMBW Recomp.app" 2>/dev/null
    fi
  else
    bash scripts/build.sh "nsmb.rvz"; rc=$?
  fi
  echo "=== exit code $rc ==="
} 2>&1 | tee build_log.txt
echo; echo "Running a 30 second diagnostic (no window)..."
sysctl -n hw.ncpu machdep.cpu.brand_string > headless_log.txt 2>&1
RT_PRESS="a@6,a@10,a@14" ./build/cmake/nsmbw-headless build game -v -t 30 -o headless_frame.ppm >> headless_log.txt 2>&1
echo "exit $?" >> headless_log.txt
tail -5 headless_log.txt
echo "Done. You can close this window."
