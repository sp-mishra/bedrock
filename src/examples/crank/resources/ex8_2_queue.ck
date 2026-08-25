package ex8_2_queue

type Queue[T] = struct {
    items: [1024]T
    front: UInt64
    back: UInt64
    size: UInt64
}

fn Main() -> Int32 {
    var q: Queue[Int32] = Queue { items: [], front: 0, back: 0, size: 0 }
    return 1
}
