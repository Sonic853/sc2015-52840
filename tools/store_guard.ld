ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0xEB000,
       "SCReceiver image overlaps its two persistent bond pages")
