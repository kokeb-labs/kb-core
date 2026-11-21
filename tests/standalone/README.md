# tests/standalone/

One configure per part: `-DKB_PARTS=utils`, `-DKB_PARTS=rtos`, `-DKB_PARTS=interfaces`, `-DKB_PARTS=handlers`.
Each must configure, build and test with only that part and its declared dependencies.
