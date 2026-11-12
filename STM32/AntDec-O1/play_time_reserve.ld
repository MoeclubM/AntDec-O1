/* Keep the generated memory layout; reserve the shared time/settings journal. */
ASSERT(LOADADDR(.tdata) + SIZEOF(.tdata) <= 0x0803F000,
       "Application overlaps device storage (last 4 KiB)");
