ASSERT(LOADADDR(.data) + SIZEOF(.data) <= 0xE9000,
       "SCReceiver image overlaps persistent mapping/bond pages")
